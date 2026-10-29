/*-------------------------------------------------------------------------
 *
 * prolly_branch.c
 *	  A spike on stock Postgres: a branch of a schema is a set of relation
 *	  files, one per table, index, TOAST relation and sequence, and checking
 *	  a branch out points the catalog at its files.
 *
 * Every index method works unchanged, since a table and its indexes switch
 * together; the switch is transactional (pg_class rows are updated), and
 * every write to a branch's files is ordinary WAL.  What it cannot give on
 * its own is in the spike's notes: one branch per database at a time, the
 * same definitions on every branch, and files the catalog does not name
 * (parked branches) that the server's own bookkeeping does not see.
 *
 * With relation_locator_hook (a core hook), a session can also work on a
 * branch of its own: prolly_branch.files maps each of the schema's
 * relations to the branch's file number for this session, and the hook
 * applies it as each relcache entry is built.  The setting is a GUC, so
 * parallel workers get it with the rest of the session's settings.
 *
 * prolly_branch_copy_files copies a relation's files to a new file number;
 * prolly_branch_point points a relation at a file number, with the
 * pg_class fields that belong to the file; prolly_branch_drop_file drops a
 * file at commit.  The branch bookkeeping is SQL (prolly_branch--1.0.sql).
 *
 * contrib/prolly_branch/prolly_branch.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/indexing.h"
#include "catalog/pg_class.h"
#include "catalog/storage.h"
#include "catalog/storage_xlog.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/smgr.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/relcache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

PG_MODULE_MAGIC_EXT(.name = "prolly_branch",.version = PG_VERSION);

PG_FUNCTION_INFO_V1(prolly_branch_copy_files);
PG_FUNCTION_INFO_V1(prolly_branch_point);
PG_FUNCTION_INFO_V1(prolly_branch_drop_file);
PG_FUNCTION_INFO_V1(prolly_branch_invalidate);

/* ------------------------------------------------------- a session's branch */

/* "relid:filenode,..." for this session; empty: the catalog's files */
static char *session_files = NULL;

typedef struct FileMap
{
	Oid			relid;
	RelFileNumber filenode;
} FileMap;

static FileMap *map = NULL;
static int	nmap = 0;
static char *map_from = NULL;	/* the setting the map was parsed from */
static relation_locator_hook_type prev_locator_hook = NULL;

static int
map_cmp(const void *a, const void *b)
{
	Oid			x = ((const FileMap *) a)->relid,
				y = ((const FileMap *) b)->relid;

	return (x > y) - (x < y);
}

/* Parses the setting when it changed; no catalog access, so safe anywhere. */
static void
load_map(void)
{
	const char *p = session_files ? session_files : "";
	int			cap = 0;

	if (map_from && strcmp(map_from, p) == 0)
		return;
	if (map_from)
		pfree(map_from);
	map_from = MemoryContextStrdup(TopMemoryContext, p);
	nmap = 0;
	for (const char *q = p; *q; q++)
		cap += *q == ',';
	cap++;
	if (map)
		pfree(map);
	map = MemoryContextAlloc(TopMemoryContext, sizeof(FileMap) * cap);
	while (*p)
	{
		char	   *end;
		unsigned long relid = strtoul(p, &end, 10),
					filenode;

		if (*end != ':')
			break;
		filenode = strtoul(end + 1, &end, 10);
		map[nmap].relid = (Oid) relid;
		map[nmap].filenode = (RelFileNumber) filenode;
		nmap++;
		p = *end == ',' ? end + 1 : end;
	}
	qsort(map, nmap, sizeof(FileMap), map_cmp);
}

static void
branch_locator(Relation relation)
{
	if (prev_locator_hook)
		prev_locator_hook(relation);
	if (session_files == NULL || session_files[0] == '\0')
		return;
	load_map();
	{
		FileMap		key = {RelationGetRelid(relation), 0};
		FileMap    *hit = bsearch(&key, map, nmap, sizeof(FileMap), map_cmp);

		if (hit)
			relation->rd_locator.relNumber = hit->filenode;
	}
}

void
_PG_init(void)
{
	DefineCustomStringVariable("prolly_branch.files",
							   "This session's file number for each relation of a branch.",
							   NULL, &session_files, "", PGC_USERSET, 0, NULL, NULL, NULL);
	MarkGUCPrefixReserved("prolly_branch");
	prev_locator_hook = relation_locator_hook;
	relation_locator_hook = branch_locator;
}

/*
 * prolly_branch_invalidate(rel regclass)
 *
 * Rebuilds rel's relcache entry at the next command, so a changed
 * prolly_branch.files takes effect.
 */
Datum
prolly_branch_invalidate(PG_FUNCTION_ARGS)
{
	CacheInvalidateRelcacheByRelid(PG_GETARG_OID(0));
	PG_RETURN_VOID();
}

static Relation
open_with_storage(Oid relid)
{
	Relation	rel;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to switch relation files")));
	rel = relation_open(relid, AccessExclusiveLock);
	if (!RELKIND_HAS_STORAGE(rel->rd_rel->relkind) || rel->rd_rel->relfilenode == InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" has no files of its own", RelationGetRelationName(rel))));
	return rel;
}

/*
 * prolly_branch_copy_files(rel regclass) returns oid
 *
 * Copies every fork of rel's current files to a new file number in the
 * same tablespace, WAL-logged as ALTER TABLE SET TABLESPACE copies, and
 * returns it.  The catalog does not name the copy; it is deleted if the
 * transaction aborts.
 */
Datum
prolly_branch_copy_files(PG_FUNCTION_ARGS)
{
	Relation	rel = open_with_storage(PG_GETARG_OID(0));
	RelFileLocator newloc = rel->rd_locator;
	SMgrRelation dst;
	char		persistence = rel->rd_rel->relpersistence;

	newloc.relNumber = GetNewRelFileNumber(rel->rd_rel->reltablespace, NULL, persistence);
	/* the copy reads the files, so whatever is in shared buffers goes first */
	FlushRelationBuffers(rel);
	dst = RelationCreateStorage(newloc, persistence, true);
	RelationCopyStorage(RelationGetSmgr(rel), dst, MAIN_FORKNUM, persistence);
	for (ForkNumber fork = MAIN_FORKNUM + 1; fork <= MAX_FORKNUM; fork++)
		if (smgrexists(RelationGetSmgr(rel), fork))
		{
			smgrcreate(dst, fork, false);
			if (RelationIsPermanent(rel) ||
				(persistence == RELPERSISTENCE_UNLOGGED && fork == INIT_FORKNUM))
				log_smgrcreate(&newloc, fork);
			RelationCopyStorage(RelationGetSmgr(rel), dst, fork, persistence);
		}
	smgrclose(dst);
	relation_close(rel, NoLock);
	PG_RETURN_OID(newloc.relNumber);
}

/*
 * prolly_branch_point(rel regclass, filenode oid, relpages int4,
 *					   reltuples float4, relallvisible int4,
 *					   relfrozenxid xid, relminmxid xid)
 *
 * Points rel at another file number, with the pg_class fields that
 * describe that file, in the current transaction.  The file it leaves
 * stays on disk.
 */
Datum
prolly_branch_point(PG_FUNCTION_ARGS)
{
	Relation	rel = open_with_storage(PG_GETARG_OID(0));
	Relation	pg_class = table_open(RelationRelationId, RowExclusiveLock);
	HeapTuple	tup = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(RelationGetRelid(rel)));
	Form_pg_class form;

	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for relation %u", RelationGetRelid(rel));
	form = (Form_pg_class) GETSTRUCT(tup);
	form->relfilenode = PG_GETARG_OID(1);
	form->relpages = PG_GETARG_INT32(2);
	form->reltuples = PG_GETARG_FLOAT4(3);
	form->relallvisible = PG_GETARG_INT32(4);
	form->relfrozenxid = PG_GETARG_TRANSACTIONID(5);
	form->relminmxid = PG_GETARG_TRANSACTIONID(6);
	CatalogTupleUpdate(pg_class, &tup->t_self, tup);
	heap_freetuple(tup);
	table_close(pg_class, RowExclusiveLock);
	relation_close(rel, NoLock);
	/* the new file number is seen by this transaction's next commands */
	CommandCounterIncrement();
	PG_RETURN_VOID();
}

/*
 * prolly_branch_drop_file(rel regclass, filenode oid)
 *
 * Drops a file number of rel's that the catalog no longer names, when the
 * transaction commits.
 */
Datum
prolly_branch_drop_file(PG_FUNCTION_ARGS)
{
	Relation	rel = open_with_storage(PG_GETARG_OID(0));
	RelFileNumber keep = rel->rd_locator.relNumber;
	RelFileNumber drop = PG_GETARG_OID(1);

	if (drop == keep)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_IN_USE),
				 errmsg("file %u is \"%s\"'s current file", drop, RelationGetRelationName(rel))));
	/* the pending delete takes the relation's address: lend it the other file's */
	RelationCloseSmgr(rel);
	rel->rd_locator.relNumber = drop;
	RelationDropStorage(rel);
	rel->rd_locator.relNumber = keep;
	RelationCloseSmgr(rel);
	relation_close(rel, NoLock);
	PG_RETURN_VOID();
}
