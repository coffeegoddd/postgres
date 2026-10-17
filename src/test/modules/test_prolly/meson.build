# Copyright (c) 2026, PostgreSQL Global Development Group

test_prolly_sources = files(
  'applier_checks.c',
  'bmap_checks.c',
  'branch_checks.c',
  'chunkstore_checks.c',
  'catalog_checks.c',
  'foundation_checks.c',
  'keying_checks.c',
  'tree_checks.c',
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
      'prolly_tables',
      'versioned_schemas',
      'catalog_branch_column',
      'branch_catalogs',
      'bmap_sharing',
      'bmap_copy_on_write',
      'bmap_folds',
      'branches',
      'fold',
      'commits',
      'status_diff',
      'collations',
      'applier',
      'move_heads',
      'merge',
      'cherry_pick',
      'schema_moves',
      'sequences',
      'schema_merges',
      'table_files',
      'incremental_folds',
      'checkpoints',
      'format_freeze',
      'dirty_merges',
      'constraint_violations',
      'redefined_merges',
      'type_moves',
      'detached_checkout',
      'branch_start',
      'keyless_moves',
      'head_only_branches',
      'clean_checkouts',
      'keyless_keys',
      'walks',
      'remotes',
      'remote_clone',
      'dependencies',
    ],
  },
  'tap': {
    'env': {
       'enable_injection_points': get_option('injection_points') ? 'yes' : 'no',
    },
    'tests': [
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
      't/039_branch_commits.pl',
      't/040_table_files.pl',
      't/041_bmap_folds.pl',
      't/042_fold_unseen.pl',
      't/043_bmap_drop_replay.pl',
      't/044_merge_violations.pl',
      't/045_remotes.pl',
      't/046_branch_crash.pl',
    ],
  },
}
