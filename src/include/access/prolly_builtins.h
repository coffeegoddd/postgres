/*-------------------------------------------------------------------------
 *
 * prolly_builtins.h
 *	  Declarations of the built-in prolly implementations' routine structs.
 *
 * Included only by the registry and by each implementation's own source
 * file (to declare its routine struct).  Everything else finds
 * implementations through the registry (prolly_registry.h).
 *
 * src/include/access/prolly_builtins.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_BUILTINS_H
#define PROLLY_BUILTINS_H

#include "access/prolly_config.h"

#define PG_PROLLY_IMPL(type, symbol) extern PGDLLIMPORT const type symbol;
#include "access/prolly_impllist.h"
#undef PG_PROLLY_IMPL

#endif							/* PROLLY_BUILTINS_H */
