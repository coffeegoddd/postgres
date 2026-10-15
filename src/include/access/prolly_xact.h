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
 * PREPARE TRANSACTION: each registered engine gives the state it needs to
 * commit later (xact_prepare, while the transaction can still read
 * catalogs), which AtPrepare_Prolly() (called by PrepareTransaction()
 * while the prepared state is assembled) records in the two-phase state; COMMIT PREPARED hands it back to the engine's commit_prepared
 * before the commit record is written (prolly_twophase_precommit).
 *
 * src/include/access/prolly_xact.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_XACT_H
#define PROLLY_XACT_H

#include "access/prolly_engine.h"
#include "access/transam.h"

extern void prolly_xact_register_engine(const ProllyEngineRoutine *engine);
extern void AtPrepare_Prolly(void);
extern void prolly_twophase_precommit(FullTransactionId fxid, uint16 info,
									  void *recdata, uint32 len);

#endif							/* PROLLY_XACT_H */
