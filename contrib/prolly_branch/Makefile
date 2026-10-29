# contrib/prolly_branch/Makefile

MODULE_big = prolly_branch
OBJS = \
	$(WIN32RES) \
	prolly_branch.o

EXTENSION = prolly_branch
DATA = prolly_branch--1.0.sql
PGFILEDESC = "prolly_branch - branches as relation files, switched in the catalog"

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/prolly_branch
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
