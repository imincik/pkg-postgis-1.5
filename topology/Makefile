#
# PostGIS PGXS build system
#

# Files to be copied to the contrib/ directory
DATA_built=topology.sql

# SQL objects (files requiring C pre-processing)
SQL_OBJS=topology.sql.in

# Extra files to remove during 'make clean'
EXTRA_CLEAN=$(SQL_OBJS)

# PGXS information
PG_CONFIG = /usr/local/pgsql/9.2/bin/pg_config 
PGXS := /usr/local/pgsql/9.2/lib/pgxs/src/makefiles/pgxs.mk
include $(PGXS)


# Unfortunately we have to copy this from the PGXS Makefile as it only gets picked up
# if MODULE_big is defined
%.sql: %.sql.in
	sed 's,MODULE_PATHNAME,$$libdir/$*,g' $< >$@

# Generate any .sql.in files from .sql.in.c files by running them through the C pre-processor 
$(SQL_OBJS): %.in: %.in.c
	$(CPP) -traditional-cpp $< | grep -v '^#' > $@

