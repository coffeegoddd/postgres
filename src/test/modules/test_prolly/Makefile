# src/test/modules/test_prolly/Makefile

MODULE_big = test_prolly
OBJS = \
	$(WIN32RES) \
	test_checksum.o \
	test_prolly.o \
	unit_harness.o \
	unit_registry.o \
	unit_smgr.o
PGFILEDESC = "test_prolly - unit tests and interface conformance suites for the version-control engine"

EXTENSION = test_prolly
DATA = test_prolly--1.0.sql

REGRESS = unit conformance
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
