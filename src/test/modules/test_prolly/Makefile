# src/test/modules/test_prolly/Makefile

MODULE_big = test_prolly
OBJS = \
	$(WIN32RES) \
	applier_checks.o \
	bmap_checks.o \
	branch_checks.o \
	chunkstore_checks.o \
	catalog_checks.o \
	foundation_checks.o \
	keying_checks.o \
	tree_checks.o \
	test_prolly.o
PGFILEDESC = "test_prolly - conformance suites for prolly interfaces"

EXTENSION = test_prolly test_prolly_a test_prolly_b
DATA = test_prolly--1.0.sql test_prolly_a--1.0.sql test_prolly_b--1.0.sql \
	test_prolly_b--1.0--1.1.sql test_prolly_b--1.1.sql

REGRESS = test_prolly prolly_tables versioned_schemas catalog_branch_column branch_catalogs bmap_sharing bmap_copy_on_write bmap_folds branches fold commits branch_writes writer_modes transaction_commits commit_messages status_diff collations applier move_heads merge cell_merges conflict_rows cherry_pick merge_options revert_series branch_rename schema_moves sequences schema_merges merge_truth table_files incremental_folds checkpoints format_freeze dirty_merges constraint_violations verify_constraints domain_violations redefined_merges type_moves detached_checkout branch_start keyless_moves head_only_branches clean_checkouts keyless_keys walks remotes remote_clone dependencies dependency_moves canonical_escapes remote_serve aggregates policies partitions append_check journal_segments gc gc_full page_sweep large_values jsonb_canonical float_canonical hash_pointers toast_by_content toast_folds redefined_rows range_types undescribed_objects statistics_casts doltgres_vc doltgres_objects doltgres_fks_auth matview_merges stat_prolly index_tablespaces
TAP_TESTS = 1
EXTRA_INSTALL = contrib/citext contrib/hstore contrib/ltree contrib/tsm_system_rows contrib/pg_buffercache contrib/pageinspect \
	contrib/pg_walinspect contrib/test_decoding \
	src/test/modules/injection_points src/test/modules/dummy_seclabel

export enable_injection_points
export with_ssl

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
