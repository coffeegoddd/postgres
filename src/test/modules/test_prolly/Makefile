# src/test/modules/test_prolly/Makefile

MODULE_big = test_prolly
OBJS = \
	$(WIN32RES) \
	bmap_checks.o \
	bmap_primitive_checks.o \
	branch_conformance.o \
	chunk_index_checks.o \
	chunk_log_checks.o \
	chunk_store_conformance.o \
	test_checksum.o \
	test_prolly.o \
	unit_branch_filter.o \
	unit_bufmgr.o \
	unit_harness.o \
	unit_registry.o \
	unit_smgr.o
PGFILEDESC = "test_prolly - unit tests and interface conformance suites for the version-control engine"

EXTENSION = test_prolly
DATA = test_prolly--1.0.sql

REGRESS = unit conformance bmap_storage bmap_copy_on_write bmap_sharing \
	bmap_tree_share bmap_captured_roots vacuum_shared bmap_primitives \
	bmap_settle_roots bmap_diff bmap_first_write bmap_block_map_stats \
	branch_map_cost chunk_log chunk_index chunk_refs chunk_free chunk_reclaim \
	history_pointers chunk_store_pages chunk_store_memory chunk_pack_reclaim
ISOLATION = bmap_shared_writers chunk_index_put_wait chunk_roots_wait chunk_free_wait \
	chunk_pack_put_wait chunk_vacuum_cancel chunk_pack_pins chunk_take_back
REGRESS_OPTS = --temp-config=$(srcdir)/chunk_store.conf
ISOLATION_OPTS = --temp-config=$(srcdir)/chunk_store_isolation.conf
TAP_TESTS = 1

EXTRA_INSTALL = contrib/amcheck contrib/pageinspect contrib/pg_buffercache contrib/pg_prewarm contrib/pg_visibility \
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

# (these need injection points: known once Makefile.global is in)
ifeq ($(enable_injection_points),yes)
ISOLATION += chunk_pack_order chunk_pack_error
endif
