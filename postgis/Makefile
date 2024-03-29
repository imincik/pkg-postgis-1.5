# **********************************************************************
# * $Id$
# *
# * PostGIS - Spatial Types for PostgreSQL
# * http://postgis.refractions.net
# * Copyright 2008 Mark Cave-Ayland
# *
# * This is free software; you can redistribute and/or modify it under
# * the terms of the GNU General Public Licence. See the COPYING file.
# *
# **********************************************************************

MODULE_big=postgis-1.5
MODULEDIR=contrib/$(MODULE_big)

# Files to be copied to the contrib/ directory
DATA_built=postgis.sql uninstall_postgis.sql postgis_upgrade_15_minor.sql postgis_upgrade_14_to_15.sql postgis_upgrade_13_to_15.sql
DATA=../spatial_ref_sys.sql

# SQL objects (files requiring C pre-processing)
SQL_OBJS=postgis.sql.in uninstall_postgis.sql.in

# PostgreSQL objects
PG_OBJS=lwgeom_pg.o \
	lwgeom_debug.o \
	lwgeom_accum.o \
	lwgeom_spheroid.o \
	lwgeom_ogc.o \
	lwgeom_functions_analytic.o \
	lwgeom_inout.o \
	lwgeom_estimate.o \
	lwgeom_functions_basic.o \
	lwgeom_gist.o \
	lwgeom_btree.o \
	lwgeom_transform.o \
	lwgeom_box.o \
	lwgeom_box3d.o \
	lwgeom_box2dfloat4.o \
	lwgeom_cache.o \
	lwgeom_chip.o \
	lwgeom_geos.o \
	lwgeom_geos_prepared.o \
	lwgeom_export.o \
	lwgeom_svg.o \
	lwgeom_gml.o \
	lwgeom_kml.o \
	lwgeom_geojson.o \
	lwgeom_in_gml.o \
	lwgeom_in_kml.o \
	lwgeom_triggers.o \
	lwgeom_dump.o \
	lwgeom_functions_lrs.o \
	long_xact.o \
	lwgeom_sqlmm.o \
	lwgeom_rtree.o \
	geography_inout.o \
	geography_gist.o \
	geography_btree.o \
	geography_estimate.o \
	geography_measurement.o 

# Objects to build using PGXS
OBJS=$(PG_OBJS)

# Libraries to link into the module (proj, geos)
#
# Note: we specify liblwgeom.a directly in SHLIB_LINK rather than using
# -L... -l options to prevent issues with some platforms trying to link
# to an existing liblwgeom.so in the PostgreSQL $libdir supplied by an
# older version of PostGIS, rather than with the static liblwgeom.a 
# supplied with newer versions of PostGIS
PG_CPPFLAGS+= -I/usr/local/include  -I/opt/local/include/libxml2 -I../liblwgeom
SHLIB_LINK+= -L/usr/local/lib  -lgeos_c -lproj -L/opt/local/lib -lxml2 -lz -lpthread -liconv -lm ../liblwgeom/liblwgeom.a

# Extra files to remove during 'make clean'
EXTRA_CLEAN=$(SQL_OBJS)

# PGXS information
PG_CONFIG = /usr/local/pgsql/9.2/bin/pg_config 
PGXS := /usr/local/pgsql/9.2/lib/pgxs/src/makefiles/pgxs.mk
include $(PGXS)

# PGXS override feature. The ability to allow PostGIS to install itself
# in a versioned directory is only available in PostgreSQL >= 8.5. To
# do this by default on older PostgreSQL versions, we need to override
# the existing PGXS targets.
#
# Once PostgreSQL 8.5 becomes the minimum supported version, this entire
# section and its associated Makefile.pgxs should be removed.
PGXSOVERRIDE = 0
ifeq ($(PGXSOVERRIDE),1)
	include Makefile.pgxs
endif

# If REGRESS=1 passed as a parameter, change the default install paths
# so that no prefix is included. This allows us to relocate to a temporary
# directory for regression testing.
ifeq ($(REGRESS),1)
        bindir=/bin
        pkglibdir=/lib
        datadir=/share
endif


# Borrow the $libdir substitution from PGXS but customise by adding the version number
%.sql: %.sql.in
	sed 's,MODULE_PATHNAME,$$libdir/postgis-1.5,g' $< >$@

postgis_upgrade_15_minor.sql: postgis.sql
	$(PERL) ../utils/postgis_proc_upgrade.pl $< 1.5 > $@

postgis_upgrade_14_to_15.sql: postgis.sql
	$(PERL) ../utils/postgis_proc_upgrade.pl $< 1.4 > $@

postgis_upgrade_13_to_15.sql: postgis.sql
	$(PERL) ../utils/postgis_proc_upgrade.pl $< 1.3 > $@

# Generate any .sql.in files from .sql.in.c files by running them through the C pre-processor 
$(SQL_OBJS): %.in: %.in.c
	$(CPP) -traditional-cpp $< | grep -v '^#' > $@

