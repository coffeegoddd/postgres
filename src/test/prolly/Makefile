#-------------------------------------------------------------------------
#
# Makefile for src/test/prolly
#
# Requirements suite for prolly storage engines.  Not part of "make check"
# or "make check-world": run it against an engine with
#
#	make -C src/test/prolly check PROLLY_AM=prolly_d1
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

PROLLY_AM ?= prolly_d1
export PROLLY_AM

# TAP groups share tmp_check and the temporary installation.
.NOTPARALLEL:

# Server settings for every instance the suite starts.
prolly_am.conf: FORCE
	@{ echo "default_table_access_method = '$(PROLLY_AM)'"; \
	   echo "max_prepared_transactions = 10"; } > $@

# Commits of branches made by copying: only the page-based engine keeps
# branches as copies, so these tests' tables are prolly's whatever
# PROLLY_AM is.
vc_commits.conf: FORCE
	@{ echo "default_table_access_method = 'prolly'"; \
	   echo "max_prepared_transactions = 10"; } > $@

# Heap tables in block-mapped storage instead (debug_block_mapped_heap).
bmap.conf: FORCE
	@{ echo "debug_block_mapped_heap = on"; \
	   echo "max_prepared_transactions = 10"; } > $@

EXTRA_INSTALL = contrib/btree_gist

REGRESS_OPTS = --temp-config=$(CURDIR)/prolly_am.conf

check: check-target check-semantics check-isolation check-vc-commits check-vc check-vc-isolation check-tap

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

check-vc: require-legacy-vc check-vc-target temp-install prolly_am.conf
	$(pg_regress_check) $(REGRESS_OPTS) --schedule=$(srcdir)/vc_schedule

check-vc-commits: temp-install vc_commits.conf
	$(pg_regress_check) --temp-config=$(CURDIR)/vc_commits.conf \
		--schedule=$(srcdir)/vc_commits_schedule

check-isolation: PROVE_TESTS = t/002_isolation.pl
check-isolation: require-tap temp-install
	$(MAKE) -C $(top_builddir)/src/test/isolation all
	$(prove_check)

check-vc-isolation: require-legacy-vc check-vc-target temp-install prolly_am.conf
	$(MAKE) -C $(top_builddir)/src/test/isolation all
	$(pg_isolation_regress_check) $(REGRESS_OPTS) \
		--schedule=$(srcdir)/vc_isolation_schedule

# TAP tests: 1xx are durability (also run against heap), 2xx version control.
check-tap: check-tap-durability check-tap-vc

check-tap-durability: PROVE_TESTS = t/1*.pl
check-tap-durability: require-tap temp-install
	$(prove_check)

check-tap-vc: PROVE_TESTS = t/2*.pl
check-tap-vc: require-legacy-vc check-vc-target require-tap temp-install
	$(prove_check)

# Upstream correctness checks: Postgres's own
# regression and isolation suites with the engine as the default table AM.
# Not part of "check": some upstream tests print heap-specific details
# (ctid values, page counts), and those exceptions aren't documented yet.
check-upstream-regress: prolly_am.conf
	@test '$(PROLLY_AM)' != prolly || { echo 'Use check-upstream-bmap for upstream tests of block-mapped storage'; exit 1; }
	$(MAKE) -C $(top_builddir)/src/test/regress check TEMP_CONFIG=$(CURDIR)/prolly_am.conf

check-upstream-isolation: prolly_am.conf
	@test '$(PROLLY_AM)' != prolly || { echo 'Use check-upstream-bmap for upstream tests of block-mapped storage'; exit 1; }
	$(MAKE) -C $(top_builddir)/src/test/isolation check TEMP_CONFIG=$(CURDIR)/prolly_am.conf

# Postgres's regression, isolation and recovery suites with every user
# table, index and TOAST table in block-mapped storage.  These pass in
# full: block-mapped storage changes where pages live, not what they hold.
check-upstream-bmap: temp-install bmap.conf
	$(MAKE) -C $(top_builddir)/src/test/recovery checkprep
	$(MAKE) -C $(top_builddir)/src/test/regress check TEMP_CONFIG=$(CURDIR)/bmap.conf
	$(MAKE) -C $(top_builddir)/src/test/isolation check TEMP_CONFIG=$(CURDIR)/bmap.conf
	$(MAKE) -C $(top_builddir)/src/test/recovery check TEMP_CONFIG=$(CURDIR)/bmap.conf

# What stock Postgres must pass: proves the tests are right.  The temporary
# installation (with EXTRA_INSTALL) is made here: temp-install does nothing
# in the recursive make below.
check-heap: temp-install
	$(MAKE) check-semantics check-isolation check-tap-durability PROLLY_AM=heap

require-tap:
	@test '$(enable_tap_tests)' = yes || { echo 'Configure with --enable-tap-tests to run requirements'; exit 1; }

require-legacy-vc:
	@test '$(PROLLY_AM)' != prolly || { echo 'The VC requirements use the legacy tree-engine API; run semantics, isolation and durability for prolly'; exit 1; }

clean distclean:
	rm -rf $(pg_regress_clean_files) prolly_am.conf bmap.conf vc_commits.conf output_iso

.PHONY: check check-target check-vc-target check-vc-target-guard check-semantics check-vc check-vc-commits check-isolation check-vc-isolation \
	check-tap check-tap-durability check-tap-vc check-heap \
	check-upstream-regress check-upstream-isolation check-upstream-bmap require-tap require-legacy-vc FORCE
