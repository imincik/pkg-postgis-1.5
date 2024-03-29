# **********************************************************************
# * $Id: Makefile.in
# *
# * PostGIS - Spatial Types for PostgreSQL
# * http://postgis.refractions.net
# * Copyright 2008 Mark Cave-Ayland
# *
# * This is free software; you can redistribute and/or modify it under
# * the terms of the GNU General Public Licence. See the COPYING file.
# *
# **********************************************************************

# PGXS information
#
# Note that PGXS currently doesn't handle building FE executables, but we need
# the INSTALL and DESTDIR variables so we can get the correct install paths. 
# Hence we include the PGXS Makefile here, but ensure that we override the
# 'all' and 'install' targets with the ones we really want to use below.
PG_CONFIG = /usr/local/pgsql/9.2/bin/pg_config
PGXS := /usr/local/pgsql/9.2/lib/pgxs/src/makefiles/pgxs.mk
include $(PGXS)

# Set CFLAGS afer PGXS, otherwise it will get overwritten with the PGXS
# version which is not what we want. 
CC=gcc
CFLAGS=-g -O2  -fno-common -DPIC  -Wall -Wmissing-prototypes

# Filenames with extension as determined by the OS
PGSQL2SHP=pgsql2shp
SHP2PGSQL-CLI=shp2pgsql
SHP2PGSQL-GUI=shp2pgsql-gui

# PostgreSQL frontend CPPFLAGS and LDFLAGS (for compiling and linking with libpq)
PGSQL_FE_CPPFLAGS=-I/usr/local/pgsql/9.2/include
PGSQL_FE_LDFLAGS=-L/usr/local/pgsql/9.2/lib -lpq

# iconv flags
ICONV_LDFLAGS= -liconv
ICONV_CFLAGS=

# liblwgeom
LIBLWGEOM=../liblwgeom/liblwgeom.a

# GTK includes and libraries
GTK_CFLAGS =  
GTK_LIBS =  
GTK_WIN32_FLAGS = 
GTK_WIN32_RES = 

# If REGRESS=1 passed as a parameter, change the default install paths
# so that no prefix is included. This allows us to relocate to a temporary
# directory for regression testing.
ifeq ($(REGRESS),1)
	bindir=/bin
	pkglibdir=/lib
	datadir=/share
endif

# We test this variable later to see if we're building the GUI
gtk_build = 

# The real parts of the Makefile
all: $(SHP2PGSQL-CLI) $(PGSQL2SHP) 

gui: $(SHP2PGSQL-GUI) $(SHP2PGSQL-CLI) 

shp2pgsql-gui.res: shp2pgsql-gui.rc shp2pgsql-gui.ico
	windres $< -O coff -o $@

# liblwgeom.a dependency to allow "make install" in 
# the loader/ subdirectory to work  
$(LIBLWGEOM):
	make -C ../liblwgeom

shp2pgsql-core.o: shp2pgsql-core.c
	$(CC) $(CFLAGS) $(ICONV_CFLAGS) -c $<

pgsql2shp.o: pgsql2shp.c
	$(CC) $(CFLAGS) $(ICONV_CFLAGS) $(PGSQL_FE_CPPFLAGS) -c $<

$(PGSQL2SHP): shpopen.o dbfopen.o safileio.o getopt.o pgsql2shp.o $(LIBLWGEOM) 
	$(CC) $(CFLAGS) $^ $(ICONV_LDFLAGS) $(PGSQL_FE_LDFLAGS) -lm -o $@ 

$(SHP2PGSQL-CLI): stringbuffer.o shpopen.o dbfopen.o safileio.o getopt.o shp2pgsql-core.o shp2pgsql-cli.o $(LIBLWGEOM) 
	$(CC) $(CFLAGS) $^ -o $@ $(ICONV_LDFLAGS) -lm 

shp2pgsql-gui.o: shp2pgsql-gui.c
	$(CC) $(CFLAGS) $(PGSQL_FE_CPPFLAGS) $(GTK_CFLAGS) -o $@ -c shp2pgsql-gui.c

$(SHP2PGSQL-GUI): stringbuffer.o shpopen.o dbfopen.o safileio.o shp2pgsql-core.o shp2pgsql-gui.o getopt.o $(LIBLWGEOM) $(GTK_WIN32_RES)
	$(CC) $(CFLAGS) $(GTK_WIN32_FLAGS) $^ -o $@ $(GTK_LIBS) $(ICONV_LDFLAGS) $(PGSQL_FE_LDFLAGS) -lm 

installdir:
	@mkdir -p $(DESTDIR)$(bindir)

install: installdir 
ifdef gtk_build
	$(INSTALL) $(SHP2PGSQL-GUI) $(DESTDIR)$(bindir)
endif
	$(INSTALL) $(PGSQL2SHP) $(DESTDIR)$(bindir)
	$(INSTALL) $(SHP2PGSQL-CLI) $(DESTDIR)$(bindir)

uninstall:
	@rm -f $(DESTDIR)$(bindir)/$(PGSQL2SHP)
	@rm -f $(DESTDIR)$(bindir)/$(SHP2PGSQL-CLI)

clean:
	@rm -f *.o $(SHP2PGSQL-CLI) $(PGSQL2SHP) $(GTK_WIN32_RES)

