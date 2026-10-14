# Copyright (c) 2026, PostgreSQL Global Development Group

test_prolly_sources = files(
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
    ],
  },
  'tap': {
    'tests': [
      't/001_store_recovery.pl',
      't/002_d1_heads.pl',
      't/003_d1_cleanup.pl',
      't/004_d1_databases.pl',
      't/005_d1_pending.pl',
      't/006_d1_head_limit.pl',
    ],
  },
}
