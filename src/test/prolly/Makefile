#-------------------------------------------------------------------------
#
# Makefile for src/test/prolly
#
# Requirements suite for prolly storage.  Not part of "make check" or
# "make check-world": run it with
#
#	make -C src/test/prolly check
#
# (PROLLY_AM is the table AM under test, prolly by default.  check-vc and
# check-vc-isolation run on prolly whatever PROLLY_AM is, after
# check-vc-target passes on it.)
#
# The semantics, isolation and durability groups also run against heap
# (PROLLY_AM=heap), which is how the tests themselves are validated: their
# expected output is what stock Postgres produces.  See README.
#
# src/test/prolly/Makefile
#
#-------------------------------------------------------------------------

subdir = src/test/prolly
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

PROLLY_AM ?= prolly
export PROLLY_AM

# TAP groups share tmp_check and the temporary installation.
.NOTPARALLEL:

# Server settings for every instance the suite starts (no autovacuum folds:
# they would land in the middle of tests that count folds).
prolly_am.conf: FORCE
	@{ echo "default_table_access_method = '$(PROLLY_AM)'"; \
	   echo "max_prepared_transactions = 10"; \
	   echo "prolly.autofold_threshold = 0"; \
	   echo "prolly_fold_worker = off"; } > $@

# Version control of branches made by copying: only the page-based engine
# keeps branches as copies, so these tests' tables are prolly's whatever
# PROLLY_AM is.
vc.conf: FORCE
	@{ echo "default_table_access_method = 'prolly'"; \
	   echo "max_prepared_transactions = 10"; \
	   echo "prolly.autofold_threshold = 0"; \
	   echo "prolly_fold_worker = off"; } > $@

# The TAP tests' instances (TEMP_CONFIG), no autovacuum folds either.
# (an environment's TEMP_CONFIG, as CI's, kept, with these settings after
# it: TEMP_CONFIG ?= would let it replace them)
# (a make this one runs inherits it as made: not made again from itself)
TAP_TEMP_CONFIG := $(abs_top_builddir)/$(subdir)/tap_temp.conf
ifneq ($(TEMP_CONFIG),$(TAP_TEMP_CONFIG))
$(shell mkdir -p '$(dir $(TAP_TEMP_CONFIG))' && { $(if $(TEMP_CONFIG),cat '$(TEMP_CONFIG)' &&) cat '$(abs_top_srcdir)/$(subdir)/autofold_off.conf'; } > '$(TAP_TEMP_CONFIG).new' && mv '$(TAP_TEMP_CONFIG).new' '$(TAP_TEMP_CONFIG)')
endif
override TEMP_CONFIG := $(TAP_TEMP_CONFIG)
export TEMP_CONFIG


# Heap tables in block-mapped storage instead (debug_block_mapped_heap).
bmap.conf: FORCE
	@{ echo "debug_block_mapped_heap = on"; \
	   echo "max_prepared_transactions = 10"; } > $@

EXTRA_INSTALL = contrib/btree_gist src/test/modules/injection_points

# (TAP tests that stop a fold in its middle need injection points)
export enable_injection_points

REGRESS_OPTS = --temp-config=$(CURDIR)/prolly_am.conf

CHECK_TARGETS = check-target check-semantics check-isolation check-vc \
	check-vc-isolation check-tap

check: $(CHECK_TARGETS)

check-target: PROVE_TESTS = t/003_target_guard.pl
check-target: require-tap temp-install
	$(prove_check)

check-vc-target: PROVE_TESTS = t/004_vc_target.pl
check-vc-target: require-tap temp-install
	$(prove_check)

check-vc-target-guard: PROVE_TESTS = t/005_vc_target_guard.pl
check-vc-target-guard: require-tap temp-install
	$(prove_check)

check-semantics: PROVE_TESTS = t/001_semantics.pl
check-semantics: require-tap temp-install
	$(prove_check)

# The VC tests on prolly claim its coverage only after the probe passes
# on it.
# (on prolly, the same target as check-vc-target, so make runs it once)
ifeq ($(PROLLY_AM),prolly)
check-vc-target-prolly: check-vc-target
else
check-vc-target-prolly: temp-install
	$(MAKE) check-vc-target PROLLY_AM=prolly
endif

check-vc: temp-install check-vc-target-prolly vc.conf
	$(pg_regress_check) --temp-config=$(CURDIR)/vc.conf \
		--schedule=$(srcdir)/vc_schedule

check-isolation: PROVE_TESTS = t/002_isolation.pl
check-isolation: require-tap temp-install
	$(MAKE) -C $(top_builddir)/src/test/isolation all
	$(prove_check)

check-vc-isolation: temp-install check-vc-target-prolly vc.conf
	$(MAKE) -C $(top_builddir)/src/test/isolation all
	$(pg_isolation_regress_check) --temp-config=$(CURDIR)/vc.conf \
		--schedule=$(srcdir)/vc_isolation_schedule

# TAP tests: 1xx are durability (also run against heap), 2xx version control.
check-tap: check-tap-durability check-tap-vc

check-tap-durability: PROVE_TESTS = t/1*.pl
check-tap-durability: require-tap temp-install
	$(prove_check)

# (PostGIS's TAP tests aside: they run in check-postgis)
check-tap-vc: PROVE_TESTS = $(filter-out $(POSTGIS_TAP),$(patsubst $(srcdir)/%,%,$(wildcard $(srcdir)/t/2*.pl)))
check-tap-vc: check-vc-target require-tap temp-install
	$(prove_check)

# Postgres's regression, isolation and recovery suites with every user
# table, index and TOAST table in block-mapped storage.  These pass in
# full: block-mapped storage changes where pages live, not what they hold.
check-upstream-bmap: temp-install bmap.conf
	$(MAKE) -C $(top_builddir)/src/test/recovery checkprep
	$(MAKE) -C $(top_builddir)/src/test/regress check TEMP_CONFIG=$(CURDIR)/bmap.conf
	$(MAKE) -C $(top_builddir)/src/test/isolation check TEMP_CONFIG=$(CURDIR)/bmap.conf
	$(MAKE) -C $(top_builddir)/src/test/recovery check TEMP_CONFIG=$(CURDIR)/bmap.conf

# PostGIS on versioned tables: what PostGIS does on heap, it does on prolly
# (check-postgis-semantics, also run against heap), and its values and
# indexes survive commits, moves and remotes (check-vc-postgis, always on
# prolly).  PostGIS isn't part of this tree: POSTGIS_BUILD names a build of
# the pinned PostGIS against this tree's installation prefix, made by
#
#	python3 src/tools/prolly_bench/build_postgis.py \
#	    --pg-config PREFIX/bin/pg_config --build-dir DIR
#
# and is installed from there into the temporary installation.
POSTGIS_BUILD ?=

# PostGIS's TAP tests: concurrency and durability on the AM under test,
# network remotes on prolly
POSTGIS_TAP = t/295_postgis_isolation.pl t/296_postgis_crash.pl \
	t/297_postgis_network.pl t/299_postgis_regress.pl

check-postgis: check-postgis-semantics check-postgis-isolation \
	check-postgis-durability check-vc-postgis check-tap-vc-postgis

postgis-install: temp-install
	@test -n '$(POSTGIS_BUILD)' || { echo 'Set POSTGIS_BUILD to a build of PostGIS (see src/test/prolly/README)'; exit 1; }
	@test -f '$(POSTGIS_BUILD)/src/GNUmakefile' || { echo '$(POSTGIS_BUILD) is not a PostGIS build directory'; exit 1; }
	$(MAKE) -C '$(POSTGIS_BUILD)/src' DESTDIR='$(abs_top_builddir)/tmp_install' install \
		>'$(abs_top_builddir)/tmp_install/log/postgis-install.log' 2>&1
	@test -f '$(abs_top_builddir)/tmp_install$(pkglibdir)/postgis-3.so' || \
		{ echo '$(POSTGIS_BUILD) is a build for another installation prefix than $(prefix)'; exit 1; }

check-postgis-semantics: PROVE_TESTS = t/006_postgis.pl
check-postgis-semantics: require-tap postgis-install
	$(prove_check)

check-postgis-isolation: PROVE_TESTS = t/295_postgis_isolation.pl
check-postgis-isolation: require-tap postgis-install
	$(MAKE) -C $(top_builddir)/src/test/isolation all
	$(prove_check)

check-postgis-durability: PROVE_TESTS = t/296_postgis_crash.pl
check-postgis-durability: require-tap postgis-install
	$(prove_check)

check-tap-vc-postgis: PROVE_TESTS = t/297_postgis_network.pl
check-tap-vc-postgis: check-vc-target-prolly require-tap postgis-install
	$(prove_check)

# PostGIS's own regression suite on versioned tables, judged against heap
# (not part of check-postgis: it takes long; POSTGIS_REGRESS_SUITES and
# POSTGIS_REGRESS_MODES narrow it, see the test)
check-postgis-regress: PROVE_TESTS = t/299_postgis_regress.pl
check-postgis-regress: export POSTGIS_BUILD := $(abspath $(POSTGIS_BUILD))
check-postgis-regress: require-tap postgis-install
	$(prove_check)

check-vc-postgis: postgis-install check-vc-target-prolly vc.conf
	$(pg_regress_check) --temp-config=$(CURDIR)/vc.conf \
		--schedule=$(srcdir)/vc_postgis_schedule

# What stock Postgres must pass: proves the tests are right.  The temporary
# installation (with EXTRA_INSTALL) is made here: temp-install does nothing
# in the recursive make below.  With POSTGIS_BUILD, PostGIS's group too.
check-heap: temp-install
	$(MAKE) check-semantics check-isolation check-tap-durability \
		$(if $(POSTGIS_BUILD),check-postgis-semantics check-postgis-isolation \
			check-postgis-durability) PROLLY_AM=heap

require-tap:
	@test '$(enable_tap_tests)' = yes || { echo 'Configure with --enable-tap-tests to run requirements'; exit 1; }

clean distclean:
	rm -rf $(pg_regress_clean_files) prolly_am.conf bmap.conf vc.conf tap_temp.conf output_iso

.PHONY: check check-target check-vc-target check-vc-target-prolly check-vc-target-guard check-semantics check-vc check-isolation check-vc-isolation \
	check-tap check-tap-durability check-tap-vc check-heap \
	check-postgis postgis-install check-postgis-semantics check-postgis-isolation \
	check-postgis-durability check-vc-postgis check-tap-vc-postgis check-postgis-regress \
	check-upstream-bmap require-tap FORCE
