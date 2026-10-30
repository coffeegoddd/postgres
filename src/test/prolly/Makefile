#-------------------------------------------------------------------------
#
# Makefile for src/test/prolly
#
# The requirements suite: tests of what the version-control engine must
# do, run against a server.  It has regression tests (semantics_schedule),
# isolation tests (isolation_schedule) and TAP tests (t/).
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
# groups.
#
# src/test/prolly/Makefile
#
#-------------------------------------------------------------------------

subdir = src/test/prolly
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

PROLLY_AM ?= heap
export PROLLY_AM

AM_CONF = $(abs_top_srcdir)/$(subdir)/am/$(PROLLY_AM).conf
ifeq ($(wildcard $(AM_CONF)),)
$(error there are no settings for table access method "$(PROLLY_AM)": $(AM_CONF) does not exist)
endif

# The regression, isolation and TAP groups share tmp_check.
.NOTPARALLEL:

# The groups that test Postgres semantics, and so also run on heap.
SEMANTICS_TARGETS = check-target check-semantics check-isolation check-tap-durability

check: $(SEMANTICS_TARGETS)

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

# Postgres's regression, isolation and recovery suites with every user
# heap table, index and TOAST table in block-mapped storage.  They pass in
# full: block-mapped storage changes where pages live, not what they hold.
BMAP_CONF = $(abs_top_srcdir)/$(subdir)/bmap.conf

check-upstream-bmap: temp-install
	$(MAKE) -C $(top_builddir)/src/test/regress check TEMP_CONFIG=$(BMAP_CONF)
	$(MAKE) -C $(top_builddir)/src/test/isolation check TEMP_CONFIG=$(BMAP_CONF)
	$(MAKE) -C $(top_builddir)/src/test/recovery check TEMP_CONFIG=$(BMAP_CONF)

.PHONY: check check-heap check-upstream-bmap $(SEMANTICS_TARGETS)

clean distclean:
	rm -rf results output_iso tmp_check tmp_check_iso log
	rm -f regression.diffs regression.out
