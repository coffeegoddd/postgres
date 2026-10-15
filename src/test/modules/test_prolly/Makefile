# src/test/modules/test_prolly/Makefile

MODULE_big = test_prolly
OBJS = \
	$(WIN32RES) \
	bmap_checks.o \
	branch_checks.o \
	catalog_checks.o \
	engine_checks.o \
	foundation_checks.o \
	storage_checks.o \
	tree_checks.o \
	rootlayout_checks.o \
	test_prolly.o
PGFILEDESC = "test_prolly - conformance suites for prolly interfaces"

EXTENSION = test_prolly
DATA = test_prolly--1.0.sql

REGRESS = test_prolly d1_tables d1_index d1_maintenance prolly_tables versioned_schemas catalog_branch_column branch_catalogs bmap_sharing
TAP_TESTS = 1
EXTRA_INSTALL = contrib/tsm_system_rows src/test/modules/injection_points

export enable_injection_points

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_prolly
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
