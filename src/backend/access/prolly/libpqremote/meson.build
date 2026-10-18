# Copyright (c) 2026, PostgreSQL Global Development Group

prolly_remote_libpq_sources = files(
  'prolly_remote_libpq.c',
)

if host_system == 'windows'
  prolly_remote_libpq_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'prolly_remote_libpq',
    '--FILEDESC', 'prolly_remote_libpq - prolly remotes over a libpq connection',])
endif

prolly_remote_libpq = shared_module('prolly_remote_libpq',
  prolly_remote_libpq_sources,
  kwargs: pg_mod_args + {
    'dependencies': pg_mod_args['dependencies'] + [libpq],
  }
)

backend_targets += prolly_remote_libpq
