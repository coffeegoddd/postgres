#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for src/backend/prolly, the version-control engine's code
#    compiled into the server
#
# IDENTIFICATION
#    src/backend/prolly/Makefile
#
#-------------------------------------------------------------------------

subdir = src/backend/prolly
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS = \
	branch_catalog.o \
	branch_catalog_copy.o \
	branch_catalog_ops.o \
	branch_filter.o \
	chunk_freer.o \
	chunk_index.o \
	chunk_log.o \
	chunk_memstore.o \
	chunk_pin.o \
	chunk_dump.o \
	chunk_pointers.o \
	chunk_region.o \
	chunk_store.o \
	chunkstore.o \
	registry.o

include $(top_srcdir)/src/backend/common.mk
