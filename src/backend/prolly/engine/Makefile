#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for src/backend/prolly/engine, the version-control engine's
#    libraries: chunk names and formats, the prolly tree, and version
#    control on a repository's references
#
# IDENTIFICATION
#    src/backend/prolly/engine/Makefile
#
#-------------------------------------------------------------------------

subdir = src/backend/prolly/engine
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = \
	chunkfmt.o \
	phash.o \
	prolly.o \
	vc.o \
	vc_refs.o \
	vc_refs_memory.o \
	vc_refs_pointers.o

include $(top_srcdir)/src/backend/common.mk
