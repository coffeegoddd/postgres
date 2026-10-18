/*-------------------------------------------------------------------------
 *
 * prolly_deps.h
 *	  Checking what commits need from outside the versioned schemas.
 *
 * A commit records each dependency of its versioned objects outside them,
 * in a tree beside its root (prolly_refs.h): extensions with their
 * versions and schemas, and objects of ordinary schemas.  A clone checks those of every
 * branch it brings before it makes anything, and creates the extensions
 * the database lacks; a move checks those of the commit it goes to.
 *
 * src/include/access/prolly_deps.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_DEPS_H
#define PROLLY_DEPS_H

#include "access/prolly_types.h"
#include "nodes/pg_list.h"

/*
 * Check the dependencies the trees (ProllyNodeRef *, commits' dependencies)
 * hold against the database: each object by its address, each extension
 * against those installed and available, in the schema recorded.  An
 * extension several commits record at different versions needs the
 * newest, by its update paths; one recorded without a version, any.
 * Anything missing is an error naming all of it, before anything is made.
 * With install, an extension the database lacks is then created at that
 * version in its schema, after the recorded ones it requires (and with
 * what else it requires); without, it is missing too.  An extension the
 * database has is never changed: one older than needed, or in another
 * schema, is an error.  what names the operation, for messages ("clone").
 */
extern void prolly_check_dependencies(List *trees, bool install,
									  const char *what);

#endif							/* PROLLY_DEPS_H */
