# contrib/prolly_wal/Makefile

MODULE_big = prolly_wal
OBJS = \
	$(WIN32RES) \
	prolly_wal.o

EXTENSION = prolly_wal
DATA = prolly_wal--1.0.sql
PGFILEDESC = "prolly_wal - a chunk store and roots logged through a custom WAL resource manager"

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/prolly_wal
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
