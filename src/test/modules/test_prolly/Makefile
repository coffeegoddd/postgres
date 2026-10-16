# src/test/modules/test_prolly/Makefile

MODULE_big = test_prolly
OBJS = \
	$(WIN32RES) \
	applier_checks.o \
	bmap_checks.o \
	branch_checks.o \
	chunkstore_checks.o \
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

REGRESS = test_prolly d1_tables d1_index d1_maintenance prolly_tables versioned_schemas catalog_branch_column branch_catalogs bmap_sharing bmap_copy_on_write bmap_folds branches fold commits status_diff collations applier move_heads merge cherry_pick schema_moves sequences schema_merges table_files incremental_folds checkpoints format_freeze dirty_merges constraint_violations redefined_merges type_moves detached_checkout
TAP_TESTS = 1
EXTRA_INSTALL = contrib/tsm_system_rows contrib/pg_buffercache contrib/pageinspect \
	contrib/pg_walinspect contrib/test_decoding \
	src/test/modules/injection_points

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
