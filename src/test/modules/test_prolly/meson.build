# Copyright (c) 2026, PostgreSQL Global Development Group

test_prolly_sources = files(
  'bmap_checks.c',
  'bmap_primitive_checks.c',
  'test_checksum.c',
  'test_prolly.c',
  'unit_bufmgr.c',
  'unit_harness.c',
  'unit_registry.c',
  'unit_smgr.c',
)

if host_system == 'windows'
  test_prolly_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_prolly',
    '--FILEDESC', 'test_prolly - unit tests and interface conformance suites for the version-control engine',])
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
      'unit',
      'conformance',
      'bmap_storage',
      'bmap_copy_on_write',
      'bmap_sharing',
      'bmap_tree_share',
      'bmap_captured_roots',
      'vacuum_shared',
      'bmap_primitives',
      'bmap_settle_roots',
      'bmap_diff',
    ],
  },
  'tap': {
    'tests': [
      't/001_preloaded.pl',
      't/002_bmap_storage.pl',
      't/003_bmap_incremental_backup.pl',
      't/004_bmap_share.pl',
      't/005_bmap_share_limits.pl',
      't/006_bmap_replaced_blocks.pl',
      't/007_bmap_shared_changes.pl',
      't/008_bmap_drop_replay.pl',
      't/009_bmap_evictions.pl',
      't/010_bmap_evict_lock.pl',
      't/011_bmap_relfilenumber_reuse.pl',
      't/012_bmap_tree.pl',
      't/013_bmap_dropped_buffers.pl',
      't/014_bmap_release_window.pl',
      't/017_vm_set_logical_decoding.pl',
      't/018_autovacuum_freezes_eagerly.pl',
      't/019_bmap_share_maps_minimal.pl',
      't/022_bmap_root_horizon.pl',
      't/015_bmap_release_autovacuum.pl',
      't/016_bmap_drop_buffers_two_passes.pl',
      't/020_bmap_share_wal.pl',
      't/021_bmap_share_redo.pl',
    ],
    'env': {
      'enable_injection_points': get_option('injection_points') ? 'yes' : 'no',
    },
  },
}
