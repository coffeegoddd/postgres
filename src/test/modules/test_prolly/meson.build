# Copyright (c) 2026, PostgreSQL Global Development Group

test_prolly_sources = files(
  'applier_checks.c',
  'bmap_checks.c',
  'branch_checks.c',
  'chunkstore_checks.c',
  'catalog_checks.c',
  'engine_checks.c',
  'foundation_checks.c',
  'storage_checks.c',
  'tree_checks.c',
  'rootlayout_checks.c',
  'test_prolly.c',
)

if host_system == 'windows'
  test_prolly_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_prolly',
    '--FILEDESC', 'test_prolly - conformance suites for prolly interfaces',])
endif

test_prolly = shared_module('test_prolly',
  test_prolly_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_prolly

test_install_data += files(
  'test_prolly.control',
  'test_prolly--1.0.sql',
)

tests += {
  'name': 'test_prolly',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'test_prolly',
      'd1_tables',
      'd1_index',
      'd1_maintenance',
      'prolly_tables',
      'versioned_schemas',
      'catalog_branch_column',
      'branch_catalogs',
      'bmap_sharing',
      'bmap_copy_on_write',
      'branches',
      'fold',
      'commits',
      'status_diff',
      'collations',
      'applier',
      'move_heads',
      'merge',
      'cherry_pick',
    ],
  },
  'tap': {
    'env': {
       'enable_injection_points': get_option('injection_points') ? 'yes' : 'no',
    },
    'tests': [
      't/001_store_recovery.pl',
      't/002_d1_heads.pl',
      't/003_d1_cleanup.pl',
      't/004_d1_databases.pl',
      't/005_d1_pending.pl',
      't/006_d1_head_limit.pl',
      't/007_d1_commit_capacity.pl',
      't/008_d1_writers.pl',
      't/009_d1_index.pl',
      't/010_d1_maintenance.pl',
      't/011_d1_respelled_keys.pl',
      't/012_d1_snapshots.pl',
      't/013_d1_row_locks.pl',
      't/014_d1_spill.pl',
      't/015_d1_serializable.pl',
      't/016_d1_large_values.pl',
      't/017_d1_gc.pl',
      't/018_d1_vacuum.pl',
      't/019_d1_branches.pl',
      't/020_d1_merge.pl',
      't/021_d1_canonical.pl',
      't/022_d1_dolt_sql.pl',
      't/023_d1_conflicts.pl',
      't/024_versioned_database.pl',
      't/025_d1_relation_trees.pl',
      't/026_d1_tid_reuse_branches.pl',
      't/027_d1_counter_crash.pl',
      't/028_bmap_storage.pl',
      't/029_versioned_schema_dump.pl',
      't/030_branch_connect.pl',
      't/031_bmap_share.pl',
      't/032_branches.pl',
      't/033_branch_maintenance.pl',
      't/034_bmap_share_limits.pl',
      't/035_bmap_replaced_blocks.pl',
      't/036_bmap_shared_changes.pl',
      't/037_branch_locks.pl',
      't/038_chunk_store.pl',
    ],
  },
}
