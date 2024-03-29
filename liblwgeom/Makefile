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

CC=gcc
CFLAGS=-g -O2  -fno-common -DPIC  -Wall -Wmissing-prototypes 
NUMERICFLAGS= -ffloat-store

YACC=bison -y
LEX=flex

# Standalone LWGEOM objects
SA_OBJS = \
	measures.o \
	box2d.o \
	ptarray.o \
	lwgeom_api.o \
	lwgeom.o \
	lwpoint.o \
	lwline.o \
	lwpoly.o \
	lwmpoint.o \
	lwmline.o \
	lwmpoly.o \
	lwcollection.o \
	lwcircstring.o \
	lwcompound.o \
	lwcurvepoly.o \
	lwmcurve.o \
	lwmsurface.o \
	lwutil.o \
	lwalgorithm.o \
	lwgunparse.o \
	lwgparse.o \
	lwsegmentize.o \
	wktparse.tab.o \
	lex.yy.o \
	vsprintf.o \
	g_box.o \
	g_coord.o \
	g_geometry.o \
	g_ptarray.o \
	g_serialized.o \
	g_util.o \
	lwgeodetic.o \
	lwtree.o

NM_OBJS = \
	lwspheroid.o 

SA_HEADERS = \
	liblwgeom.h \
	lwalgorithm.h

all: liblwgeom.a

liblwgeom.a: $(SA_OBJS) $(NM_OBJS) $(SA_HEADERS) 
	ar rs liblwgeom.a $(SA_OBJS) $(NM_OBJS) 	

clean:
	rm -f $(SA_OBJS) 
	rm -f $(NM_OBJS) 
	rm -f liblwgeom.a 

check: liblwgeom.a
	make -C cunit check

# Command to build each of the .o files
$(SA_OBJS): %.o: %.c 
	$(CC) $(CFLAGS) -c -o $@ $<

# Command to build each of the .o files
$(NM_OBJS): %.o: %.c 
	$(CC) $(CFLAGS) $(NUMERICFLAGS) -c -o $@ $<


# Commands to generate the lexer and parser from input files
wktparse.tab.c: wktparse.y
	$(YACC) -vd -p lwg_parse_yy wktparse.y
	mv -f y.tab.c wktparse.tab.c
	mv -f y.tab.h wktparse.tab.h
 
lex.yy.c: wktparse.lex wktparse.tab.c
	$(LEX) -Plwg_parse_yy -i -f -o'lex.yy.c' wktparse.lex
 
