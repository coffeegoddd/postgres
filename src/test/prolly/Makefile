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

# Server settings for every instance the suite starts.
prolly_am.conf: FORCE
	@{ echo "default_table_access_method = '$(PROLLY_AM)'"; \
	   echo "max_prepared_transactions = 10"; } > $@

EXTRA_INSTALL = contrib/btree_gist

REGRESS_OPTS = --temp-config=$(CURDIR)/prolly_am.conf

check: check-semantics check-isolation check-vc check-vc-isolation check-tap

check-semantics: temp-install prolly_am.conf
	$(pg_regress_check) $(REGRESS_OPTS) --schedule=$(srcdir)/semantics_schedule

check-vc: temp-install prolly_am.conf
	$(pg_regress_check) $(REGRESS_OPTS) --schedule=$(srcdir)/vc_schedule

check-isolation: temp-install prolly_am.conf
	$(MAKE) -C $(top_builddir)/src/test/isolation all
	$(pg_isolation_regress_check) $(REGRESS_OPTS) \
		--schedule=$(srcdir)/isolation_schedule

check-vc-isolation: temp-install prolly_am.conf
	$(MAKE) -C $(top_builddir)/src/test/isolation all
	$(pg_isolation_regress_check) $(REGRESS_OPTS) \
		--schedule=$(srcdir)/vc_isolation_schedule

# TAP tests: 1xx are durability (also run against heap), 2xx version control.
check-tap: check-tap-durability check-tap-vc

check-tap-durability: PROVE_TESTS = t/1*.pl
check-tap-durability: temp-install
	$(prove_check)

check-tap-vc: PROVE_TESTS = t/2*.pl
check-tap-vc: temp-install
	$(prove_check)

# Correctness gates 1 and 2 (specs/06-benchmarking.md): Postgres's own
# regression and isolation suites with the engine as the default table AM.
# Not part of "check": some upstream tests print heap-specific details
# (ctid values, page counts), and those exceptions aren't documented yet.
check-upstream-regress: prolly_am.conf
	$(MAKE) -C $(top_builddir)/src/test/regress check TEMP_CONFIG=$(CURDIR)/prolly_am.conf

check-upstream-isolation: prolly_am.conf
	$(MAKE) -C $(top_builddir)/src/test/isolation check TEMP_CONFIG=$(CURDIR)/prolly_am.conf

# What stock Postgres must pass: proves the tests are right.  The temporary
# installation (with EXTRA_INSTALL) is made here: temp-install does nothing
# in the recursive make below.
check-heap: temp-install
	$(MAKE) check-semantics check-isolation check-tap-durability PROLLY_AM=heap

clean distclean:
	rm -rf $(pg_regress_clean_files) prolly_am.conf output_iso

.PHONY: check check-semantics check-vc check-isolation check-vc-isolation \
	check-tap check-tap-durability check-tap-vc check-heap \
	check-upstream-regress check-upstream-isolation FORCE
