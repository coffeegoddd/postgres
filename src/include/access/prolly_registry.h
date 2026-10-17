/*-------------------------------------------------------------------------
 *
 * prolly_registry.h
 *	  Finding prolly implementations by interface and name.
 *
 * Built-in implementations are listed in prolly_impllist.h.  Other code (an
 * extension in shared_preload_libraries) can add implementations with
 * prolly_register_impl() from its _PG_init.
 *
 * Callers normally use the typed lookup helpers in each interface header
 * (prolly_tree_lookup() and so on), or prolly_config.h, rather than these.
 *
 * src/include/access/prolly_registry.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_REGISTRY_H
#define PROLLY_REGISTRY_H

#include "access/prolly_impl.h"
#include "nodes/pg_list.h"

/*
 * Returns the implementation of iface with the given name or impl_id, or
 * NULL if there is none.
 */
extern const ProllyImplInfo *prolly_lookup_impl(ProllyInterface iface,
												const char *name);
extern const ProllyImplInfo *prolly_lookup_impl_id(ProllyInterface iface,
												   uint16 impl_id);

/* Like prolly_lookup_impl(), but raises an error if not found. */
extern const ProllyImplInfo *prolly_get_impl(ProllyInterface iface,
											 const char *name);

/* All implementations of iface, as a palloc'd List of ProllyImplInfo *. */
extern List *prolly_list_impls(ProllyInterface iface);

/*
 * Adds an implementation.  Its info must have static lifetime, and its
 * routine struct must be the one declared by the interface header.  Call
 * only from _PG_init of a library in shared_preload_libraries, so that every
 * backend sees the same set.  Raises an error on a duplicate name or impl_id.
 */
extern void prolly_register_impl(const ProllyImplInfo *impl);

/* Interface metadata, for messages, configuration and the conformance suite. */
extern bool prolly_interface_valid(int iface);
extern const char *prolly_interface_name(ProllyInterface iface);
extern bool prolly_interface_from_name(const char *name,
									   ProllyInterface *iface);
extern int	prolly_interface_ncallbacks(ProllyInterface iface);
extern const char *prolly_interface_callback_name(ProllyInterface iface,
												  int callbackno);

/* True if impl provides callback number callbackno (0-based). */
extern bool prolly_impl_has_callback(const ProllyImplInfo *impl,
									 int callbackno);

#endif							/* PROLLY_REGISTRY_H */
