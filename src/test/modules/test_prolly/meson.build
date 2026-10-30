# Copyright (c) 2026, PostgreSQL Global Development Group

test_prolly_sources = files(
  'bmap_checks.c',
  'bmap_primitive_checks.c',
  'branch_conformance.c',
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
      'bmap_first_write',
      'bmap_block_map_stats',
      'branch_map_cost',
    ],
  },
  'isolation': {
    'specs': [
      'bmap_shared_writers',
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
      't/031_bmap_refs.pl',
      't/023_bmap_held_root_reuse.pl',
      't/024_bmap_sweep_incremental.pl',
      't/025_bmap_sparse_map.pl',
      't/026_bmap_checksums.pl',
      't/027_bmap_verifybackup.pl',
      't/028_bmap_rewind.pl',
      't/029_bmap_upgrade.pl',
      't/030_bmap_crash_steps.pl',
      't/032_bmap_share_synced.pl',
      't/033_bmap_rewind_synced.pl',
      't/034_bmap_copy_without_image.pl',
      't/035_bmap_share_maps.pl',
      't/036_bmap_share_vm_images.pl',
      't/037_bmap_share_free_space.pl',
      't/038_bmap_copy_images_default.pl',
      't/039_bmap_sequences.pl',
      't/040_bmap_share_index_ams.pl',
      't/041_bmap_pitr_steps.pl',
      't/042_bmap_orphan_maps.pl',
      't/043_bmap_shared_writers.pl',
      't/044_bmap_standby_hold.pl',
      't/045_bmap_first_share_walk.pl',
      't/046_bmap_reused_stale_since.pl',
      't/047_bmap_passed_restart.pl',
      't/048_bmap_stats_movedb.pl',
    ],
    'env': {
      'enable_injection_points': get_option('injection_points') ? 'yes' : 'no',
    },
  },
}
