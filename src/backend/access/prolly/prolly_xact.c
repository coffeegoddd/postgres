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
#include "access/xact.h"

#define MAX_ENGINES 8

static const ProllyEngineRoutine *engines[MAX_ENGINES];
static int	nengines = 0;
static bool callbacks_registered = false;

static void
forget_engines(void)
{
	nengines = 0;
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
			for (int i = 0; i < nengines; i++)
				PROLLY_CALL(engines[i], xact_prepare);
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
