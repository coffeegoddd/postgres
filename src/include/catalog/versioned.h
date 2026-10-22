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

#include "catalog/objectaddress.h"

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

/* error if an object on another branch depends on object, being dropped */
extern void check_other_branch_dependents(const ObjectAddress *object);

/* the access method a new relation in namespace nspid gets */
extern Oid	versioned_relation_am(Oid nspid, Oid amoid, char relkind,
								  char relpersistence, bool explicit);

/* error if an object would move between a versioned and a shared schema */
extern void check_versioned_namespace_move(Oid oldnspid, Oid newnspid);

#endif							/* VERSIONED_H */
