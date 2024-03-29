# **********************************************************************
# * $Id: Makefile.in
# *
# * PostGIS - Spatial Types for PostgreSQL
# * http://postgis.refractions.net
# * Copyright 2008 Paul Ramsey, Mark Cave-Ayland
# *
# * This is free software; you can redistribute and/or modify it under
# * the terms of the GNU General Public Licence. See the COPYING file.
# *
# **********************************************************************

CC=gcc
CFLAGS=-g -O2  -Wall -Wmissing-prototypes

CUNIT_LDFLAGS= -lcunit
CUNIT_CPPFLAGS= -I..

OBJS=	\
	cu_algorithm.o \
	cu_geodetic.o \
	cu_measures.o \
	cu_libgeom.o \
	cu_tester.o 

# If we couldn't find the cunit library then display a helpful message
ifeq ($(CUNIT_LDFLAGS),)
all: requirements_not_met_cunit
check: requirements_not_met_cunit
else

# Build the unit tester
all: cu_tester

# Build and run the unit tester
check: cu_tester
	@./cu_tester

endif

# Build the main unit test executable
cu_tester: ../liblwgeom.a $(OBJS)
	$(CC) -o $@ $(OBJS) ../liblwgeom.a -lm $(CUNIT_LDFLAGS)

# Command to build each of the .o files
$(OBJS): %.o: %.c
	$(CC) $(CFLAGS) $(CUNIT_CPPFLAGS) -c -o $@ $<

# Clean target
clean:
	@rm $(OBJS)
	@rm cu_tester

# Requirements message
requirements_not_met_cunit:
	@echo
	@echo "WARNING:"
	@echo
	@echo "configure was unable to find CUnit which is required for unit testing."
	@echo "In order to enable unit testing, you must install CUnit and then re-run configure."
	@echo
