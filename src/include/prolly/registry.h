/*-------------------------------------------------------------------------
 *
 * registry.h
 *	  Named implementations of the engine's interfaces.
 *
 * Every component of the engine sits behind an interface: a struct of
 * callbacks, in the style of TableAmRoutine, that callers use without
 * knowing which implementation is behind it.  An implementation registers
 * its routine struct here, under the interface's name and a name of its
 * own, so that the code choosing an implementation, and the conformance
 * tests that every implementation of an interface must pass, can find all
 * of them without a list of them being written down anywhere else.
 *
 * The registry is local to each process.  An implementation in a loadable
 * module registers from the module's _PG_init function, so preloading the
 * module makes the implementation known to every backend.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/prolly/registry.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_REGISTRY_H
#define PROLLY_REGISTRY_H

#include "nodes/pg_list.h"

/*
 * One registered implementation.  The routine struct's type is the
 * interface's; a caller that looked an implementation up by its interface's
 * name casts it back.
 */
typedef struct ProllyImplementation
{
	const char *iface;			/* name of the interface */
	const char *name;			/* name of this implementation */
	const void *routine;		/* the interface's routine struct */
} ProllyImplementation;

extern void ProllyRegisterImplementation(const char *iface, const char *name,
										 const void *routine);
extern const ProllyImplementation *ProllyLookupImplementation(const char *iface,
															  const char *name);
extern List *ProllyListImplementations(const char *iface);

#endif							/* PROLLY_REGISTRY_H */
