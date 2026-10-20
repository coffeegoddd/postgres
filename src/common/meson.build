# Copyright (c) 2022-2026, PostgreSQL Global Development Group

common_sources = files(
  'archive.c',
  'base64.c',
  'binaryheap.c',
  'blkreftable.c',
  'checksum_helper.c',
  'compression.c',
  'controldata_utils.c',
  'encnames.c',
  'exec.c',
  'file_perm.c',
  'file_utils.c',
  'hashfn.c',
  'instr_time.c',
  'ip.c',
  'jsonapi.c',
  'keywords.c',
  'kwlookup.c',
  'link-canary.c',
  'md5_common.c',
  'parse_manifest.c',
  'percentrepl.c',
  'pg_get_line.c',
  'pg_lzcompress.c',
  'pg_parse_lsn.c',
  'pg_prng.c',
  'pgfnames.c',
  'psprintf.c',
  'relpath.c',
  'rmtree.c',
  'saslprep.c',
  'scram-common.c',
  'string.c',
  'stringinfo.c',
  'unicode_case.c',
  'unicode_category.c',
  'unicode_norm.c',
  'username.c',
  'wait_error.c',
  'wchar.c',
)

if ssl.found()
  common_sources += files(
    'cryptohash_openssl.c',
    'hmac_openssl.c',
  )
else
  common_sources += files(
    'cryptohash.c',
    'hmac.c',
    'md5.c',
    'sha1.c',
    'sha2.c',
  )
endif

common_kwlist = custom_target('kwlist',
  input: files('../include/parser/kwlist.h'),
  output: 'kwlist_d.h',
  depend_files: gen_kwlist_deps,
  command: [gen_kwlist_cmd, '--extern'])
generated_sources += common_kwlist
common_sources += common_kwlist

# The code imported from Ryu gets a pass on declaration-after-statement,
# in order to keep it more closely aligned with its upstream.
ryu_sources = files(
  'd2s.c',
  'f2s.c',
)
ryu_cflags = []

ryu_cflags += cflags_no_decl_after_statement

# BLAKE3, imported (README.blake3), likewise; its SIMD variants are built
# with the instructions they use enabled, and chosen at run time.  A variant
# whose intrinsics the compiler can't build with its flags is left out, and
# the dispatcher told so (BLAKE3_NO_*).
blake3_sources = files(
  'blake3.c',
  'blake3_dispatch.c',
  'blake3_portable.c',
)
blake3_simd = {}
blake3_cflags = cflags_no_decl_after_statement
if host_cpu == 'x86' or host_cpu == 'x86_64'
  foreach variant, probe : {
      'sse2': [['-msse2'], 'emmintrin.h',
        '__m128i a = _mm_setzero_si128(); a = _mm_add_epi32(a, a);'],
      'sse41': [['-msse4.1'], 'smmintrin.h',
        '__m128i a = _mm_setzero_si128(); a = _mm_blend_epi16(a, a, 0);'],
      'avx2': [['-mavx2'], 'immintrin.h',
        '__m256i a = _mm256_setzero_si256(); a = _mm256_add_epi32(a, a);'],
      'avx512': [['-mavx512f', '-mavx512vl'], 'immintrin.h',
        '''__m512i a = _mm512_setzero_si512(); __m256i b = _mm256_setzero_si256();
        a = _mm512_add_epi32(a, a); b = _mm256_ror_epi32(b, 7);'''],
    }
    flags = cc.get_id() == 'msvc' ? [] : probe[0]
    code = '''#include <@0@>
int f(void);
int f(void) { @1@ return 0; }
'''.format(probe[1], probe[2])
    if cc.compiles(code, args: flags, name: 'BLAKE3 ' + variant + ' intrinsics')
      blake3_simd += {'blake3_' + variant: flags}
    else
      blake3_cflags += '-DBLAKE3_NO_@0@'.format(variant.to_upper())
    endif
  endforeach
elif host_cpu == 'aarch64' and host_machine.endian() == 'little'
  # (big-endian AArch64 has no NEON variant: the portable code runs)
  blake3_simd += {'blake3_neon': []}
endif
blake3_simd_sources = {}
blake3_simd_cflags = {}
foreach name, flags : blake3_simd
  blake3_simd_sources += {name: files(name + '.c')}
  blake3_simd_cflags += {name: blake3_cflags + flags}
endforeach

config_info_sources = files('config_info.c',)
config_info_cflags = [
  '-DVAL_CC="@0@"'.format(var_cc),
  '-DVAL_CPPFLAGS="@0@"'.format(var_cppflags),
  '-DVAL_CFLAGS="@0@"'.format(var_cflags),
  '-DVAL_CFLAGS_SL="@0@"'.format(var_cflags_sl),
  '-DVAL_LDFLAGS="@0@"'.format(var_ldflags),
  '-DVAL_LDFLAGS_EX="@0@"'.format(var_ldflags_ex),
  '-DVAL_LDFLAGS_SL="@0@"'.format(var_ldflags_sl),
  '-DVAL_LIBS="@0@"'.format(var_libs),
]

# Some files need to be built with different cflags. The different sets are
# defined here.
common_cflags = {
  'ryu': ryu_cflags,
  'config_info': config_info_cflags,
  'blake3': blake3_cflags,
} + blake3_simd_cflags
common_sources_cflags = {
  'ryu': ryu_sources,
  'config_info': config_info_sources,
  'blake3': blake3_sources,
} + blake3_simd_sources


# A few files are currently only built for frontend, not server.
# logging.c is excluded from OBJS_FRONTEND_SHLIB (shared library) as
# a matter of policy, because it is not appropriate for general purpose
# libraries such as libpq to report errors directly.  fe_memutils.c is
# excluded because libpq must not exit() on allocation failure.
#
# The excluded files for _shlib builds are pulled into their own static
# library, for the benefit of test programs that need not follow the
# shlib rules.

common_sources_frontend_shlib = common_sources
common_sources_frontend_shlib += files(
  'restricted_token.c',
  'sprompt.c',
)

common_sources_excluded_shlib = files(
  'fe_memutils.c',
  'logging.c',
)

common_sources_frontend_static = [
  common_sources_frontend_shlib,
  common_sources_excluded_shlib,
]

# Build pgcommon once for backend, once for use in frontend binaries, and
# once for use in shared libraries
#
# XXX: in most environments we could probably link_whole pgcommon_shlib
# against pgcommon_static, instead of compiling twice.
#
# For the server build of pgcommon, depend on lwlocknames_h and because at
# least cryptohash_openssl.c, hmac_openssl.c depend on it.
# controldata_utils.c depends on wait_event_types_h. That's arguably a
# layering violation, but ...
pgcommon = {}
pgcommon_variants = {
  '_srv': internal_lib_args + {
    'sources': common_sources + [lwlocknames_h] + [wait_event_types_h],
    'dependencies': [backend_common_code],
  },
  '': default_lib_args + {
    'sources': common_sources_frontend_static,
    'dependencies': [frontend_common_code],
    # Files in libpgcommon.a should use/export the "xxx_private" versions
    # of pg_char_to_encoding() and friends.
    'c_args': ['-DUSE_PRIVATE_ENCODING_FUNCS'],
  },
  '_shlib': default_lib_args + {
    'pic': true,
    'sources': common_sources_frontend_shlib,
    'dependencies': [frontend_common_code],
    # The JSON API normally exits on out-of-memory; disable that behavior for
    # shared library builds. This requires libpq's pqexpbuffer.h.
    'c_args': ['-DJSONAPI_USE_PQEXPBUFFER'],
    'include_directories': include_directories('../interfaces/libpq'),
  },
}

foreach name, opts : pgcommon_variants

  # Build internal static libraries for sets of files that need to be built
  # with different cflags
  cflag_libs = []
  foreach cflagname, sources : common_sources_cflags
    if sources.length() == 0
      continue
    endif
    c_args = opts.get('c_args', []) + common_cflags[cflagname]
    cflag_libs += static_library('libpgcommon@0@_@1@'.format(name, cflagname),
      c_pch: pch_c_h,
      kwargs: opts + {
        'include_directories': [
          include_directories('.'),
          opts.get('include_directories', []),
        ],
        'sources': sources,
        'c_args': c_args,
        'build_by_default': false,
        'install': false,
      },
    )
  endforeach

  lib = static_library('libpgcommon@0@'.format(name),
      link_with: cflag_libs,
      link_whole: cflag_libs,
      c_pch: pch_c_h,
      kwargs: opts + {
        'include_directories': [
          include_directories('.'),
          opts.get('include_directories', []),
        ],
        'dependencies': opts['dependencies'] + [ssl],
        'install': install_internal_static_lib and name != '_srv',
      }
    )
  pgcommon += {name: lib}
endforeach

common_srv = pgcommon['_srv']
common_shlib = pgcommon['_shlib']
common_static = pgcommon['']

common_excluded_shlib = static_library('libpgcommon_excluded_shlib',
  sources: common_sources_excluded_shlib,
  dependencies: [frontend_common_code],
  build_by_default: false,
  kwargs: default_lib_args + {
    'install': false,
  },
)

subdir('unicode')
