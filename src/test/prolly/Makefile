#-------------------------------------------------------------------------
#
# Makefile for src/test/prolly
#
# The requirements suite: tests of what the version-control engine must
# do, run against a server.  It has regression tests (semantics_schedule,
# branches_schedule), isolation tests (isolation_schedule) and TAP tests
# (t/).
#
# PROLLY_AM names the table access method under test, and am/$(PROLLY_AM).conf
# holds the server settings that put new tables on it.  The tests of
# Postgres semantics expect exactly what stock Postgres does, so check-heap
# runs them on heap to show that the tests themselves are right.
#
#	make -C src/test/prolly check			# every group, on PROLLY_AM
#	make -C src/test/prolly check-heap		# the semantics groups, on heap
#	make -C src/test/prolly check-upstream-bmap	# Postgres's suites, block-mapped
#
# TAP tests are numbered by group: 0xx check that the access method under
# test is in effect, 1xx check durability and are part of the semantics
# groups, 2xx check branched catalogs.
#
# src/test/prolly/Makefile
#
#-------------------------------------------------------------------------

# The modules Postgres's recovery suite loads.  check-upstream-bmap runs that
# suite in a recursive make, where its own Makefile's EXTRA_INSTALL does not
# reach the temporary installation built here once for every suite.
EXTRA_INSTALL = contrib/pg_prewarm \
	contrib/pg_stat_statements \
	contrib/test_decoding \
	src/test/modules/injection_points \
	src/test/modules/test_wait_lsn

subdir = src/test/prolly
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

PROLLY_AM ?= heap
export PROLLY_AM

# extensions the branched catalogs' tests install on branches, and the
# output plugin their decoding test reads changes with
EXTRA_INSTALL = contrib/hstore contrib/pg_trgm contrib/test_decoding

AM_CONF = $(abs_top_srcdir)/$(subdir)/am/$(PROLLY_AM).conf
ifeq ($(wildcard $(AM_CONF)),)
$(error there are no settings for table access method "$(PROLLY_AM)": $(AM_CONF) does not exist)
endif

# The regression, isolation and TAP groups share tmp_check.
.NOTPARALLEL:

# The groups that test Postgres semantics, and so also run on heap.
SEMANTICS_TARGETS = check-target check-semantics check-isolation check-tap-durability

# The groups that test what version control adds.
VC_TARGETS = check-branches

check: $(SEMANTICS_TARGETS) $(VC_TARGETS)

check-heap: temp-install
	$(MAKE) $(SEMANTICS_TARGETS) PROLLY_AM=heap

check-target: PROVE_TESTS = t/0*.pl
check-target: temp-install
	$(prove_check)

check-semantics: temp-install
	$(pg_regress_check) --temp-config=$(AM_CONF) \
		--schedule=$(srcdir)/semantics_schedule

check-isolation: temp-install
	$(MAKE) -C $(top_builddir)/src/test/isolation all
	$(pg_isolation_regress_check) --temp-config=$(AM_CONF) \
		--schedule=$(srcdir)/isolation_schedule

check-tap-durability: PROVE_TESTS = t/1*.pl
check-tap-durability: temp-install
	$(prove_check)

check-branches: temp-install
	$(pg_regress_check) --temp-config=$(AM_CONF) \
		--schedule=$(srcdir)/branches_schedule
	$(MAKE) check-tap-branches

check-tap-branches: PROVE_TESTS = t/2*.pl
check-tap-branches: temp-install
	$(prove_check)

# Postgres's regression, isolation and recovery suites with every user
# heap table, index and TOAST table in block-mapped storage.  They pass in
# full: block-mapped storage changes where pages live, not what they hold.
BMAP_CONF = $(abs_top_srcdir)/$(subdir)/bmap.conf

# (each suite runs whatever became of the one before, and the target fails
# if any did)
check-upstream-bmap: temp-install
	@status=0; \
	$(MAKE) -C $(top_builddir)/src/test/regress check TEMP_CONFIG=$(BMAP_CONF) || status=1; \
	$(MAKE) -C $(top_builddir)/src/test/isolation check TEMP_CONFIG=$(BMAP_CONF) || status=1; \
	$(MAKE) -C $(top_builddir)/src/test/recovery check TEMP_CONFIG=$(BMAP_CONF) || status=1; \
	exit $$status

.PHONY: check check-heap check-upstream-bmap check-tap-branches $(SEMANTICS_TARGETS) $(VC_TARGETS)

clean distclean:
	rm -rf results output_iso tmp_check tmp_check_iso log
	rm -f regression.diffs regression.out
