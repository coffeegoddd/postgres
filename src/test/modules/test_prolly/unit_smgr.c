/*-------------------------------------------------------------------------
 *
 * unit_smgr.c
 *	  Unit tests of the storage manager interface's calls for storage that
 *	  shares pages, on md.c, which implements none of them.
 *
 * Every relation not claimed by another storage manager stays on md.c, and
 * md.c must behave exactly as before the calls existed: its pages are
 * never shared, never need preparing before a change, and are never given
 * other storage, and each of the hooks around truncation, extension, reads
 * and drops does nothing.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/test/modules/test_prolly/unit_smgr.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/table.h"
#include "access/xlog.h"
#include "catalog/pg_class.h"
#include "storage/bufmgr.h"
#include "storage/smgr.h"
#include "test_prolly.h"
#include "utils/memutils.h"
#include "utils/rel.h"

/* pg_class: a relation on md.c with at least one block */
static Relation
open_md_relation(void)
{
	return table_open(RelationRelationId, AccessShareLock);
}

static void
md_is_chosen(void)
{
	Relation	rel = open_md_relation();

	CHECK_EQ(RelationGetSmgr(rel)->smgr_which, SMGR_MD);
	table_close(rel, AccessShareLock);
}

static void
md_pages_are_its_own(void)
{
	Relation	rel = open_md_relation();
	SMgrRelation srel = RelationGetSmgr(rel);
	bool		copies = true;

	CHECK(!smgrpreparesforchanges(srel, MAIN_FORKNUM));
	CHECK(!smgrpageisshared(srel, MAIN_FORKNUM, 0, &copies));
	CHECK(!copies);
	CHECK(!smgrpagesharedelsewhere(srel, MAIN_FORKNUM, 0));
	CHECK(!smgrreplacesblocks(srel, MAIN_FORKNUM));
	CHECK(!smgrreplacedafter(srel, MAIN_FORKNUM, 0, GetXLogInsertRecPtr()));
	CHECK(!smgrcanshare(srel));
	CHECK(!smgrwritesread(MAIN_FORKNUM));
	CHECK_EQ(smgrreleasestep(srel), 0);
	table_close(rel, AccessShareLock);
}

/* md.c storage cannot be shared, and saying so is an error, not a crash */
static void
md_refuses_shares(void)
{
	Relation	rel = open_md_relation();
	SMgrRelation srel = RelationGetSmgr(rel);
	MemoryContext cxt = CurrentMemoryContext;
	char	   *msg = NULL;

	PG_TRY();
	{
		smgrshare(srel, srel);
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		MemoryContextSwitchTo(cxt);
		edata = CopyErrorData();
		FlushErrorState();
		msg = edata->message;
	}
	PG_END_TRY();
	CHECK(msg != NULL);
	CHECK(strstr(msg, "can't be shared") != NULL);
	table_close(rel, AccessShareLock);
}

/* the hooks md.c leaves unset do nothing, and change nothing */
static void
md_hooks_do_nothing(void)
{
	Relation	rel = open_md_relation();
	SMgrRelation srel = RelationGetSmgr(rel);
	ForkNumber	fork = MAIN_FORKNUM;
	BlockNumber nblocks = smgrnblocks(srel, MAIN_FORKNUM);
	Buffer		buf;

	CHECK_LT(0, nblocks);

	smgrprepareread(srel, MAIN_FORKNUM, 0, 1);
	smgrbeforeextend(srel, MAIN_FORKNUM, 1);
	smgrpreextend(srel, MAIN_FORKNUM, nblocks, 1);
	smgrpretruncate(srel, &fork, 1);
	smgrposttruncate(srel, &fork, 1);
	smgrprerestore(srel, MAIN_FORKNUM, 0, GetXLogInsertRecPtr());
	smgrenddrops();

	buf = ReadBuffer(rel, 0);
	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	smgrprepareforchange(srel, MAIN_FORKNUM, 0, buf);
	UnlockReleaseBuffer(buf);

	CHECK_EQ(smgrnblocks(srel, MAIN_FORKNUM), nblocks);
	table_close(rel, AccessShareLock);
}

static const ProllyUnitTest tests[] = {
	{"md_is_chosen", md_is_chosen},
	{"md_pages_are_its_own", md_pages_are_its_own},
	{"md_refuses_shares", md_refuses_shares},
	{"md_hooks_do_nothing", md_hooks_do_nothing},
};

const ProllyUnitSuite unit_smgr_suite = {"smgr", tests, lengthof(tests)};
