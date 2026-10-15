/*-------------------------------------------------------------------------
 *
 * prolly_xact.c
 *	  Delivering transaction events to prolly engines.
 *
 * Engines register when they first hold private writes in a transaction.
 * At the end of the transaction the registrations are dropped.  Parallel
 * workers never write, so they never register.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/prolly_xact.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_xact.h"
#include "access/twophase_rmgr.h"
#include "access/xact.h"
#include "utils/memutils.h"

#define MAX_ENGINES 8

/* Memory a transaction's private writes may use before spilling (kB). */
int			prolly_write_set_memory = 65536;

static const ProllyEngineRoutine *engines[MAX_ENGINES];
static int	nengines = 0;
static bool callbacks_registered = false;

/* States engines gave at PREPARE, until recorded. */
static StringInfoData prepared[MAX_ENGINES];
static int	nprepared = 0;

static void prepare_engines(void);

static void
forget_engines(void)
{
	nengines = 0;
	nprepared = 0;
}

static void
prolly_xact_callback(XactEvent event, void *arg)
{
	switch (event)
	{
		case XACT_EVENT_PRE_COMMIT:
			for (int i = 0; i < nengines; i++)
				PROLLY_CALL(engines[i], xact_precommit);
			break;
		case XACT_EVENT_COMMIT:
			for (int i = 0; i < nengines; i++)
				PROLLY_CALL(engines[i], xact_commit);
			forget_engines();
			break;
		case XACT_EVENT_PRE_PREPARE:
			prepare_engines();
			break;
		case XACT_EVENT_PREPARE:
			forget_engines();
			break;
		case XACT_EVENT_ABORT:
			for (int i = 0; i < nengines; i++)
				PROLLY_CALL(engines[i], xact_abort);
			forget_engines();
			break;
		case XACT_EVENT_PARALLEL_PRE_COMMIT:
		case XACT_EVENT_PARALLEL_COMMIT:
		case XACT_EVENT_PARALLEL_ABORT:
			break;
	}
}

static void
prolly_subxact_callback(SubXactEvent event, SubTransactionId mySubid,
						SubTransactionId parentSubid, void *arg)
{
	if (event != SUBXACT_EVENT_ABORT_SUB)
		return;
	for (int i = 0; i < nengines; i++)
		PROLLY_CALL(engines[i], subxact_abort, mySubid);
}

/*
 * PREPARE TRANSACTION, while the transaction can still read catalogs: each
 * engine's state for COMMIT PREPARED, kept as the engine's name, a NUL,
 * then the state, until AtPrepare_Prolly() records it.
 */
static void
prepare_engines(void)
{
	MemoryContext old = MemoryContextSwitchTo(TopTransactionContext);

	for (int i = 0; i < nengines; i++)
	{
		Size		namelen = strlen(engines[i]->info.name) + 1;

		initStringInfo(&prepared[i]);
		appendBinaryStringInfo(&prepared[i], engines[i]->info.name, namelen);
		PROLLY_CALL(engines[i], xact_prepare, &prepared[i]);
		if (prepared[i].len == namelen)
			prepared[i].len = 0;	/* nothing to record */
	}
	nprepared = nengines;
	MemoryContextSwitchTo(old);
}

/* PREPARE TRANSACTION, while the two-phase state is assembled. */
void
AtPrepare_Prolly(void)
{
	for (int i = 0; i < nprepared; i++)
	{
		if (prepared[i].len > 0)
			RegisterTwoPhaseRecord(TWOPHASE_RM_PROLLY_ID, 0, prepared[i].data,
								   prepared[i].len);
	}
	nprepared = 0;
}

/* COMMIT PREPARED, before the commit record: the engine commits its state. */
void
prolly_twophase_precommit(FullTransactionId fxid, uint16 info,
						  void *recdata, uint32 len)
{
	const char *name = recdata;
	Size		namelen = strnlen(name, len);
	const ProllyEngineRoutine *engine;

	if (namelen >= len)
		elog(ERROR, "prolly two-phase state is corrupt");
	engine = prolly_engine_lookup(name);
	PROLLY_CALL(engine, commit_prepared, XidFromFullTransactionId(fxid),
				(const char *) recdata + namelen + 1, len - namelen - 1);
}

void
prolly_xact_register_engine(const ProllyEngineRoutine *engine)
{
	for (int i = 0; i < nengines; i++)
	{
		if (engines[i] == engine)
			return;
	}
	if (nengines >= MAX_ENGINES)
		elog(ERROR, "too many prolly engines in one transaction");
	if (!callbacks_registered)
	{
		RegisterXactCallback(prolly_xact_callback, NULL);
		RegisterSubXactCallback(prolly_subxact_callback, NULL);
		callbacks_registered = true;
	}
	engines[nengines++] = engine;
}
