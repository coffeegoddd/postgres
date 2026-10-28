# contrib/prolly_pages/Makefile

MODULE_big = prolly_pages
OBJS = \
	$(WIN32RES) \
	prolly_pages.o

EXTENSION = prolly_pages
DATA = prolly_pages--1.0.sql
PGFILEDESC = "prolly_pages - write whole pages back into relations"

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/prolly_pages
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
