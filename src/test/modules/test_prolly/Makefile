# src/test/modules/test_prolly/Makefile

MODULE_big = test_prolly
OBJS = \
	$(WIN32RES) \
	foundation_checks.o \
	storage_checks.o \
	tree_checks.o \
	test_prolly.o
PGFILEDESC = "test_prolly - conformance suites for prolly interfaces"

EXTENSION = test_prolly
DATA = test_prolly--1.0.sql

REGRESS = test_prolly
TAP_TESTS = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_prolly
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
