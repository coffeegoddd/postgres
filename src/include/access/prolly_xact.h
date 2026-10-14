/*-------------------------------------------------------------------------
 *
 * prolly_xact.h
 *	  Transaction events for prolly engines.
 *
 * An engine with private writes in the current transaction registers
 * itself once per transaction; it then gets xact_precommit, xact_commit,
 * xact_prepare, xact_abort and subxact_abort calls (prolly_engine.h) from
 * Postgres's transaction callbacks.
 *
 * src/include/access/prolly_xact.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_XACT_H
#define PROLLY_XACT_H

#include "access/prolly_engine.h"

extern void prolly_xact_register_engine(const ProllyEngineRoutine *engine);

#endif							/* PROLLY_XACT_H */
