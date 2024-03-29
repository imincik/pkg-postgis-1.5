/**********************************************************************
 * $Id: lwgeom_estimate.c 10579 2012-10-29 18:00:32Z pramsey $
 *
 * PostGIS - Spatial Types for PostgreSQL
 * http://postgis.refractions.net
 * Copyright 2001-2006 Refractions Research Inc.
 *
 * This is free software; you can redistribute and/or modify it under
 * the terms of the GNU General Public Licence. See the COPYING file.
 *
 **********************************************************************/

#include "postgres.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "commands/vacuum.h"
#include "nodes/relation.h"
#include "parser/parsetree.h"
#include "utils/array.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

#include "liblwgeom.h"
#include "lwgeom_pg.h"

#if POSTGIS_PGSQL_VERSION >= 92
#include "utils/rel.h"
#endif

#include <math.h>
#if HAVE_IEEEFP_H
#include <ieeefp.h>
#endif
#include <float.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <ctype.h>

/**
 * 	Assign a number to the postgis statistics kind
 *
 * 	tgl suggested:
 *
 * 	1-100:	reserved for assignment by the core Postgres project
 * 	100-199: reserved for assignment by PostGIS
 * 	200-9999: reserved for other globally-known stats kinds
 * 	10000-32767: reserved for private site-local use
 *
 */
#define STATISTIC_KIND_GEOMETRY 100

/*
 * Define this if you want to use standard deviation based
 * histogram extent computation. If you do, you can also
 * tweak the deviation factor used in computation with
 * SDFACTOR.
 */
#define USE_STANDARD_DEVIATION 1
#define SDFACTOR 3.25

typedef struct GEOM_STATS_T
{
	/* cols * rows = total boxes in grid */
	float4 cols;
	float4 rows;

	/* average bounding box area of not-null features */
	float4 avgFeatureArea;

	/*
	 * average number of histogram cells
	 * covered by the sample not-null features
	 */
	float4 avgFeatureCells;

	/* BOX of area */
	float4 xmin,ymin, xmax, ymax;

	/*
	 * variable length # of floats for histogram
	 */
	float4 value[1];
}
GEOM_STATS;

static float8 estimate_selectivity(BOX2DFLOAT4 *box, GEOM_STATS *geomstats);


#define SHOW_DIGS_DOUBLE 15
#define MAX_DIGS_DOUBLE (SHOW_DIGS_DOUBLE + 6 + 1 + 3 +1)

/**
 * Default geometry selectivity factor
 */
#define DEFAULT_GEOMETRY_SEL 0.000005

/**
 * Default geometry join selectivity factor
 */
#define DEFAULT_GEOMETRY_JOINSEL 0.000005

/**
 * Define this to actually DO join selectivity
 * (as contrary to just return the default JOINSEL value)
 * Note that this is only possible when compiling postgis
 * against pgsql >= 800
 */
#define REALLY_DO_JOINSEL 1

Datum LWGEOM_gist_sel(PG_FUNCTION_ARGS);
Datum LWGEOM_gist_joinsel(PG_FUNCTION_ARGS);
Datum LWGEOM_estimated_extent(PG_FUNCTION_ARGS);
Datum LWGEOM_analyze(PG_FUNCTION_ARGS);


#if ! REALLY_DO_JOINSEL
/**
 * JOIN selectivity in the GiST && operator
 * for all PG versions
 */
PG_FUNCTION_INFO_V1(LWGEOM_gist_joinsel);
Datum LWGEOM_gist_joinsel(PG_FUNCTION_ARGS)
{
	POSTGIS_DEBUGF(2, "LWGEOM_gist_joinsel called (returning %f)",
	               DEFAULT_GEOMETRY_JOINSEL);

	PG_RETURN_FLOAT8(DEFAULT_GEOMETRY_JOINSEL);
}

#else /* REALLY_DO_JOINSEL */

int calculate_column_intersection(BOX2DFLOAT4 *search_box, GEOM_STATS *geomstats1, GEOM_STATS *geomstats2);

int
calculate_column_intersection(BOX2DFLOAT4 *search_box, GEOM_STATS *geomstats1, GEOM_STATS *geomstats2)
{
	/**
	* Calculate the intersection of two columns from their geomstats extents - return true
	* if a valid intersection was found, false if there is no overlap
	*/

	float8 i_xmin = LW_MAX(geomstats1->xmin, geomstats2->xmin);
	float8 i_ymin = LW_MAX(geomstats1->ymin, geomstats2->ymin);
	float8 i_xmax = LW_MIN(geomstats1->xmax, geomstats2->xmax);
	float8 i_ymax = LW_MIN(geomstats1->ymax, geomstats2->ymax);

	/* If the rectangles don't intersect, return false */
	if (i_xmin > i_xmax || i_ymin > i_ymax)
		return 0;

	/* Otherwise return the rectangle in search_box */
	search_box->xmin = i_xmin;
	search_box->ymin = i_ymin;
	search_box->xmax = i_xmax;
	search_box->ymax = i_ymax;

	return -1;
}

/**
* JOIN selectivity in the GiST && operator
* for all PG versions
*/
PG_FUNCTION_INFO_V1(LWGEOM_gist_joinsel);
Datum LWGEOM_gist_joinsel(PG_FUNCTION_ARGS)
{
	PlannerInfo *root = (PlannerInfo *) PG_GETARG_POINTER(0);

	/* Oid operator = PG_GETARG_OID(1); */
	List *args = (List *) PG_GETARG_POINTER(2);
	JoinType jointype = (JoinType) PG_GETARG_INT16(3);

	Node *arg1, *arg2;
	Var *var1, *var2;
	Oid relid1, relid2;

	HeapTuple stats1_tuple, stats2_tuple, class_tuple;
	GEOM_STATS *geomstats1, *geomstats2;
	/*
	* These are to avoid casting the corresponding
	* "type-punned" pointers, which would break
	* "strict-aliasing rules".
	*/
	GEOM_STATS **gs1ptr=&geomstats1, **gs2ptr=&geomstats2;
	int geomstats1_nvalues = 0, geomstats2_nvalues = 0;
	float8 selectivity1 = 0.0, selectivity2 = 0.0;
	float4 num1_tuples = 0.0, num2_tuples = 0.0;
	float4 total_tuples = 0.0, rows_returned = 0.0;
	BOX2DFLOAT4 search_box;


	/**
	* Join selectivity algorithm. To calculation the selectivity we
	* calculate the intersection of the two column sample extents,
	* sum the results, and then multiply by two since for each
	* geometry in col 1 that intersects a geometry in col 2, the same
	* will also be true.
	*/


	POSTGIS_DEBUGF(3, "LWGEOM_gist_joinsel called with jointype %d", jointype);

	/*
	* We'll only respond to an inner join/unknown context join
	*/
	if (jointype != JOIN_INNER)
	{
		elog(NOTICE, "LWGEOM_gist_joinsel called with incorrect join type");
		PG_RETURN_FLOAT8(DEFAULT_GEOMETRY_JOINSEL);
	}

	/*
	* Determine the oids of the geometry columns we are working with
	*/
	arg1 = (Node *) linitial(args);
	arg2 = (Node *) lsecond(args);

	if (!IsA(arg1, Var) || !IsA(arg2, Var))
	{
		elog(DEBUG1, "LWGEOM_gist_joinsel called with arguments that are not column references");
		PG_RETURN_FLOAT8(DEFAULT_GEOMETRY_JOINSEL);
	}

	var1 = (Var *)arg1;
	var2 = (Var *)arg2;

	relid1 = getrelid(var1->varno, root->parse->rtable);
	relid2 = getrelid(var2->varno, root->parse->rtable);

	POSTGIS_DEBUGF(3, "Working with relations oids: %d %d", relid1, relid2);

	/* Read the stats tuple from the first column */
	stats1_tuple = SearchSysCache(STATRELATT, ObjectIdGetDatum(relid1), Int16GetDatum(var1->varattno), 0, 0);
	if ( ! stats1_tuple )
	{
		POSTGIS_DEBUG(3, " No statistics, returning default geometry join selectivity");

		PG_RETURN_FLOAT8(DEFAULT_GEOMETRY_JOINSEL);
	}



	if ( ! get_attstatsslot(stats1_tuple, 0, 0, STATISTIC_KIND_GEOMETRY, InvalidOid, NULL, NULL,
#if POSTGIS_PGSQL_VERSION >= 85
	                        NULL,
#endif
	                        (float4 **)gs1ptr, &geomstats1_nvalues) )
	{
		POSTGIS_DEBUG(3, " STATISTIC_KIND_GEOMETRY stats not found - returning default geometry join selectivity");

		ReleaseSysCache(stats1_tuple);
		PG_RETURN_FLOAT8(DEFAULT_GEOMETRY_JOINSEL);
	}


	/* Read the stats tuple from the second column */
	stats2_tuple = SearchSysCache(STATRELATT, ObjectIdGetDatum(relid2), Int16GetDatum(var2->varattno), 0, 0);
	if ( ! stats2_tuple )
	{
		POSTGIS_DEBUG(3, " No statistics, returning default geometry join selectivity");

		free_attstatsslot(0, NULL, 0, (float *)geomstats1,
		                  geomstats1_nvalues);
		ReleaseSysCache(stats1_tuple);
		PG_RETURN_FLOAT8(DEFAULT_GEOMETRY_JOINSEL);
	}


	if ( ! get_attstatsslot(stats2_tuple, 0, 0, STATISTIC_KIND_GEOMETRY, InvalidOid, NULL, NULL,
#if POSTGIS_PGSQL_VERSION >= 85
	                        NULL,
#endif
	                        (float4 **)gs2ptr, &geomstats2_nvalues) )
	{
		POSTGIS_DEBUG(3, " STATISTIC_KIND_GEOMETRY stats not found - returning default geometry join selectivity");

		free_attstatsslot(0, NULL, 0, (float *)geomstats1,
		                  geomstats1_nvalues);
		ReleaseSysCache(stats2_tuple);
		ReleaseSysCache(stats1_tuple);
		PG_RETURN_FLOAT8(DEFAULT_GEOMETRY_JOINSEL);
	}


	/**
	* Setup the search box - this is the intersection of the two column
	* extents.
	*/
	calculate_column_intersection(&search_box, geomstats1, geomstats2);

	POSTGIS_DEBUGF(3, " -- geomstats1 box: %.15g %.15g, %.15g %.15g",geomstats1->xmin,geomstats1->ymin,geomstats1->xmax,geomstats1->ymax);
	POSTGIS_DEBUGF(3, " -- geomstats2 box: %.15g %.15g, %.15g %.15g",geomstats2->xmin,geomstats2->ymin,geomstats2->xmax,geomstats2->ymax);
	POSTGIS_DEBUGF(3, " -- calculated intersection box is : %.15g %.15g, %.15g %.15g",search_box.xmin,search_box.ymin,search_box.xmax,search_box.ymax);


	/* Do the selectivity */
	selectivity1 = estimate_selectivity(&search_box, geomstats1);
	selectivity2 = estimate_selectivity(&search_box, geomstats2);

	POSTGIS_DEBUGF(3, "selectivity1: %.15g   selectivity2: %.15g", selectivity1, selectivity2);

	/* Free the statistic tuples */
	free_attstatsslot(0, NULL, 0, (float *)geomstats1, geomstats1_nvalues);
	ReleaseSysCache(stats1_tuple);

	free_attstatsslot(0, NULL, 0, (float *)geomstats2, geomstats2_nvalues);
	ReleaseSysCache(stats2_tuple);

	/*
	* OK, so before we calculate the join selectivity we also need to
	* know the number of tuples in each of the columns since
	* estimate_selectivity returns the number of estimated tuples
	* divided by the total number of tuples - hence we need to
	* multiply out the returned selectivity by the total number of rows.
	*/
	class_tuple = SearchSysCache(RELOID, ObjectIdGetDatum(relid1),
	                             0, 0, 0);

	if (HeapTupleIsValid(class_tuple))
	{
		Form_pg_class reltup = (Form_pg_class) GETSTRUCT(class_tuple);
		num1_tuples = reltup->reltuples;
	}

	ReleaseSysCache(class_tuple);


	class_tuple = SearchSysCache(RELOID, ObjectIdGetDatum(relid2),
	                             0, 0, 0);

	if (HeapTupleIsValid(class_tuple))
	{
		Form_pg_class reltup = (Form_pg_class) GETSTRUCT(class_tuple);
		num2_tuples = reltup->reltuples;
	}

	ReleaseSysCache(class_tuple);


	/*
	* Finally calculate the estimate of the number of rows returned
	*
	*    = 2 * (nrows from col1 + nrows from col2) /
	*	total nrows in col1 x total nrows in col2
	*
	* The factor of 2 accounts for the fact that for each tuple in
	* col 1 matching col 2,
	* there will be another match in col 2 matching col 1
	*/

	total_tuples = num1_tuples * num2_tuples;
	rows_returned = 2 * ((num1_tuples * selectivity1) +
	                     (num2_tuples * selectivity2));

	POSTGIS_DEBUGF(3, "Rows from rel1: %f", num1_tuples * selectivity1);
	POSTGIS_DEBUGF(3, "Rows from rel2: %f", num2_tuples * selectivity2);
	POSTGIS_DEBUGF(3, "Estimated rows returned: %f", rows_returned);

	/*
	* One (or both) tuple count is zero...
	* We return default selectivity estimate.
	* We could probably attempt at an estimate
	* w/out looking at tables tuple count, with
	* a function of selectivity1, selectivity2.
	*/
	if ( ! total_tuples )
	{
		POSTGIS_DEBUG(3, "Total tuples == 0, returning default join selectivity");

		PG_RETURN_FLOAT8(DEFAULT_GEOMETRY_JOINSEL);
	}

	if ( rows_returned > total_tuples )
		PG_RETURN_FLOAT8(1.0);

	PG_RETURN_FLOAT8(rows_returned / total_tuples);
}

#endif /* REALLY_DO_JOINSEL */

/**************************** FROM POSTGIS ****************/


/**
 * This function returns an estimate of the selectivity
 * of a search_box looking at data in the GEOM_STATS
 * structure.
 * */
/**
* TODO: handle box dimension collapses (probably should be handled
* by the statistic generator, avoiding GEOM_STATS with collapsed
* dimensions)
*/
static float8
estimate_selectivity(BOX2DFLOAT4 *box, GEOM_STATS *geomstats)
{
	int x, y;
	int x_idx_min, x_idx_max, y_idx_min, y_idx_max;
	double intersect_x, intersect_y, AOI;
	double cell_area, box_area;
	double geow, geoh; /* width and height of histogram */
	int histocols, historows; /* histogram grid size */
	double value;
	float overlapping_cells;
	float avg_feat_cells;
	double gain;
	float8 selectivity;


	/*
	 * Search box completely miss histogram extent
	 */
	if ( box->xmax < geomstats->xmin ||
	        box->xmin > geomstats->xmax ||
	        box->ymax < geomstats->ymin ||
	        box->ymin > geomstats->ymax )
	{
		POSTGIS_DEBUG(3, " search_box does not overlaps histogram, returning 0");

		return 0.0;
	}

	/*
	 * Search box completely contains histogram extent
	 */
	if ( box->xmax >= geomstats->xmax &&
	        box->xmin <= geomstats->xmin &&
	        box->ymax >= geomstats->ymax &&
	        box->ymin <= geomstats->ymin )
	{
		POSTGIS_DEBUG(3, " search_box contains histogram, returning 1");

		return 1.0;
	}

	geow = geomstats->xmax-geomstats->xmin;
	geoh = geomstats->ymax-geomstats->ymin;

	histocols = geomstats->cols;
	historows = geomstats->rows;

	POSTGIS_DEBUGF(3, " histogram has %d cols, %d rows", histocols, historows);
	POSTGIS_DEBUGF(3, " histogram geosize is %fx%f", geow, geoh);

	cell_area = (geow*geoh) / (histocols*historows);
	box_area = (box->xmax-box->xmin)*(box->ymax-box->ymin);
	value = 0;

	/* Find first overlapping column */
	x_idx_min = (box->xmin-geomstats->xmin) / geow * histocols;
	if (x_idx_min < 0)
	{
		POSTGIS_DEBUGF(3, " search_box overlaps %d columns on the left of histogram grid", -x_idx_min);

		/* should increment the value somehow */
		x_idx_min = 0;
	}
	if (x_idx_min >= histocols)
	{
		POSTGIS_DEBUGF(3, " search_box overlaps %d columns on the right of histogram grid", x_idx_min-histocols+1);

		/* should increment the value somehow */
		x_idx_min = histocols-1;
	}

	/* Find first overlapping row */
	y_idx_min = (box->ymin-geomstats->ymin) / geoh * historows;
	if (y_idx_min <0)
	{
		POSTGIS_DEBUGF(3, " search_box overlaps %d columns on the bottom of histogram grid", -y_idx_min);

		/* should increment the value somehow */
		y_idx_min = 0;
	}
	if (y_idx_min >= historows)
	{
		POSTGIS_DEBUGF(3, " search_box overlaps %d columns on the top of histogram grid", y_idx_min-historows+1);

		/* should increment the value somehow */
		y_idx_min = historows-1;
	}

	/* Find last overlapping column */
	x_idx_max = (box->xmax-geomstats->xmin) / geow * histocols;
	if (x_idx_max <0)
	{
		/* should increment the value somehow */
		x_idx_max = 0;
	}
	if (x_idx_max >= histocols )
	{
		/* should increment the value somehow */
		x_idx_max = histocols-1;
	}

	/* Find last overlapping row */
	y_idx_max = (box->ymax-geomstats->ymin) / geoh * historows;
	if (y_idx_max <0)
	{
		/* should increment the value somehow */
		y_idx_max = 0;
	}
	if (y_idx_max >= historows)
	{
		/* should increment the value somehow */
		y_idx_max = historows-1;
	}

	/*
	 * the {x,y}_idx_{min,max}
	 * define the grid squares that the box intersects
	 */
	for (y=y_idx_min; y<=y_idx_max; y++)
	{
		for (x=x_idx_min; x<=x_idx_max; x++)
		{
			double val;
			double gain;

			val = geomstats->value[x+y*histocols];

			/*
			 * Of the cell value we get
			 * only the overlap fraction.
			 */

			intersect_x = LW_MIN(box->xmax, geomstats->xmin + (x+1) * geow / histocols) - LW_MAX(box->xmin, geomstats->xmin + x * geow / histocols );
			intersect_y = LW_MIN(box->ymax, geomstats->ymin + (y+1) * geoh / historows) - LW_MAX(box->ymin, geomstats->ymin+ y * geoh / historows) ;

			AOI = intersect_x*intersect_y;
			gain = AOI/cell_area;

			POSTGIS_DEBUGF(4, " [%d,%d] cell val %.15f",
			               x, y, val);
			POSTGIS_DEBUGF(4, " [%d,%d] AOI %.15f",
			               x, y, AOI);
			POSTGIS_DEBUGF(4, " [%d,%d] gain %.15f",
			               x, y, gain);

			val *= gain;

			POSTGIS_DEBUGF(4, " [%d,%d] adding %.15f to value",
			               x, y, val);

			value += val;
		}
	}


	/*
	 * If the search_box is a point, it will
	 * overlap a single cell and thus get
	 * it's value, which is the fraction of
	 * samples (we can presume of row set also)
	 * which bumped to that cell.
	 *
	 * If the table features are points, each
	 * of them will overlap a single histogram cell.
	 * Our search_box value would then be correctly
	 * computed as the sum of the bumped cells values.
	 *
	 * If both our search_box AND the sample features
	 * overlap more then a single histogram cell we
	 * need to consider the fact that our sum computation
	 * will have many duplicated included. E.g. each
	 * single sample feature would have contributed to
	 * raise the search_box value by as many times as
	 * many cells in the histogram are commonly overlapped
	 * by both searc_box and feature. We should then
	 * divide our value by the number of cells in the virtual
	 * 'intersection' between average feature cell occupation
	 * and occupation of the search_box. This is as
	 * fuzzy as you understand it :)
	 *
	 * Consistency check: whenever the number of cells is
	 * one of whichever part (search_box_occupation,
	 * avg_feature_occupation) the 'intersection' must be 1.
	 * If sounds that our 'intersaction' is actually the
	 * minimun number between search_box_occupation and
	 * avg_feat_occupation.
	 *
	 */
	overlapping_cells = (x_idx_max-x_idx_min+1) *
	                    (y_idx_max-y_idx_min+1);
	avg_feat_cells = geomstats->avgFeatureCells;

	POSTGIS_DEBUGF(3, " search_box overlaps %f cells", overlapping_cells);
	POSTGIS_DEBUGF(3, " avg feat overlaps %f cells", avg_feat_cells);

	if ( ! overlapping_cells )
	{
		POSTGIS_DEBUG(3, " no overlapping cells, returning 0.0");

		return 0.0;
	}

	gain = 1/LW_MIN(overlapping_cells, avg_feat_cells);
	selectivity = value*gain;

	POSTGIS_DEBUGF(3, " SUM(ov_histo_cells)=%f", value);
	POSTGIS_DEBUGF(3, " gain=%f", gain);
	POSTGIS_DEBUGF(3, " selectivity=%f", selectivity);

	/* prevent rounding overflows */
	if (selectivity > 1.0) selectivity = 1.0;
	else if (selectivity < 0) selectivity = 0.0;

	return selectivity;
}

/**
 * This function should return an estimation of the number of
 * rows returned by a query involving an overlap check
 * ( it's the restrict function for the && operator )
 *
 * It can make use (if available) of the statistics collected
 * by the geometry analyzer function.
 *
 * Note that the good work is done by estimate_selectivity() above.
 * This function just tries to find the search_box, loads the statistics
 * and invoke the work-horse.
 *
 * This is the one used for PG version >= 7.5
 *
 */
PG_FUNCTION_INFO_V1(LWGEOM_gist_sel);
Datum LWGEOM_gist_sel(PG_FUNCTION_ARGS)
{
	PlannerInfo *root = (PlannerInfo *) PG_GETARG_POINTER(0);

	/* Oid operator = PG_GETARG_OID(1); */
	List *args = (List *) PG_GETARG_POINTER(2);
	/* int varRelid = PG_GETARG_INT32(3); */
	Oid relid;
	HeapTuple stats_tuple;
	GEOM_STATS *geomstats;
	/*
	 * This is to avoid casting the corresponding
	 * "type-punned" pointer, which would break
	 * "strict-aliasing rules".
	 */
	GEOM_STATS **gsptr=&geomstats;
	int geomstats_nvalues=0;
	Node *other;
	Var *self;
	uchar *in;
	BOX2DFLOAT4 search_box;
	float8 selectivity=0;

	POSTGIS_DEBUG(2, "LWGEOM_gist_sel called");

	/* Fail if not a binary opclause (probably shouldn't happen) */
	if (list_length(args) != 2)
	{
		POSTGIS_DEBUG(3, "LWGEOM_gist_sel: not a binary opclause");

		PG_RETURN_FLOAT8(DEFAULT_GEOMETRY_SEL);
	}


	/*
	 * Find the constant part
	 */
	other = (Node *) linitial(args);
	if ( ! IsA(other, Const) )
	{
		self = (Var *)other;
		other = (Node *) lsecond(args);
	}
	else
	{
		self = (Var *) lsecond(args);
	}

	if ( ! IsA(other, Const) )
	{
		POSTGIS_DEBUG(3, " no constant arguments - returning default selectivity");

		PG_RETURN_FLOAT8(DEFAULT_GEOMETRY_SEL);
	}

	/*
	 * We are working on two constants..
	 * TODO: check if expression is true,
	 *       returned set would be either
	 *       the whole or none.
	 */
	if ( ! IsA(self, Var) )
	{
		POSTGIS_DEBUG(3, " no variable argument ? - returning default selectivity");

		PG_RETURN_FLOAT8(DEFAULT_GEOMETRY_SEL);
	}

	/*
	 * Convert the constant to a BOX
	 */

	in = (uchar *)PG_DETOAST_DATUM( ((Const*)other)->constvalue );
	if ( ! getbox2d_p(in+4, &search_box) )
	{
		POSTGIS_DEBUG(3, "search box is EMPTY");

		PG_RETURN_FLOAT8(0.0);
	}

	POSTGIS_DEBUGF(4, " requested search box is : %.15g %.15g, %.15g %.15g",search_box.xmin,search_box.ymin,search_box.xmax,search_box.ymax);

	/*
	 * Get pg_statistic row
	 */

	relid = getrelid(self->varno, root->parse->rtable);

	stats_tuple = SearchSysCache(STATRELATT, ObjectIdGetDatum(relid), Int16GetDatum(self->varattno), 0, 0);
	if ( ! stats_tuple )
	{
		POSTGIS_DEBUG(3, " No statistics, returning default estimate");

		PG_RETURN_FLOAT8(DEFAULT_GEOMETRY_SEL);
	}


	if ( ! get_attstatsslot(stats_tuple, 0, 0, STATISTIC_KIND_GEOMETRY, InvalidOid, NULL, NULL,
#if POSTGIS_PGSQL_VERSION >= 85
	                        NULL,
#endif
	                        (float4 **)gsptr, &geomstats_nvalues) )
	{
		POSTGIS_DEBUG(3, " STATISTIC_KIND_GEOMETRY stats not found - returning default geometry selectivity");

		ReleaseSysCache(stats_tuple);
		PG_RETURN_FLOAT8(DEFAULT_GEOMETRY_SEL);
	}

	POSTGIS_DEBUGF(4, " %d read from stats", geomstats_nvalues);

	POSTGIS_DEBUGF(4, " histo: xmin,ymin: %f,%f",
	               geomstats->xmin, geomstats->ymin);
	POSTGIS_DEBUGF(4, " histo: xmax,ymax: %f,%f",
	               geomstats->xmax, geomstats->ymax);
	POSTGIS_DEBUGF(4, " histo: cols: %f", geomstats->rows);
	POSTGIS_DEBUGF(4, " histo: rows: %f", geomstats->cols);
	POSTGIS_DEBUGF(4, " histo: avgFeatureArea: %f", geomstats->avgFeatureArea);
	POSTGIS_DEBUGF(4, " histo: avgFeatureCells: %f", geomstats->avgFeatureCells);

	/*
	 * Do the estimation
	 */
	selectivity = estimate_selectivity(&search_box, geomstats);


	POSTGIS_DEBUGF(3, " returning computed value: %f", selectivity);

	free_attstatsslot(0, NULL, 0, (float *)geomstats, geomstats_nvalues);
	ReleaseSysCache(stats_tuple);
	PG_RETURN_FLOAT8(selectivity);

}


/**
 * This function is called by the analyze function iff
 * the geometry_analyze() function give it its pointer
 * (this is always the case so far).
 * The geometry_analyze() function is also responsible
 * of deciding the number of "sample" rows we will receive
 * here. It is able to give use other 'custom' data, but we
 * won't use them so far.
 *
 * Our job is to build some statistics on the sample data
 * for use by operator estimators.
 *
 * Currently we only need statistics to estimate the number of rows
 * overlapping a given extent (estimation function bound
 * to the && operator).
 *
 */
static void
compute_geometry_stats(VacAttrStats *stats, AnalyzeAttrFetchFunc fetchfunc,
                       int samplerows, double totalrows)
{
	MemoryContext old_context;
	int i;
	int geom_stats_size;
	BOX2DFLOAT4 **sampleboxes;
	GEOM_STATS *geomstats;
	bool isnull;
	int null_cnt=0, notnull_cnt=0, examinedsamples=0;
	BOX2DFLOAT4 *sample_extent=NULL;
	double total_width=0;
	double total_boxes_area=0;
	int total_boxes_cells=0;
	double cell_area;
	double cell_width;
	double cell_height;
#if USE_STANDARD_DEVIATION
	/* for standard deviation */
	double avgLOWx, avgLOWy, avgHIGx, avgHIGy;
	double sumLOWx=0, sumLOWy=0, sumHIGx=0, sumHIGy=0;
	double sdLOWx=0, sdLOWy=0, sdHIGx=0, sdHIGy=0;
	BOX2DFLOAT4 *newhistobox=NULL;
#endif
	double geow, geoh; /* width and height of histogram */
	int histocells;
	int cols, rows; /* histogram grid size */
	BOX2DFLOAT4 histobox;

	/*
	 * This is where geometry_analyze
	 * should put its' custom parameters.
	 */
	/* void *mystats = stats->extra_data; */

	/*
	 * We'll build an histogram having from 40 to 400 boxesPerSide
	 * Total number of cells is determined by attribute stat
	 * target. It can go from  1600 to 160000 (stat target: 10,1000)
	 */
	histocells = 160*stats->attr->attstattarget;


	POSTGIS_DEBUG(2, "compute_geometry_stats called");
	POSTGIS_DEBUGF(3, " samplerows: %d", samplerows);
	POSTGIS_DEBUGF(3, " histogram cells: %d", histocells);

	/*
	 * We might need less space, but don't think
	 * its worth saving...
	 */
	sampleboxes = palloc(sizeof(BOX2DFLOAT4 *)*samplerows);

	/*
	 * First scan:
	 *  o find extent of the sample rows
	 *  o count null-infinite/not-null values
	 *  o compute total_width
	 *  o compute total features's box area (for avgFeatureArea)
	 *  o sum features box coordinates (for standard deviation)
	 */
	for (i=0; i<samplerows; i++)
	{
		Datum datum;
		PG_LWGEOM *geom;
		BOX2DFLOAT4 box;

		datum = fetchfunc(stats, i, &isnull);

		/*
		 * Skip nulls
		 */
		if ( isnull )
		{
			null_cnt++;
			continue;
		}

		geom = (PG_LWGEOM *)PG_DETOAST_DATUM(datum);

		if ( ! getbox2d_p(SERIALIZED_FORM(geom), &box) )
		{
			/* Skip empty geometry */
			POSTGIS_DEBUGF(3, " skipped empty geometry %d", i);

			continue;
		}

		/*
		 * Skip infinite geoms
		 */
		if ( ! finite(box.xmin) ||
		        ! finite(box.xmax) ||
		        ! finite(box.ymin) ||
		        ! finite(box.ymax) )
		{
			POSTGIS_DEBUGF(3, " skipped infinite geometry %d", i);

			continue;
		}

		/*
		 * Cache bounding box
		 * TODO: reduce BOX2DFLOAT4 copies
		 */
		sampleboxes[notnull_cnt] = palloc(sizeof(BOX2DFLOAT4));
		memcpy(sampleboxes[notnull_cnt], &box, sizeof(BOX2DFLOAT4));

		/*
		 * Add to sample extent union
		 */
		if ( ! sample_extent )
		{
			sample_extent = palloc(sizeof(BOX2DFLOAT4));
			memcpy(sample_extent, &box, sizeof(BOX2DFLOAT4));
		}
		else
		{
			sample_extent->xmax = LWGEOM_Maxf(sample_extent->xmax,
			                                  box.xmax);
			sample_extent->ymax = LWGEOM_Maxf(sample_extent->ymax,
			                                  box.ymax);
			sample_extent->xmin = LWGEOM_Minf(sample_extent->xmin,
			                                  box.xmin);
			sample_extent->ymin = LWGEOM_Minf(sample_extent->ymin,
			                                  box.ymin);
		}

		/** TODO: ask if we need geom or bvol size for stawidth */
		total_width += geom->size;
		total_boxes_area += (box.xmax-box.xmin)*(box.ymax-box.ymin);

#if USE_STANDARD_DEVIATION
		/*
		 * Add bvol coordinates to sum for standard deviation
		 * computation.
		 */
		sumLOWx += box.xmin;
		sumLOWy += box.ymin;
		sumHIGx += box.xmax;
		sumHIGy += box.ymax;
#endif

		notnull_cnt++;

		/* give backend a chance of interrupting us */
		vacuum_delay_point();

	}

	if ( ! notnull_cnt )
	{
		elog(NOTICE, " no notnull values, invalid stats");
		stats->stats_valid = false;
		return;
	}

#if USE_STANDARD_DEVIATION

	POSTGIS_DEBUGF(3, " sample_extent: xmin,ymin: %f,%f",
	               sample_extent->xmin, sample_extent->ymin);
	POSTGIS_DEBUGF(3, " sample_extent: xmax,ymax: %f,%f",
	               sample_extent->xmax, sample_extent->ymax);

	/*
	 * Second scan:
	 *  o compute standard deviation
	 */
	avgLOWx = sumLOWx/notnull_cnt;
	avgLOWy = sumLOWy/notnull_cnt;
	avgHIGx = sumHIGx/notnull_cnt;
	avgHIGy = sumHIGy/notnull_cnt;
	for (i=0; i<notnull_cnt; i++)
	{
		BOX2DFLOAT4 *box;
		box = (BOX2DFLOAT4 *)sampleboxes[i];

		sdLOWx += (box->xmin - avgLOWx) * (box->xmin - avgLOWx);
		sdLOWy += (box->ymin - avgLOWy) * (box->ymin - avgLOWy);
		sdHIGx += (box->xmax - avgHIGx) * (box->xmax - avgHIGx);
		sdHIGy += (box->ymax - avgHIGy) * (box->ymax - avgHIGy);
	}
	sdLOWx = sqrt(sdLOWx/notnull_cnt);
	sdLOWy = sqrt(sdLOWy/notnull_cnt);
	sdHIGx = sqrt(sdHIGx/notnull_cnt);
	sdHIGy = sqrt(sdHIGy/notnull_cnt);

	POSTGIS_DEBUG(3, " standard deviations:");
	POSTGIS_DEBUGF(3, "  LOWx - avg:%f sd:%f", avgLOWx, sdLOWx);
	POSTGIS_DEBUGF(3, "  LOWy - avg:%f sd:%f", avgLOWy, sdLOWy);
	POSTGIS_DEBUGF(3, "  HIGx - avg:%f sd:%f", avgHIGx, sdHIGx);
	POSTGIS_DEBUGF(3, "  HIGy - avg:%f sd:%f", avgHIGy, sdHIGy);

	histobox.xmin = LW_MAX((avgLOWx - SDFACTOR * sdLOWx),
	                       sample_extent->xmin);
	histobox.ymin = LW_MAX((avgLOWy - SDFACTOR * sdLOWy),
	                       sample_extent->ymin);
	histobox.xmax = LW_MIN((avgHIGx + SDFACTOR * sdHIGx),
	                       sample_extent->xmax);
	histobox.ymax = LW_MIN((avgHIGy + SDFACTOR * sdHIGy),
	                       sample_extent->ymax);

	POSTGIS_DEBUGF(3, " sd_extent: xmin,ymin: %f,%f",
	               histobox.xmin, histobox.ymin);
	POSTGIS_DEBUGF(3, " sd_extent: xmax,ymax: %f,%f",
	               histobox.xmin, histobox.ymax);

	/*
	 * Third scan:
	 *   o skip hard deviants
	 *   o compute new histogram box
	 */
	for (i=0; i<notnull_cnt; i++)
	{
		BOX2DFLOAT4 *box;
		box = (BOX2DFLOAT4 *)sampleboxes[i];

		if ( box->xmin > histobox.xmax ||
		        box->xmax < histobox.xmin ||
		        box->ymin > histobox.ymax ||
		        box->ymax < histobox.ymin )
		{
			POSTGIS_DEBUGF(4, " feat %d is an hard deviant, skipped", i);

			sampleboxes[i] = NULL;
			continue;
		}
		if ( ! newhistobox )
		{
			newhistobox = palloc(sizeof(BOX2DFLOAT4));
			memcpy(newhistobox, box, sizeof(BOX2DFLOAT4));
		}
		else
		{
			if ( box->xmin < newhistobox->xmin )
				newhistobox->xmin = box->xmin;
			if ( box->ymin < newhistobox->ymin )
				newhistobox->ymin = box->ymin;
			if ( box->xmax > newhistobox->xmax )
				newhistobox->xmax = box->xmax;
			if ( box->ymax > newhistobox->ymax )
				newhistobox->ymax = box->ymax;
		}
	}

	/*
	 * Set histogram extent as the intersection between
	 * standard deviation based histogram extent
	 * and computed sample extent after removal of
	 * hard deviants (there might be no hard deviants).
	 */
	if ( histobox.xmin < newhistobox->xmin )
		histobox.xmin = newhistobox->xmin;
	if ( histobox.ymin < newhistobox->ymin )
		histobox.ymin = newhistobox->ymin;
	if ( histobox.xmax > newhistobox->xmax )
		histobox.xmax = newhistobox->xmax;
	if ( histobox.ymax > newhistobox->ymax )
		histobox.ymax = newhistobox->ymax;


#else /* ! USE_STANDARD_DEVIATION */

	/*
	* Set histogram extent box
	*/
	histobox.xmin = sample_extent->xmin;
	histobox.ymin = sample_extent->ymin;
	histobox.xmax = sample_extent->xmax;
	histobox.ymax = sample_extent->ymax;
#endif /* USE_STANDARD_DEVIATION */


	POSTGIS_DEBUGF(3, " histogram_extent: xmin,ymin: %f,%f",
	               histobox.xmin, histobox.ymin);
	POSTGIS_DEBUGF(3, " histogram_extent: xmax,ymax: %f,%f",
	               histobox.xmax, histobox.ymax);


	geow = histobox.xmax - histobox.xmin;
	geoh = histobox.ymax - histobox.ymin;

	/*
	 * Compute histogram cols and rows based on aspect ratio
	 * of histogram extent
	 */
	if ( ! geow && ! geoh )
	{
		cols = 1;
		rows = 1;
		histocells = 1;
	}
	else if ( ! geow )
	{
		cols = 1;
		rows = histocells;
	}
	else if ( ! geoh )
	{
		cols = histocells;
		rows = 1;
	}
	else
	{
		if ( geow<geoh)
		{
			cols = ceil(sqrt((double)histocells*(geow/geoh)));
			rows = ceil((double)histocells/cols);
		}
		else
		{
			rows = ceil(sqrt((double)histocells*(geoh/geow)));
			cols = ceil((double)histocells/rows);
		}
		histocells = cols*rows;
	}

	POSTGIS_DEBUGF(3, " computed histogram grid size (CxR): %dx%d (%d cells)", cols, rows, histocells);


	/*
	 * Create the histogram (GEOM_STATS)
	 */
	old_context = MemoryContextSwitchTo(stats->anl_context);
	geom_stats_size=sizeof(GEOM_STATS)+(histocells-1)*sizeof(float4);
	geomstats = palloc(geom_stats_size);
	MemoryContextSwitchTo(old_context);

	geomstats->avgFeatureArea = total_boxes_area/notnull_cnt;
	geomstats->xmin = histobox.xmin;
	geomstats->ymin = histobox.ymin;
	geomstats->xmax = histobox.xmax;
	geomstats->ymax = histobox.ymax;
	geomstats->cols = cols;
	geomstats->rows = rows;

	/* Initialize all values to 0 */
	for (i=0; i<histocells; i++) geomstats->value[i] = 0;

	cell_width = geow/cols;
	cell_height = geoh/rows;
	cell_area = cell_width*cell_height;

	POSTGIS_DEBUGF(4, "cell_width: %f", cell_width);
	POSTGIS_DEBUGF(4, "cell_height: %f", cell_height);


	/*
	 * Fourth scan:
	 *  o fill histogram values with the number of
	 *    features' bbox overlaps: a feature's bvol
	 *    can fully overlap (1) or partially overlap
	 *    (fraction of 1) an histogram cell.
	 *
	 *  o compute total cells occupation
	 *
	 */
	for (i=0; i<notnull_cnt; i++)
	{
		BOX2DFLOAT4 *box;
		int x_idx_min, x_idx_max, x;
		int y_idx_min, y_idx_max, y;
		int numcells=0;

		box = (BOX2DFLOAT4 *)sampleboxes[i];
		if ( ! box ) continue; /* hard deviant.. */

		/* give backend a chance of interrupting us */
		vacuum_delay_point();

		POSTGIS_DEBUGF(4, " feat %d box is %f %f, %f %f",
		               i, box->xmax, box->ymax,
		               box->xmin, box->ymin);

		/* Find first overlapping column */
		x_idx_min = (box->xmin-geomstats->xmin) / geow * cols;
		if (x_idx_min <0) x_idx_min = 0;
		if (x_idx_min >= cols) x_idx_min = cols-1;

		/* Find first overlapping row */
		y_idx_min = (box->ymin-geomstats->ymin) / geoh * rows;
		if (y_idx_min <0) y_idx_min = 0;
		if (y_idx_min >= rows) y_idx_min = rows-1;

		/* Find last overlapping column */
		x_idx_max = (box->xmax-geomstats->xmin) / geow * cols;
		if (x_idx_max <0) x_idx_max = 0;
		if (x_idx_max >= cols ) x_idx_max = cols-1;

		/* Find last overlapping row */
		y_idx_max = (box->ymax-geomstats->ymin) / geoh * rows;
		if (y_idx_max <0) y_idx_max = 0;
		if (y_idx_max >= rows) y_idx_max = rows-1;

		POSTGIS_DEBUGF(4, " feat %d overlaps columns %d-%d, rows %d-%d",
		               i, x_idx_min, x_idx_max, y_idx_min, y_idx_max);

		/*
		 * the {x,y}_idx_{min,max}
		 * define the grid squares that the box intersects
		 */
		for (y=y_idx_min; y<=y_idx_max; y++)
		{
			for (x=x_idx_min; x<=x_idx_max; x++)
			{
				geomstats->value[x+y*cols] += 1;
				numcells++;
			}
		}

		/*
		 * before adding to the total cells
		 * we could decide if we really
		 * want this feature to count
		 */
		total_boxes_cells += numcells;

		examinedsamples++;
	}

	POSTGIS_DEBUGF(3, " examined_samples: %d/%d", examinedsamples, samplerows);

	if ( ! examinedsamples )
	{
		elog(NOTICE, " no examined values, invalid stats");
		stats->stats_valid = false;

		POSTGIS_DEBUG(3, " no stats have been gathered");

		return;
	}

	/** TODO: what about null features (TODO) */
	geomstats->avgFeatureCells = (float4)total_boxes_cells/examinedsamples;

	POSTGIS_DEBUGF(3, " histo: total_boxes_cells: %d", total_boxes_cells);
	POSTGIS_DEBUGF(3, " histo: avgFeatureArea: %f", geomstats->avgFeatureArea);
	POSTGIS_DEBUGF(3, " histo: avgFeatureCells: %f", geomstats->avgFeatureCells);


	/*
	 * Normalize histogram
	 *
	 * We divide each histogram cell value
	 * by the number of samples examined.
	 *
	 */
	for (i=0; i<histocells; i++)
		geomstats->value[i] /= examinedsamples;

	{
		int x, y;
		for (x=0; x<cols; x++)
		{
			for (y=0; y<rows; y++)
			{
				POSTGIS_DEBUGF(4, " histo[%d,%d] = %.15f", x, y, geomstats->value[x+y*cols]);
			}
		}
	}


	/*
	 * Write the statistics data
	 */
	stats->stakind[0] = STATISTIC_KIND_GEOMETRY;
	stats->staop[0] = InvalidOid;
	stats->stanumbers[0] = (float4 *)geomstats;
	stats->numnumbers[0] = geom_stats_size/sizeof(float4);

	stats->stanullfrac = (float4)null_cnt/samplerows;
	stats->stawidth = total_width/notnull_cnt;
	stats->stadistinct = -1.0;

	POSTGIS_DEBUGF(3, " out: slot 0: kind %d (STATISTIC_KIND_GEOMETRY)",
	               stats->stakind[0]);
	POSTGIS_DEBUGF(3, " out: slot 0: op %d (InvalidOid)", stats->staop[0]);
	POSTGIS_DEBUGF(3, " out: slot 0: numnumbers %d", stats->numnumbers[0]);
	POSTGIS_DEBUGF(3, " out: null fraction: %d/%d=%g", null_cnt, samplerows, stats->stanullfrac);
	POSTGIS_DEBUGF(3, " out: average width: %d bytes", stats->stawidth);
	POSTGIS_DEBUG(3, " out: distinct values: all (no check done)");

	stats->stats_valid = true;
}

/**
 * This function will be called when the ANALYZE command is run
 * on a column of the "geometry" type.
 *
 * It will need to return a stats builder function reference
 * and a "minimum" sample rows to feed it.
 * If we want analisys to be completely skipped we can return
 * FALSE and leave output vals untouched.
 *
 * What we know from this call is:
 *
 * 	o The pg_attribute row referring to the specific column.
 * 	  Could be used to get reltuples from pg_class (which
 * 	  might quite inexact though...) and use them to set an
 * 	  appropriate minimum number of sample rows to feed to
 * 	  the stats builder. The stats builder will also receive
 * 	  a more accurate "estimation" of the number or rows.
 *
 * 	o The pg_type row for the specific column.
 * 	  Could be used to set stat builder / sample rows
 * 	  based on domain type (when postgis will be implemented
 * 	  that way).
 *
 * Being this experimental we'll stick to a static stat_builder/sample_rows
 * value for now.
 *
 */
PG_FUNCTION_INFO_V1(LWGEOM_analyze);
Datum LWGEOM_analyze(PG_FUNCTION_ARGS)
{
	VacAttrStats *stats = (VacAttrStats *)PG_GETARG_POINTER(0);
	Form_pg_attribute attr = stats->attr;

	POSTGIS_DEBUG(2, "lwgeom_analyze called");

	/* If the attstattarget column is negative, use the default value */
	/* NB: it is okay to scribble on stats->attr since it's a copy */
	if (attr->attstattarget < 0)
		attr->attstattarget = default_statistics_target;

	POSTGIS_DEBUGF(3, " attribute stat target: %d", attr->attstattarget);

	/*
	 * There might be a reason not to analyze this column
	 * (can we detect the absence of an index?)
	 */
#if 0
	elog(NOTICE, "compute_geometry_stats not implemented yet");
	PG_RETURN_BOOL(false);
#endif

	/* Setup the minimum rows and the algorithm function */
	stats->minrows = 300 * stats->attr->attstattarget;
	stats->compute_stats = compute_geometry_stats;

	POSTGIS_DEBUGF(3, " minrows: %d", stats->minrows);

	/* Indicate we are done successfully */
	PG_RETURN_BOOL(true);
}

/**
 * Return the estimated extent of the table
 * looking at gathered statistics (or NULL if
 * no statistics have been gathered).
 */
PG_FUNCTION_INFO_V1(LWGEOM_estimated_extent);
Datum LWGEOM_estimated_extent(PG_FUNCTION_ARGS)
{
	text *txnsp = NULL;
	text *txtbl = NULL;
	text *txcol = NULL;
	char *nsp = NULL;
	char *tbl = NULL;
	char *col = NULL;
	char *query;
	ArrayType *array = NULL;
	int SPIcode;
	SPITupleTable *tuptable;
	TupleDesc tupdesc ;
	HeapTuple tuple ;
	bool isnull;
	BOX2DFLOAT4 *box;
	size_t querysize;
	Datum binval;

	if ( PG_NARGS() == 3 )
	{
		txnsp = PG_GETARG_TEXT_P(0);
		txtbl = PG_GETARG_TEXT_P(1);
		txcol = PG_GETARG_TEXT_P(2);
	}
	else if ( PG_NARGS() == 2 )
	{
		txtbl = PG_GETARG_TEXT_P(0);
		txcol = PG_GETARG_TEXT_P(1);
	}
	else
	{
		elog(ERROR, "estimated_extent() called with wrong number of arguments");
		PG_RETURN_NULL();
	}

	POSTGIS_DEBUG(2, "LWGEOM_estimated_extent called");

	/* Connect to SPI manager */
	SPIcode = SPI_connect();
	if (SPIcode != SPI_OK_CONNECT)
	{
		elog(ERROR, "LWGEOM_estimated_extent: couldnt open a connection to SPI");
		PG_RETURN_NULL() ;
	}

	querysize = VARSIZE(txtbl)+VARSIZE(txcol)+516;

	if ( txnsp )
	{
		nsp = palloc(VARSIZE(txnsp)+1);
		memcpy(nsp, VARDATA(txnsp), VARSIZE(txnsp)-VARHDRSZ);
		nsp[VARSIZE(txnsp)-VARHDRSZ]='\0';
		querysize += VARSIZE(txnsp);
	}
	else
	{
		querysize += 32; /* current_schema() */
	}

	tbl = palloc(VARSIZE(txtbl)+1);
	memcpy(tbl, VARDATA(txtbl), VARSIZE(txtbl)-VARHDRSZ);
	tbl[VARSIZE(txtbl)-VARHDRSZ]='\0';

	col = palloc(VARSIZE(txcol)+1);
	memcpy(col, VARDATA(txcol), VARSIZE(txcol)-VARHDRSZ);
	col[VARSIZE(txcol)-VARHDRSZ]='\0';

#if POSTGIS_DEBUG_LEVEL > 0
	if ( txnsp )
	{
		POSTGIS_DEBUGF(3, " schema:%s table:%s column:%s", nsp, tbl, col);
	}
	else
	{
		POSTGIS_DEBUGF(3, " schema:current_schema() table:%s column:%s",
		               tbl, col);
	}
#endif

	query = palloc(querysize);


	/* Security check: because we access information in the pg_statistic table, we must run as the database
	superuser (by marking the function as SECURITY DEFINER) and check permissions ourselves */
	if ( txnsp )
	{
		sprintf(query, "SELECT has_table_privilege((SELECT usesysid FROM pg_user WHERE usename = session_user), '\"%s\".\"%s\"', 'select')", nsp, tbl);
	}
	else
	{
		sprintf(query, "SELECT has_table_privilege((SELECT usesysid FROM pg_user WHERE usename = session_user), '\"%s\"', 'select')", tbl);
	}

	POSTGIS_DEBUGF(4, "permission check sql query is: %s", query);

	SPIcode = SPI_exec(query, 1);
	if (SPIcode != SPI_OK_SELECT)
	{
		elog(ERROR, "LWGEOM_estimated_extent: couldn't execute permission check sql via SPI");
		SPI_finish();
		PG_RETURN_NULL();
	}

	tuptable = SPI_tuptable;
	tupdesc = SPI_tuptable->tupdesc;
	tuple = tuptable->vals[0];

	binval = SPI_getbinval(tuple, tupdesc, 1, &isnull);
	if ( isnull || !DatumGetBool(binval) )
	{
		elog(ERROR, "LWGEOM_estimated_extent: permission denied for relation %s", tbl);
		SPI_finish();
		PG_RETURN_NULL();
	}


	/* Return the stats data */
	if ( txnsp )
	{
		sprintf(query, "SELECT s.stanumbers1[5:8] FROM pg_statistic s, pg_class c, pg_attribute a, pg_namespace n WHERE c.relname = '%s' AND a.attrelid = c.oid AND a.attname = '%s' AND n.nspname = '%s' AND c.relnamespace = n.oid AND s.starelid=c.oid AND s.staattnum = a.attnum AND staattnum = attnum", tbl, col, nsp);
	}
	else
	{
		sprintf(query, "SELECT s.stanumbers1[5:8] FROM pg_statistic s, pg_class c, pg_attribute a, pg_namespace n WHERE c.relname = '%s' AND a.attrelid = c.oid AND a.attname = '%s' AND n.nspname = current_schema() AND c.relnamespace = n.oid AND s.starelid=c.oid AND s.staattnum = a.attnum AND staattnum = attnum", tbl, col);
	}

	POSTGIS_DEBUGF(4, " query: %s", query);

	SPIcode = SPI_exec(query, 1);
	if (SPIcode != SPI_OK_SELECT )
	{
		elog(ERROR,"LWGEOM_estimated_extent: couldnt execute sql via SPI");
		SPI_finish();
		PG_RETURN_NULL();
	}
	if (SPI_processed != 1)
	{

		POSTGIS_DEBUGF(3, " %d stat rows", SPI_processed);

		/* 
		 * TODO: distinguish between empty and not analyzed ?
		 */
		elog(WARNING, "No stats for \"%s\".\"%s\".\"%s\" "
			"(empty or not analyzed)",
			( nsp ? nsp : "<current>" ), tbl, col);
		SPI_finish();

		PG_RETURN_NULL() ;
	}

	tuptable = SPI_tuptable;
	tupdesc = SPI_tuptable->tupdesc;
	tuple = tuptable->vals[0];
	binval = SPI_getbinval(tuple, tupdesc, 1, &isnull);
	if ( isnull )
	{

		POSTGIS_DEBUG(3, " stats are NULL");

		elog(ERROR, "LWGEOM_estimated_extent: couldn't locate statistics for table");
		SPI_finish();

		PG_RETURN_NULL();
	}
	array = DatumGetArrayTypeP(binval);
	if ( ArrayGetNItems(ARR_NDIM(array), ARR_DIMS(array)) != 4 )
	{
		elog(ERROR, " corrupted histogram");
		PG_RETURN_NULL();
	}

	POSTGIS_DEBUGF(3, " stats array has %d elems", ArrayGetNItems(ARR_NDIM(array), ARR_DIMS(array)));

	/*
	 * Construct box2dfloat4.
	 * Must allocate this in upper executor context
	 * to keep it alive after SPI_finish().
	 */
	box = SPI_palloc(sizeof(BOX2DFLOAT4));

	/* Construct the box */
	memcpy(box, ARR_DATA_PTR(array), sizeof(BOX2DFLOAT4));

	POSTGIS_DEBUGF(3, " histogram extent = %g %g, %g %g", box->xmin,
	               box->ymin, box->xmax, box->ymax);

	SPIcode = SPI_finish();
	if (SPIcode != SPI_OK_FINISH )
	{
		elog(ERROR, "LWGEOM_estimated_extent: couldnt disconnect from SPI");
	}

	/* TODO: enlarge the box by some factor */

	PG_RETURN_POINTER(box);
}




/**********************************************************************
 * $Log$
 * Revision 1.39  2006/05/30 08:38:58  strk
 * Added some missing copyright headers.
 *
 * Revision 1.38  2006/03/13 10:54:08  strk
 * Applied patch from Mark Cave Ayland embedding access control for
 * the estimated_extent functions.
 *
 * Revision 1.37  2006/01/09 11:48:15  strk
 * Fixed "strict-aliasing rule" breaks.
 *
 * Revision 1.36  2005/12/30 17:40:37  strk
 * Moved PG_LWGEOM WKB I/O and SRID get/set funx
 * from lwgeom_api.c to lwgeom_pg.c.
 * Made lwgeom_from_ewkb directly invoke grammar parser rather then invoke
 * the PG_LWGEOM-specific function.
 * Cleaned up signedness-related and comments-related warnings for the files
 * being committed (more to do on other files)
 *
 * Revision 1.35  2005/10/10 16:19:16  strk
 * Fixed null values fraction computation in geometry analyzer as suggested by Michael Fuhr
 *
 * Revision 1.34  2005/09/08 19:26:22  strk
 * Handled search_box outside of histogram_box case in selectivity estimator
 *
 * Revision 1.33  2005/06/28 22:00:09  strk
 * Fixed extimators to work with postgresql 8.1.x
 *
 * Revision 1.32  2005/04/22 01:07:09  strk
 * Fixed bug in join selectivity estimator returning invalid estimates (>1)
 *
 * Revision 1.31  2005/04/18 14:12:43  strk
 * Slightly changed standard deviation computation to be more corner-case-friendly.
 *
 * Revision 1.30  2005/04/18 10:57:13  strk
 * Applied patched by Ron Mayer fixing memory leakages and invalid results
 * in join selectivity estimator. Fixed some return to use default JOIN
 * selectivity estimate instead of default RESTRICT selectivity estimate.
 *
 * Revision 1.29  2005/03/25 09:34:25  strk
 * code cleanup
 *
 * Revision 1.28  2005/03/24 16:27:32  strk
 * Added comments in estimate_allocation() bugfix point.
 *
 * Revision 1.27  2005/03/24 14:45:50  strk
 * Fixed bug in estimated_extent() returning pointer to a memory allocated in SPI memory context
 *
 * Revision 1.26  2005/03/08 09:27:23  strk
 * RESTRICT selectivity estimator use self->varno instead of varRelid.
 * Seems to work for subqueries...
 *
 * Revision 1.25  2005/03/08 09:23:34  strk
 * Fixed debugging lines.
 *
 * Revision 1.24  2005/02/21 16:22:32  strk
 * Changed min() max() usage with LW_MIN() LW_MAX()
 *
 * Revision 1.23  2005/02/10 10:52:53  strk
 * Changed 'char' to 'uchar' (unsigned char typedef) wherever octet is actually
 * meant to be.
 *
 * Revision 1.22  2005/01/13 18:26:49  strk
 * estimated_extent() implemented for PG<80
 *
 * Revision 1.21  2005/01/13 17:41:40  strk
 * estimated_extent() prepared for future expansion (support of pre-800 PGSQL)
 *
 * Revision 1.20  2005/01/07 09:52:12  strk
 * JOINSEL disabled for builds against pgsql<80
 *
 * Revision 1.19  2004/12/22 17:12:34  strk
 * Added Mark Cave-Ayland implementation of JOIN selectivity estimator.
 *
 * Revision 1.18  2004/12/21 12:21:45  mcayland
 * Fixed bug in pass 4 where sample boxes were referred as BOXs and not BOX2DFLOAT4. Also increased SDFACTOR to 3.25
 *
 * Revision 1.17  2004/12/17 18:00:33  strk
 * LWGEOM_gist_joinsel defined for all PG versions
 *
 * Revision 1.16  2004/12/17 11:07:48  strk
 * Added missing prototype
 *
 * Revision 1.15  2004/12/13 14:03:07  strk
 * Initial skeleton on join selectivity estimator.
 * Current estimators application for box2d && box2d operator.
 *
 * Revision 1.14  2004/12/13 12:25:27  strk
 * Removed obsoleted function and fixed some warnings.
 *
 * Revision 1.13  2004/12/10 12:35:11  strk
 * implemented estimated_extent() function
 *
 * Revision 1.12  2004/11/04 11:40:08  strk
 * Renamed max/min/avg macros to LW_MAX, LW_MIN, LW_AVG.
 *
 * Revision 1.11  2004/10/27 11:02:24  strk
 * Removed another getbox2d() call.
 *
 * Revision 1.10  2004/10/25 17:07:09  strk
 * Obsoleted getbox2d(). Use getbox2d_p() or getbox2d_internal() instead.
 *
 * Revision 1.9  2004/10/08 13:20:54  strk
 *
 * Changed LWGEOM structure to point to an actual BOX2DFLOAT4.
 * Renamed most function to reflect a TYPE_method naming convention.
 * (you'll need a dump/reload for it to work)
 * Added more manipulation functions.
 *
 **********************************************************************/
