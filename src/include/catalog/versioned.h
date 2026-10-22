/*-------------------------------------------------------------------------
 *
 * versioned.h
 *	  Versioned schemas: which objects are versioned, and the rules that
 *	  keep shared objects from depending on them.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/versioned.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VERSIONED_H
#define VERSIONED_H

#include "access/htup.h"
#include "catalog/objectaddress.h"
#include "nodes/pg_list.h"
#include "utils/relcache.h"

/* does the database have any versioned schema?  (cheap after the first call) */
extern bool database_has_versioned_schemas(void);

/* is this schema versioned? */
extern bool namespace_is_versioned(Oid nspid);

/* is this object versioned (in a versioned schema, or part of something that is)? */
extern bool object_is_versioned(const ObjectAddress *object);

/* error if a shared object would depend on a versioned one */
extern void check_versioned_dependencies(const ObjectAddress *depender,
										 const ObjectAddress *referenced,
										 int nreferenced);

/* does a branch other than the session's have copies? */
extern bool other_branches_have_copies(void);

/* error if an object on another branch depends on object, being dropped */
extern void check_other_branch_dependents(const ObjectAddress *object);

/*
 * the name of another branch whose objects depend on object (with
 * data_only, only those that can hold its values), or NULL
 */
extern char *other_branch_dependent(const ObjectAddress *object,
									bool data_only);

/* the name of another branch with a typed table of typeOid, or NULL */
extern char *other_branch_typed_table(Oid typeOid);

/* the first extension in schema nspid, or InvalidOid */
extern Oid	extension_in_namespace(Oid nspid);

/* error if an extension can't be created in schema nspid */
extern void check_versioned_extension_schema(Oid nspid);

/* error if an extension's new shared row has another branch's object's identity */
extern void check_extension_member_identity(Relation rel, HeapTuple tup);

/* error if a versioned schema holding an extension would be dropped or renamed */
extern void check_versioned_schema_change(Oid nspid, const char *what);

/* error if an object would move between an extension and a versioned schema */
extern void check_versioned_extension_member(const ObjectAddress *object,
											 bool adding, Oid extension);

/* an extension script begins and ends: what it let go is checked at its end */
extern List *extension_script_begin(void);
extern void extension_script_end(List *outer, bool check);

/* error if an extension would move into or out of a versioned schema */
extern void check_versioned_extension_move(Oid oldnspid, Oid newnspid);

/* the access method a new relation in namespace nspid gets */
extern Oid	versioned_relation_am(Oid nspid, Oid amoid, char relkind,
								  char relpersistence, bool explicit);

/* error if an object would move between a versioned and a shared schema */
extern void check_versioned_namespace_move(Oid oldnspid, Oid newnspid);

#endif							/* VERSIONED_H */
