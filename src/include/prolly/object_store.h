/*-------------------------------------------------------------------------
 *
 * object_store.h
 *	  Named immutable objects off the server: where tiered history keeps
 *	  the bytes of its chunks.
 *
 * An object store keeps objects by name, each written whole once and never
 * changed: written, it is there in full or not at all (a crash in the
 * writing leaves none), and read in ranges.  Object storage is the target;
 * "directory" keeps objects as files under a directory, written to a
 * temporary file, synced and linked into place.
 *
 * A name is of the letters, digits and "._-", in parts separated by "/"
 * (none empty, "." or ".."), as object storage keys and paths both allow.
 *
 * Implementations register with the registry (prolly/registry.h) under
 * OBJECT_STORE_IFACE; conformance tests every one.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/prolly/object_store.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_OBJECT_STORE_H
#define PROLLY_OBJECT_STORE_H

#include "nodes/pg_list.h"

#define OBJECT_STORE_IFACE	"object_store"

/* The longest name an object may have. */
#define OBJECT_NAME_MAX		256

typedef struct ObjectStore ObjectStore;

/* Called with each object a listing finds, in the names' order. */
typedef void (*ObjectListFn) (void *arg, const char *name, uint64 size);

typedef struct ObjectStoreRoutine
{
	/* The store at location (for "directory", a directory), palloc'd. */
	ObjectStore *(*open) (const char *location);

	/*
	 * Writes the object name whole, durably: once it returns the object is
	 * there, after a crash too.  An error if one of the name is there
	 * (objects are never changed).
	 */
	void		(*put) (ObjectStore *os, const char *name, const void *data,
						size_t len);

	/*
	 * len bytes of the object from off into dst; an error if it is not
	 * there, or has fewer.
	 */
	void		(*read) (ObjectStore *os, const char *name, uint64 off,
						 void *dst, size_t len);

	/* The object's size in *size; false if it is not there. */
	bool		(*size) (ObjectStore *os, const char *name, uint64 *size);

	/* fn called with each object whose name begins with prefix. */
	void		(*list) (ObjectStore *os, const char *prefix, ObjectListFn fn,
						 void *arg);

	/* Removes the object, durably; false if it was not there. */
	bool		(*remove) (ObjectStore *os, const char *name);
} ObjectStoreRoutine;

struct ObjectStore
{
	const ObjectStoreRoutine *routine;
};

extern PGDLLIMPORT const ObjectStoreRoutine DirectoryObjectStore;

/* The store of implementation impl at location; an error if none is impl. */
extern ObjectStore *ObjectStoreOpen(const char *impl, const char *location);

/* The implementations registered (ProllyImplementation), in order. */
extern List *ObjectStoreList(void);

/* An error unless name is an object's name as object_store.h says. */
extern void ObjectNameCheck(const char *name);

#endif							/* PROLLY_OBJECT_STORE_H */
