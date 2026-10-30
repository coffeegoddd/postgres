# Copyright (c) 2026, PostgreSQL Global Development Group

test_prolly_sources = files(
  'test_checksum.c',
  'test_prolly.c',
  'unit_harness.c',
  'unit_registry.c',
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
    ],
  },
  'tap': {
    'tests': [
      't/001_preloaded.pl',
    ],
  },
}
