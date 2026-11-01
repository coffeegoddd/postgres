#-------------------------------------------------------------------------
#
# Makefile for src/tools/prolly_bench
#
# Benchmarks of the version-control engine.  They are not part of the build
# or of any check target: they take minutes to hours and their results
# depend on the machine, so they run only when asked for, with
#
#	make -C src/tools/prolly_bench run
#
# Each benchmark adds itself to BENCHMARKS, a list of targets that "run"
# makes in turn.
#
# src/tools/prolly_bench/Makefile
#
#-------------------------------------------------------------------------

subdir = src/tools/prolly_bench
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

BENCHMARKS = bench-branch-filter

all:

# what the catalog filter costs a session at 1000 branches of a schema
bench-branch-filter:
	$(srcdir)/branch_filter.sh '$(bindir)' '$(abs_top_builddir)/$(subdir)/tmp_bench'

run: $(BENCHMARKS)

.PHONY: run $(BENCHMARKS)
