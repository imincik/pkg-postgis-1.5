/**********************************************************************
 * $Id: lwgeom_functions_lrs.c 10612 2012-10-31 21:16:08Z pramsey $
 *
 * PostGIS - Spatial Types for PostgreSQL
 * http://postgis.refractions.net
 * Copyright 2001-2005 Refractions Research Inc.
 *
 * This is free software; you can redistribute and/or modify it under
 * the terms of the GNU General Public Licence. See the COPYING file.
 *
 **********************************************************************/

#include "postgres.h"
#include "fmgr.h"
#include "liblwgeom.h"
#include "lwgeom_pg.h"
#include "math.h"


Datum LWGEOM_locate_between_m(PG_FUNCTION_ARGS);
Datum ST_AddMeasure(PG_FUNCTION_ARGS);

typedef struct
{
	POINTARRAY **ptarrays;
	uint32 nptarrays;
}
POINTARRAYSET;

static POINTARRAYSET ptarray_locate_between_m(
    POINTARRAY *ipa, double m0, double m1);

static LWGEOM *lwcollection_locate_between_m(
    LWCOLLECTION *lwcoll, double m0, double m1);

static LWGEOM *lwgeom_locate_between_m(
    LWGEOM *lwin, double m0, double m1);

static LWGEOM *lwline_locate_between_m(
    LWLINE *lwline_in, double m0, double m1);

static LWGEOM *lwpoint_locate_between_m(
    LWPOINT *lwpoint, double m0, double m1);

static int clip_seg_by_m_range(
    POINT4D *p1, POINT4D *p2, double m0, double m1);


/*
 * Clip a segment by a range of measures.
 * Z and M values are interpolated in case of clipping.
 *
 * Returns a bitfield, flags being:
 *    0x0001 : segment intersects the range
 *    0x0010 : first point is modified
 *    0x0100 : second point is modified
 *
 * Values:
 *     - 0 segment fully outside the range, no modifications
 *     - 1 segment fully inside the range, no modifications
 *     - 7 segment crosses the range, both points modified.
 *     - 3 first point out, second in, first point modified
 *     - 5 first point in, second out, second point modified
 */
static int
clip_seg_by_m_range(POINT4D *p1, POINT4D *p2, double m0, double m1)
{
	double dM0, dM1, dX, dY, dZ;
	POINT4D *tmp;
	int swapped=0;
	int ret=0;

	LWDEBUGF(3, "m0: %g m1: %g", m0, m1);

	/* Handle corner case of m values being the same */
	if ( p1->m == p2->m )
	{
		/* out of range, no clipping */
		if ( p1->m < m0 || p1->m > m1 )
			return 0;

		/* inside range, no clipping */
		return 1;
	}

	/*
	 * Order points so that p1 has the smaller M
	 */
	if ( p1->m > p2->m )
	{
		tmp=p2;
		p2=p1;
		p1=tmp;
		swapped=1;
	}

	/*
	 * The M range is not intersected, segment
	 * fully out of range, no clipping.
	 */
	if ( p2->m < m0 || p1->m > m1 )
		return 0;

	/*
	 * The segment is fully inside the range,
	 * no clipping.
	 */
	if ( p1->m >= m0 && p2->m <= m1 )
		return 1;

	/*
	 * Segment intersects range, lets compute
	 * the proportional location of the two
	 * measures wrt p1/p2 m range.
	 *
	 * if p1 and p2 have the same measure
	 * this should never be reached (either
	 * both inside or both outside)
	 *
	 */
	dM0=(m0-p1->m)/(p2->m-p1->m); /* delta-M0 */
	dM1=(m1-p2->m)/(p2->m-p1->m); /* delta-M1 */
	dX=p2->x-p1->x;
	dY=p2->y-p1->y;
	dZ=p2->z-p1->z;

	LWDEBUGF(3, "dM0:%g dM1:%g", dM0, dM1);
	LWDEBUGF(3, "dX:%g dY:%g dZ:%g", dX, dY, dZ);
	LWDEBUGF(3, "swapped: %d", swapped);

	/*
	 * First point out of range, project
	 * it on the range
	 */
	if ( p1->m < m0 )
	{
		/*
		 * To prevent rounding errors, then if m0==m1 and p2 lies within the range, copy
		 * p1 as a direct copy of p2
		 */
		if (m0 == m1 && p2->m <= m1)
		{
			memcpy(p1, p2, sizeof(POINT4D));

			LWDEBUG(3, "Projected p1 on range (as copy of p2)");
		}
		else
		{
			/* Otherwise interpolate coordinates */
			p1->x += (dX*dM0);
			p1->y += (dY*dM0);
			p1->z += (dZ*dM0);
			p1->m = m0;

			LWDEBUG(3, "Projected p1 on range");
		}

		if ( swapped ) ret |= 0x0100;
		else ret |= 0x0010;
	}

	/*
	 * Second point out of range, project
	 * it on the range
	 */
	if ( p2->m > m1 )
	{
		/*
		 * To prevent rounding errors, then if m0==m1 and p1 lies within the range, copy
		 * p2 as a direct copy of p1
		 */
		if (m0 == m1 && p1->m >= m0)
		{
			memcpy(p2, p1, sizeof(POINT4D));

			LWDEBUG(3, "Projected p2 on range (as copy of p1)");
		}
		else
		{
			/* Otherwise interpolate coordinates */
			p2->x += (dX*dM1);
			p2->y += (dY*dM1);
			p2->z += (dZ*dM1);
			p2->m = m1;

			LWDEBUG(3, "Projected p2 on range");
		}

		if ( swapped ) ret |= 0x0010;
		else ret |= 0x0100;
	}

	/* Clipping occurred */
	return ret;

}

static POINTARRAYSET
ptarray_locate_between_m(POINTARRAY *ipa, double m0, double m1)
{
	POINTARRAYSET ret;
	DYNPTARRAY *dpa=NULL;
	int i;

	ret.nptarrays=0;

	/*
	 * We allocate space for as many pointarray as
	 * segments in the input POINTARRAY, as worst
	 * case is for each segment to cross the M range
	 * window.
	 * TODO: rework this to reduce used memory
	 */
	ret.ptarrays=lwalloc(sizeof(POINTARRAY *)*ipa->npoints-1);

	LWDEBUGF(2, "ptarray_locate...: called for pointarray %x, m0:%g, m1:%g",
	         ipa, m0, m1);


	for (i=1; i<ipa->npoints; i++)
	{
		POINT4D p1, p2;
		int clipval;

		getPoint4d_p(ipa, i-1, &p1);
		getPoint4d_p(ipa, i, &p2);

		LWDEBUGF(3, " segment %d-%d [ %g %g %g %g -  %g %g %g %g ]",
		         i-1, i,
		         p1.x, p1.y, p1.z, p1.m,
		         p2.x, p2.y, p2.z, p2.m);

		clipval = clip_seg_by_m_range(&p1, &p2, m0, m1);

		/* segment completely outside, nothing to do */
		if (! clipval ) continue;

		LWDEBUGF(3, " clipped to: [ %g %g %g %g - %g %g %g %g ]   clipval: %x", p1.x, p1.y, p1.z, p1.m,
		         p2.x, p2.y, p2.z, p2.m, clipval);

		/* If no points have been accumulated so far, then if clipval != 0 the first point must be the
		   start of the intersection */
		if (dpa == NULL)
		{
			LWDEBUGF(3, " 1 creating new POINTARRAY with first point %g,%g,%g,%g", p1.x, p1.y, p1.z, p1.m);

			dpa = dynptarray_create(ipa->npoints-i, ipa->dims);
			dynptarray_addPoint4d(dpa, &p1, 1);
		}

		/* Otherwise always add the next point, avoiding duplicates */
		if (dpa)
			dynptarray_addPoint4d(dpa, &p2, 0);

		/*
		 * second point has been clipped
		 */
		if ( clipval & 0x0100 || i == ipa->npoints-1 )
		{
			LWDEBUGF(3, " closing pointarray %x with %d points", dpa->pa, dpa->pa->npoints);

			ret.ptarrays[ret.nptarrays++] = dpa->pa;
			lwfree(dpa);
			dpa=NULL;
		}
	}

	/*
	 * if dpa!=NULL it means we didn't close it yet.
	 * this should never happen.
	 */
	if ( dpa != NULL ) lwerror("Something wrong with algorithm");

	return ret;
}

/*
 * Point is assumed to have an M value.
 * Return NULL if point is not in the given range (inclusive)
 * Return an LWPOINT *copy* otherwise.
 */
static LWGEOM *
lwpoint_locate_between_m(LWPOINT *lwpoint, double m0, double m1)
{
	POINT3DM p3dm;

	LWDEBUGF(2, "lwpoint_locate_between called for lwpoint %x", lwpoint);

	lwpoint_getPoint3dm_p(lwpoint, &p3dm);
	if ( p3dm.m >= m0 && p3dm.m <= m1)
	{
		LWDEBUG(3, " lwpoint... returning a clone of input");

		return (LWGEOM *)lwpoint_clone(lwpoint);
	}
	else
	{
		LWDEBUG(3, " lwpoint... returning a clone of input");

		return NULL;
	}
}

/*
 * Line is assumed to have an M value.
 *
 * Return NULL if no parts of the line are in the given range (inclusive)
 *
 * Return an LWCOLLECTION with LWLINES and LWPOINT being consecutive
 * and isolated points on the line falling in the range.
 *
 * X,Y and Z (if present) ordinates are interpolated.
 *
 */
static LWGEOM *
lwline_locate_between_m(LWLINE *lwline_in, double m0, double m1)
{
	POINTARRAY *ipa=lwline_in->points;
	int i;
	LWGEOM **geoms;
	int ngeoms;
	int outtype;
	int typeflag=0; /* see flags below */
	const int pointflag=0x01;
	const int lineflag=0x10;
	POINTARRAYSET paset=ptarray_locate_between_m(ipa, m0, m1);

	LWDEBUGF(2, "lwline_locate_between called for lwline %x", lwline_in);

	LWDEBUGF(3, " ptarray_locate... returned %d pointarrays",
	         paset.nptarrays);

	if ( paset.nptarrays == 0 )
	{
		return NULL;
	}

	ngeoms=paset.nptarrays;
	/* TODO: rework this to reduce used memory */
	geoms=lwalloc(sizeof(LWGEOM *)*ngeoms);
	for (i=0; i<ngeoms; i++)
	{
		LWPOINT *lwpoint;
		LWLINE *lwline;

		POINTARRAY *pa=paset.ptarrays[i];

		/* This is a point */
		if ( pa->npoints == 1 )
		{
			lwpoint=lwalloc(sizeof(LWPOINT));
			lwpoint->type=lwgeom_makeType_full(
			                  TYPE_HASZ(pa->dims),
			                  TYPE_HASM(pa->dims),
			                  lwline_in->SRID,
			                  POINTTYPE,
			                  0);
			lwpoint->SRID=lwline_in->SRID;
			lwpoint->bbox=NULL;
			lwpoint->point=pa;
			geoms[i]=(LWGEOM *)lwpoint;
			typeflag|=pointflag;
		}

		/* This is a line */
		else if ( pa->npoints > 1 )
		{
			lwline=lwalloc(sizeof(LWLINE));
			lwline->type=lwgeom_makeType_full(
			                 TYPE_HASZ(pa->dims),
			                 TYPE_HASM(pa->dims),
			                 lwline_in->SRID,
			                 LINETYPE,
			                 0);
			lwline->SRID=lwline_in->SRID;
			lwline->bbox=NULL;
			lwline->points=pa;
			geoms[i]=(LWGEOM *)lwline;
			typeflag|=lineflag;
		}

		/* This is a bug */
		else
		{
			lwerror("ptarray_locate_between_m returned a POINARRAY set containing POINTARRAY with 0 points");
		}

	}

	if ( ngeoms == 1 )
	{
		return geoms[0];
	}
	else
	{
		/* Choose best type */
		if ( typeflag == 1 ) outtype=MULTIPOINTTYPE;
		else if ( typeflag == 2 ) outtype=MULTILINETYPE;
		else outtype = COLLECTIONTYPE;

		return (LWGEOM *)lwcollection_construct(outtype,
		                                        lwline_in->SRID, NULL, ngeoms, geoms);
	}
}

/*
 * Return a fully new allocated LWCOLLECTION
 * always tagged as COLLECTIONTYPE.
 */
static LWGEOM *
lwcollection_locate_between_m(LWCOLLECTION *lwcoll, double m0, double m1)
{
	int i;
	int ngeoms=0;
	LWGEOM **geoms;

	LWDEBUGF(2, "lwcollection_locate_between_m called for lwcoll %x", lwcoll);

	geoms=lwalloc(sizeof(LWGEOM *)*lwcoll->ngeoms);
	for (i=0; i<lwcoll->ngeoms; i++)
	{
		LWGEOM *sub=lwgeom_locate_between_m(lwcoll->geoms[i],
		                                    m0, m1);
		if ( sub != NULL )
			geoms[ngeoms++] = sub;
	}

	if ( ngeoms == 0 ) return NULL;

	return (LWGEOM *)lwcollection_construct(COLLECTIONTYPE,
	                                        lwcoll->SRID, NULL, ngeoms, geoms);
}

/*
 * Return a fully allocated LWGEOM containing elements
 * intersected/interpolated with the given M range.
 * Return NULL if none of the elements fall in the range.
 *
 * m0 is assumed to be less-or-equal to m1.
 * input LWGEOM is assumed to contain an M value.
 *
 */
static LWGEOM *
lwgeom_locate_between_m(LWGEOM *lwin, double m0, double m1)
{
	LWDEBUGF(2, "lwgeom_locate_between called for lwgeom %x", lwin);

	switch (TYPE_GETTYPE(lwin->type))
	{
	case POINTTYPE:
		return lwpoint_locate_between_m(
		           (LWPOINT *)lwin, m0, m1);
	case LINETYPE:
		return lwline_locate_between_m(
		           (LWLINE *)lwin, m0, m1);

	case MULTIPOINTTYPE:
	case MULTILINETYPE:
	case COLLECTIONTYPE:
		return lwcollection_locate_between_m(
		           (LWCOLLECTION *)lwin, m0, m1);

		/* Polygon types are not supported */
	case POLYGONTYPE:
	case MULTIPOLYGONTYPE:
		lwerror("Areal geometries are not supported by locate_between_measures");
		return NULL;
	}

	lwerror("Unkonwn geometry type (%s:%d)", __FILE__, __LINE__);
	return NULL;
}

/*
 * Return a derived geometry collection value with elements that match
 * the specified range of measures inclusively.
 *
 * Implements SQL/MM ST_LocateBetween(measure, measure) method.
 * See ISO/IEC CD 13249-3:200x(E)
 *
 */
PG_FUNCTION_INFO_V1(LWGEOM_locate_between_m);
Datum LWGEOM_locate_between_m(PG_FUNCTION_ARGS)
{
	PG_LWGEOM *gin = (PG_LWGEOM *)PG_DETOAST_DATUM(PG_GETARG_DATUM(0));
	PG_LWGEOM *gout;
	double start_measure = PG_GETARG_FLOAT8(1);
	double end_measure = PG_GETARG_FLOAT8(2);
	LWGEOM *lwin, *lwout;
	int type;

	if ( end_measure < start_measure )
	{
		lwerror("locate_between_m: 2nd arg must be bigger then 1st arg");
		PG_RETURN_NULL();
	}

	/*
	 * Return error if input doesn't have a measure
	 */
	if ( ! lwgeom_hasM(gin->type) )
	{
		lwerror("Geometry argument does not have an 'M' ordinate");
		PG_RETURN_NULL();
	}

	/*
	 * Raise an error if input is a polygon, a multipolygon
	 * or a collection
	 */
	type=lwgeom_getType(gin->type);

	if ( type == POLYGONTYPE || type == MULTIPOLYGONTYPE || type == COLLECTIONTYPE )
	{
		lwerror("Areal or Collection types are not supported");
		PG_RETURN_NULL();
	}

	lwin = pglwgeom_deserialize(gin);

	lwout = lwgeom_locate_between_m(lwin,
	                                start_measure, end_measure);

	lwgeom_release(lwin);

	if ( lwout == NULL )
	{
		lwout = (LWGEOM *)lwcollection_construct_empty(
		            pglwgeom_getSRID(gin), lwgeom_hasZ(gin->type),
		            lwgeom_hasM(gin->type));
	}

	gout = pglwgeom_serialize(lwout);
	lwgeom_release(lwout);

	PG_RETURN_POINTER(gout);
}


/*
* CREATE OR REPLACE FUNCTION ST_AddMeasure(geometry, float8, float8)
* RETURNS geometry
* AS '$libdir/postgis-1.5', 'ST_AddMeasure'
* LANGUAGE 'c' IMMUTABLE STRICT;
*/
PG_FUNCTION_INFO_V1(ST_AddMeasure);
Datum ST_AddMeasure(PG_FUNCTION_ARGS)
{
	PG_LWGEOM *gin = (PG_LWGEOM *)PG_DETOAST_DATUM(PG_GETARG_DATUM(0));
	PG_LWGEOM *gout;
	double start_measure = PG_GETARG_FLOAT8(1);
	double end_measure = PG_GETARG_FLOAT8(2);
	LWGEOM *lwin, *lwout;
	int type = TYPE_GETTYPE(gin->type);

	/* Raise an error if input is not a linestring or multilinestring */
	if ( type != LINETYPE && type != MULTILINETYPE )
	{
		lwerror("Only LINESTRING and MULTILINESTRING are supported");
		PG_RETURN_NULL();
	}

	lwin = pglwgeom_deserialize(gin);
	if ( type == LINETYPE )
		lwout = (LWGEOM*)lwline_measured_from_lwline((LWLINE*)lwin, start_measure, end_measure);
	else
		lwout = (LWGEOM*)lwmline_measured_from_lwmline((LWMLINE*)lwin, start_measure, end_measure);

	lwgeom_release(lwin);

	if ( lwout == NULL )
		PG_RETURN_NULL();

	gout = pglwgeom_serialize(lwout);
	lwgeom_release(lwout);

	PG_RETURN_POINTER(gout);
}

