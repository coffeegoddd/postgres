# Copyright (c) 2026, PostgreSQL Global Development Group
#
# The requirements suite, run on heap.  Running it on another table access
# method is done with the Makefile's PROLLY_AM.

tests += {
  'name': 'prolly',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'schedule': files('semantics_schedule'),
    'regress_args': ['--temp-config', files('am/heap.conf')],
  },
  'isolation': {
    'schedule': files('isolation_schedule'),
    'regress_args': ['--temp-config', files('am/heap.conf')],
  },
  'tap': {
    'env': {'PROLLY_AM': 'heap'},
    'tests': [
      't/000_target.pl',
      't/100_restart.pl',
    ],
  },
}

# Branched catalogs, which do not depend on the table access method.
tests += {
  'name': 'prolly_branches',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'schedule': files('branches_schedule'),
    'regress_args': ['--temp-config', files('am/heap.conf')],
  },
  'isolation': {
    'schedule': files('branches_isolation_schedule'),
    'regress_args': ['--temp-config', files('am/heap.conf')],
  },
  'tap': {
    'env': {'PROLLY_AM': 'heap'},
    'tests': [
      't/200_branch_schema_names.pl',
      't/201_branch_sessions.pl',
      't/202_branch_decoding.pl',
    ],
  },
}
