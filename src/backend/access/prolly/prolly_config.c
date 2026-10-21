/*-------------------------------------------------------------------------
 *
 * prolly_config.c
 *	  Choosing which implementation of each interface prolly uses.
 *
 * A prolly table's rows are in pages; the components its rows' history is
 * folded with (keying, encoders, hash, and trees of chunks) are its
 * access method's profile.  A database's version control (refs, transfer,
 * diff and merge, canonical form, reclamation) is its branches' as copies
 * of its versioned schemas.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/prolly_config.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_config.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/pg_am.h"
#include "catalog/pg_class.h"
#include "miscadmin.h"
#include "utils/guc_hooks.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

/* The prolly table access method's components, by ProllyInterface. */
ProllyRefsEndHook prolly_refs_end_hook = NULL;

static const char *const history_profile[PROLLY_IFACE_LAST + 1] = {
	[PROLLY_IFACE_CHUNKER] = "bytes",
	[PROLLY_IFACE_CONTENT_HASH] = "blake3",
	[PROLLY_IFACE_KEY_ENCODER] = "ordered",
	[PROLLY_IFACE_VALUE_ENCODER] = "canonical",
	[PROLLY_IFACE_TABLE_KEYING] = "pk_or_prefix",
	[PROLLY_IFACE_TREE] = "cow",
	[PROLLY_IFACE_VALUE_MEMO] = "shared",
	[PROLLY_IFACE_CHANGE_FINDER] = "log",
	[PROLLY_IFACE_CHANGE_LOG] = "relation",
};

/*
 * A database's version control: its branches are copies of its versioned
 * schemas, their commits folded into its chunk store.
 */
static const char *const copies_profile[PROLLY_IFACE_LAST + 1] = {
	[PROLLY_IFACE_REFS] = "copies",
	[PROLLY_IFACE_TRANSFER] = "chunks",
	[PROLLY_IFACE_DIFF_MERGE] = "folded",
	[PROLLY_IFACE_CANONICALIZER] = "folded",
	[PROLLY_IFACE_RECLAMATION] = "copying",
	[PROLLY_IFACE_WRITERS] = "modes",
};

static const ProllyImplInfo *
profile_impl(const char *const *impls, ProllyInterface iface)
{
	if (impls[iface] == NULL)
		return NULL;
	return prolly_get_impl(iface, impls[iface]);
}

Oid
prolly_table_am_for_persistence(Oid amoid, char relpersistence, bool explicit)
{
	if (relpersistence == RELPERSISTENCE_PERMANENT || amoid != PROLLY_TABLE_AM_OID)
		return amoid;
	if (explicit)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("prolly storage supports only permanent tables"),
				 errdetail("Temporary and unlogged tables are never part of a commit."),
				 errhint("Leave out USING, and the table is stored by heap.")));
	return HEAP_TABLE_AM_OID;
}

void
prolly_check_unlogged(Relation rel)
{
	if (rel->rd_rel->relam == PROLLY_TABLE_AM_OID)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot change prolly table \"%s\" to unlogged",
						RelationGetRelationName(rel)),
				 errdetail("Prolly tables are always permanent."),
				 errhint("Use ALTER TABLE ... SET ACCESS METHOD heap first.")));
}

static const ProllyChangeLogRoutine *change_log(void);

void
prolly_components_for_relation(Relation rel, ProllyComponents *components)
{
	Oid			amoid;

	/* (an index's are its table's) */
	if (rel->rd_rel->relkind == RELKIND_INDEX ||
		rel->rd_rel->relkind == RELKIND_PARTITIONED_INDEX)
		amoid = get_rel_relam(rel->rd_index->indrelid);
	else
		amoid = rel->rd_rel->relam;

	if (amoid != PROLLY_TABLE_AM_OID)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a prolly table",
						RelationGetRelationName(rel))));
	prolly_history_components(components);
}

void
prolly_history_components(ProllyComponents *components)
{
	components->chunker = (const ProllyChunkerRoutine *)
		profile_impl(history_profile, PROLLY_IFACE_CHUNKER);
	components->hash = prolly_content_hash();
	components->key_encoder = (const ProllyKeyEncoderRoutine *)
		profile_impl(history_profile, PROLLY_IFACE_KEY_ENCODER);
	components->value_encoder = (const ProllyValueEncoderRoutine *)
		profile_impl(history_profile, PROLLY_IFACE_VALUE_ENCODER);
	components->tree = (const ProllyTreeRoutine *)
		profile_impl(history_profile, PROLLY_IFACE_TREE);
	components->keying = (const ProllyKeyingRoutine *)
		profile_impl(history_profile, PROLLY_IFACE_TABLE_KEYING);
	components->change_finder = prolly_change_finder();
	components->change_log = change_log();
}

/* (empty: the profile's) */
char	   *debug_prolly_change_finder = NULL;

/*
 * A change finder that folds: one an extension registers is known only
 * once shared_preload_libraries is loaded, so a name set before then (in
 * postgresql.conf) is taken on trust, and a fold errors if it is none.
 */
bool
check_debug_prolly_change_finder(char **newval, void **extra, GucSource source)
{
	const ProllyChangeFinderRoutine *finder;

	if (**newval == '\0')
		return true;
	finder = (const ProllyChangeFinderRoutine *)
		prolly_lookup_impl(PROLLY_IFACE_CHANGE_FINDER, *newval);
	if (finder == NULL)
	{
		if (!process_shared_preload_libraries_done)
			return true;
		GUC_check_errdetail("There is no change finder named \"%s\".", *newval);
		return false;
	}
	if (finder->fold == NULL)
	{
		GUC_check_errdetail("Change finder \"%s\" cannot fold.", *newval);
		return false;
	}
	return true;
}

const ProllyChangeFinderRoutine *
prolly_change_finder(void)
{
	if (debug_prolly_change_finder != NULL && *debug_prolly_change_finder != '\0')
		return prolly_changes_lookup(debug_prolly_change_finder);
	return (const ProllyChangeFinderRoutine *)
		profile_impl(history_profile, PROLLY_IFACE_CHANGE_FINDER);
}

/*
 * The profile's change log, looked up once: told of every write.  Asked in
 * critical sections too (prolly_is_change_log), so it neither errors nor
 * allocates: the profile's log is built in, so the lookup finds it.
 */
static const ProllyChangeLogRoutine *
change_log(void)
{
	static const ProllyChangeLogRoutine *routine = NULL;
	static bool looked_up = false;

	if (!looked_up)
	{
		if (history_profile[PROLLY_IFACE_CHANGE_LOG] != NULL)
			routine = (const ProllyChangeLogRoutine *)
				prolly_lookup_impl(PROLLY_IFACE_CHANGE_LOG,
								   history_profile[PROLLY_IFACE_CHANGE_LOG]);
		looked_up = true;
	}
	return routine;
}

/*
 * Make relid's change log if it is a prolly table (or materialized view)
 * that has none.  Only under a lock writers wait for: a log made under a
 * weaker one (an ALTER's ShareUpdateExclusiveLock) isn't seen by a writer
 * whose transaction is open, whose rows would then be in no entry once a
 * fold had trusted the new log; it is made at the next ALTER that takes
 * one.
 */
void
prolly_make_change_log(Oid relid, LOCKMODE lockmode)
{
	const ProllyChangeLogRoutine *log = change_log();
	Relation	rel;
	char		relkind;

	if (log == NULL || lockmode < ShareLock ||
		get_rel_relam(relid) != PROLLY_TABLE_AM_OID)
		return;
	relkind = get_rel_relkind(relid);
	if (relkind != RELKIND_RELATION && relkind != RELKIND_MATVIEW)
		return;
	rel = table_open(relid, lockmode);
	if (PROLLY_CALL(log, identity, rel) == 0)
	{
		PROLLY_CALL(log, create, rel);
		CommandCounterIncrement();
	}
	table_close(rel, NoLock);
}

/*
 * Before the session's branch changes: entries the transaction holds for
 * the change logs are written to its branch's.
 */
void
prolly_change_logs_flush(void)
{
	const ProllyChangeLogRoutine *log = change_log();

	if (log != NULL && log->flush != NULL)
		log->flush();
}

void
prolly_note_change(Relation rel, const ItemPointerData *oldtid,
				   HeapTuple oldtup, const ItemPointerData *newtid)
{
	const ProllyChangeLogRoutine *log = change_log();

	if (log != NULL)
		PROLLY_CALL(log, note, rel, oldtid, oldtup, newtid);
}

double
prolly_change_log_pending(Relation rel)
{
	const ProllyChangeLogRoutine *log = change_log();

	if (log == NULL || log->pending == NULL)
		return 0;
	return log->pending(rel);
}

void
prolly_note_emptied(Relation rel)
{
	const ProllyChangeLogRoutine *log = change_log();

	if (log == NULL)
		return;
	if (log->note_emptied != NULL)
		log->note_emptied(rel);
	else
		PROLLY_CALL(log, note, rel, NULL, NULL, NULL);
}

void
prolly_note_made(Relation rel, const ItemPointerData *oldtid, HeapTuple oldtup,
				 HeapTuple *made, int nmade)
{
	const ProllyChangeLogRoutine *log = change_log();

	if (log == NULL)
		return;
	if (log->note_made != NULL)
		log->note_made(rel, oldtid, oldtup, made, nmade);
	else
		for (int i = 0; i < nmade; i++)
			PROLLY_CALL(log, note, rel, i == 0 ? oldtid : NULL,
						i == 0 ? oldtup : NULL, &made[i]->t_self);
}

Oid
prolly_change_log_relid(Oid relid, Oid relam)
{
	const ProllyChangeLogRoutine *log = change_log();

	if (relam != PROLLY_TABLE_AM_OID || log == NULL || log->relid == NULL)
		return InvalidOid;
	return log->relid(relid);
}

bool
prolly_change_log_sizes(Relation rel, ProllyChangeLogSizes *sizes)
{
	const ProllyChangeLogRoutine *log = change_log();

	memset(sizes, 0, sizeof(*sizes));
	if (log == NULL)
		return true;			/* (none is kept) */
	if (log->sizes == NULL)
		return false;
	(void) log->sizes(rel, sizes);
	return true;
}

/* (in critical sections too: no error, no allocation) */
bool
prolly_is_change_log(Relation rel)
{
	const ProllyChangeLogRoutine *log = change_log();

	return log != NULL && log->is_log != NULL && log->is_log(rel);
}

/*
 * The profile's refs, looked up once, as change_log() is: asked in critical
 * sections too (prolly_forget_folds()).
 */
static const ProllyRefsRoutine *
database_refs(void)
{
	static const ProllyRefsRoutine *routine = NULL;
	static bool looked_up = false;

	if (!looked_up)
	{
		if (copies_profile[PROLLY_IFACE_REFS] != NULL)
			routine = (const ProllyRefsRoutine *)
				prolly_lookup_impl(PROLLY_IFACE_REFS,
								   copies_profile[PROLLY_IFACE_REFS]);
		looked_up = true;
	}
	return routine;
}

void
prolly_note_branch_write(Oid branch)
{
	const ProllyRefsRoutine *refs = database_refs();

	if (refs != NULL && refs->note_write != NULL)
		refs->note_write(branch);
}

void
prolly_forget_folds(void)
{
	const ProllyRefsRoutine *refs = database_refs();

	if (refs != NULL && refs->forget_folds != NULL)
		refs->forget_folds();
}

void
prolly_note_prepared_ended(FullTransactionId xid)
{
	const ProllyRefsRoutine *refs = database_refs();

	if (refs != NULL && refs->prepared_ended != NULL)
		refs->prepared_ended(xid);
}

/*
 * The content hash chunks are named by: the profile's, the one hash every
 * chunk store, tree, transfer and canonical form of this server shares.
 * Not one to change on its own yet: a TOAST pointer to a tree (va_hash)
 * holds a hash of 32 bytes, so one of another length would be copied
 * short or long.
 */
const ProllyHashRoutine *
prolly_content_hash(void)
{
	return (const ProllyHashRoutine *)
		profile_impl(history_profile, PROLLY_IFACE_CONTENT_HASH);
}

void
prolly_database_components(ProllyDatabaseComponents *components)
{
	components->refs = (const ProllyRefsRoutine *)
		profile_impl(copies_profile, PROLLY_IFACE_REFS);
	components->transfer = (const ProllyTransferRoutine *)
		profile_impl(copies_profile, PROLLY_IFACE_TRANSFER);
	components->diff_merge = (const ProllyMergeRoutine *)
		profile_impl(copies_profile, PROLLY_IFACE_DIFF_MERGE);
	components->canonicalizer = (const ProllyCanonRoutine *)
		profile_impl(copies_profile, PROLLY_IFACE_CANONICALIZER);
	components->reclamation = (const ProllyReclaimRoutine *)
		profile_impl(copies_profile, PROLLY_IFACE_RECLAMATION);
	components->writers = (const ProllyWritersRoutine *)
		profile_impl(copies_profile, PROLLY_IFACE_WRITERS);
}

/* A data tree value's references: its row's. */
static void
keying_value_refs(const ProllyValue *value, void *arg, StringInfo refs)
{
	ProllyKeyingRefs *r = arg;
	ProllyValue row;
	uint64		count;

	PROLLY_CALL(r->routine, parse_value, r->keying, value, &row, &count);
	PROLLY_CALL(r->routine, row_refs, r->keying, &row, refs);
}

void
prolly_keying_use_env(ProllyKeyingRefs *refs, const ProllyKeyingRoutine *routine,
					  ProllyKeying *k, const ProllyTreeRoutine *tree,
					  ProllyTreeEnv *env)
{
	refs->routine = routine;
	refs->keying = k;
	refs->values.tree = tree;
	refs->values.env = env;
	refs->values.memo = (const ProllyValueMemoRoutine *)
		profile_impl(history_profile, PROLLY_IFACE_VALUE_MEMO);
	refs->values.pointers = false;
	PROLLY_CALL(routine, set_values, k, &refs->values);
	env->value_refs = keying_value_refs;
	env->value_refs_arg = refs;
}

/* A value's references, the value an encoded row itself. */
static void
keying_row_refs(const ProllyValue *value, void *arg, StringInfo refs)
{
	ProllyKeyingRefs *r = arg;

	PROLLY_CALL(r->routine, row_refs, r->keying, value, refs);
}

void
prolly_keying_use_env_rows(ProllyKeyingRefs *refs, ProllyTreeEnv *env)
{
	env->value_refs = keying_row_refs;
	env->value_refs_arg = refs;
}
