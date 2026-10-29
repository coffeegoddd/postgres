# contrib/prolly_chunkpages/Makefile

MODULE_big = prolly_chunkpages
OBJS = \
	$(WIN32RES) \
	prolly_chunkpages.o

EXTENSION = prolly_chunkpages
DATA = prolly_chunkpages--1.0.sql
PGFILEDESC = "prolly_chunkpages - a chunk store in pages of its own layout"

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/prolly_chunkpages
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
