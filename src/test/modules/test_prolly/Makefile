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

EXTENSION = test_prolly test_prolly_normalizing test_prolly_ordering test_prolly_a test_prolly_b test_prolly_ext_table \
	test_prolly_ext_detach test_prolly_ext_readd test_prolly_ext_config test_prolly_fixed
DATA = test_prolly_normalizing--1.0.sql test_prolly_ordering--1.0.sql test_prolly_ordering--1.0--1.1.sql \
	test_prolly--1.0.sql test_prolly_a--1.0.sql test_prolly_b--1.0.sql \
	test_prolly_b--1.0--1.1.sql test_prolly_b--1.1.sql test_prolly_ext_table--1.0.sql \
	test_prolly_ext_detach--1.0.sql test_prolly_ext_readd--1.0.sql test_prolly_ext_config--1.0.sql \
	test_prolly_fixed--1.0.sql

REGRESS = test_prolly content_hashes prolly_tables versioned_schemas catalog_branch_column branch_catalogs bmap_sharing bmap_copy_on_write bmap_folds branches fold commits branch_writes writer_modes transaction_commits transaction_commit_cases txn_commit_baselines commit_messages status_diff schema_diffs diff_stat diff_history diff_by_name as_of identity_renames collations applier move_heads merge cell_merges field_merges conflict_rows cherry_pick merge_options revert_series branch_rename schema_moves sequences schema_merges schema_conflicts merge_truth table_files conjoin_choice incremental_folds change_log log_folds checkpoints format_freeze dirty_merges commit_root kept_checkouts kept_planned share_layouts checkout_table_work constraint_violations keyless_violations verify_constraints domain_violations violation_storage redefined_merges retyped_merges type_moves detached_checkout branch_start keyless_moves head_only_branches clean_checkouts keyless_keys walks remotes stored_chunks remote_clone clone_whole transfer_spill dependencies dependency_moves extension_ordering extension_ordering_nested shared_drops shared_alters versioned_extensions extension_moves extension_clone extension_config_rows text_types versioned_event_triggers internal_move_events foreign_tables canonical_escapes remote_serve commit_heights aggregates policies partitions append_check journal_segments journal_compression gc gc_full gc_spill gc_declared gc_file_size page_sweep truncated_blocks large_values large_keys jsonb_canonical float_canonical send_canonical receive_refusals hash_pointers toast_identity toast_by_content toast_folds redefined_rows enum_history range_types undescribed_objects statistics_casts doltgres_vc doltgres_objects doltgres_fks_auth matview_merges matview_status described_properties table_settings stat_prolly stat_prolly_moves chunk_cache vacuum_shared vacuum_released eager_freeze full_folds spilled_folds incremental_canon shape_folds truncate_folds bulk_notes fold_verify fold_caches compared_folds fold_on_commit row_locations describe_caches nearest_source near_sources reload_moves discarded_storage keyless_toast_folds store_sizes storage_breakdown get_many put_batches index_tablespaces diff_memory stash reset_modes checkout_tables branch_maps reflog bulk_moves retype_moves tags rebase rewrite_folds bmap_tree_share bmap_tree_snapshots kept_pages first_use_kept kept_commit_shares branch_catalog_index snapshot_mounts history_mounts head_move_privileges writer_definitions lineage_breaks
REGRESS_OPTS = --temp-config $(top_srcdir)/src/test/modules/test_prolly/test_prolly.conf
TAP_TESTS = 1
# (no autovacuum folds but where a test turns them on: autofold_off.conf,
# below)
EXTRA_INSTALL = contrib/amcheck contrib/btree_gist contrib/cube contrib/seg contrib/citext contrib/hstore contrib/ltree contrib/tsm_system_rows contrib/pg_buffercache contrib/pageinspect contrib/pg_prewarm \
	contrib/pg_walinspect contrib/postgres_fdw contrib/test_decoding \
	src/test/modules/injection_points src/test/modules/dummy_seclabel \
	src/test/modules/xid_wraparound

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

# (an environment's TEMP_CONFIG, as CI's, kept, with these settings after
# it: TEMP_CONFIG ?= would let it replace them)
# (a make this one runs inherits it as made: not made again from itself)
TAP_TEMP_CONFIG := $(abs_top_builddir)/$(subdir)/tap_temp.conf
ifneq ($(TEMP_CONFIG),$(TAP_TEMP_CONFIG))
$(shell mkdir -p '$(dir $(TAP_TEMP_CONFIG))' && { $(if $(TEMP_CONFIG),cat '$(TEMP_CONFIG)' &&) cat '$(abs_top_srcdir)/$(subdir)/autofold_off.conf'; } > '$(TAP_TEMP_CONFIG).new' && mv '$(TAP_TEMP_CONFIG).new' '$(TAP_TEMP_CONFIG)')
endif
override TEMP_CONFIG := $(TAP_TEMP_CONFIG)
export TEMP_CONFIG

