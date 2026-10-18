#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for src/backend/access/prolly/libpqremote
#
# IDENTIFICATION
#    src/backend/access/prolly/libpqremote/Makefile
#
#-------------------------------------------------------------------------

subdir = src/backend/access/prolly/libpqremote
top_builddir = ../../../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I$(srcdir) -I$(libpq_srcdir) $(CPPFLAGS)

OBJS = \
	$(WIN32RES) \
	prolly_remote_libpq.o
SHLIB_LINK_INTERNAL = $(libpq)
SHLIB_LINK = $(filter -lintl, $(LIBS))
SHLIB_PREREQS = submake-libpq
PGFILEDESC = "prolly_remote_libpq - prolly remotes over a libpq connection"
NAME = prolly_remote_libpq

all: all-shared-lib

include $(top_srcdir)/src/Makefile.shlib

install: all installdirs install-lib

installdirs: installdirs-lib

uninstall: uninstall-lib

clean distclean: clean-lib
	rm -f $(OBJS)
