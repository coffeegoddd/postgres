/*-------------------------------------------------------------------------
 *
 * commitindex_refs.c
 *	  Commit index implementation "refs": the index kept as entries of the
 *	  database's refs (their side_put()), two for each commit.
 *
 * In space 'c', by id; in space 'd', by date, then id:
 *
 *	'c' id			-> the entry
 *	'd' date id		-> the entry
 *
 * An id is its hash's length (a byte), then the hash; a date is its
 * microseconds since the Postgres epoch, 8 bytes big-endian with the sign
 * bit flipped, so that dates sort as their bytes do.  An entry is the
 * date (8 bytes, as in the key), the height (8 bytes big-endian), the
 * count of parents (a byte) and each parent's id, its hash's bytes
 * complemented: an entry names no chunk, and no bytes of it spell a
 * reference as a value that names one does (a hash's length, then the
 * hash), so nothing reads one there.
 *
 * The refs' tree is content-defined: the entries of a range of dates are
 * the same bytes, and sort into the same nodes, however they were added.
 * Each commit's entries are keyed by its content, so a commit on one
 * branch writes them as it writes its own branch's entries, and commits on
 * other branches don't wait for it.
 *
 * Noting a head indexes the commits it reaches that aren't indexed, in
 * parent-first order (a walk of what isn't, depth first), so an indexed
 * commit's ancestors are always indexed, as far as the store holds them:
 * a commit's noting reads it and finds its parents indexed, and a fetch's
 * or clone's new head indexes what it brought, once.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/commitindex_refs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_builtins.h"
#include "miscadmin.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"

#define SPACE_BY_ID		'c'
#define SPACE_BY_DATE	'd'

#define DATE_LEN		8

static const ProllyRefsRoutine *
index_refs(void)
{
	static const ProllyRefsRoutine *refs = NULL;

	if (refs == NULL)
	{
		ProllyDatabaseComponents dc;

		prolly_database_components(&dc);
		refs = dc.refs;
	}
	return refs;
}

static void
put_be64(StringInfo out, uint64 v)
{
	for (int i = 7; i >= 0; i--)
		appendStringInfoChar(out, (char) (v >> (i * 8)));
}

static uint64
get_be64(const uint8 *p)
{
	uint64		v = 0;

	for (int i = 0; i < 8; i++)
		v = (v << 8) | p[i];
	return v;
}

static void
put_date(StringInfo out, TimestampTz at)
{
	put_be64(out, ((uint64) at) ^ ((uint64) 1 << 63));
}

static TimestampTz
get_date(const uint8 *p)
{
	return (TimestampTz) (get_be64(p) ^ ((uint64) 1 << 63));
}

static void
put_id(StringInfo out, const ProllyHash *id)
{
	appendStringInfoChar(out, (char) id->len);
	appendBinaryStringInfo(out, (const char *) id->bytes, id->len);
}

/* An id at p (of end); NULL if there isn't one. */
static const uint8 *
get_id(const uint8 *p, const uint8 *end, ProllyHash *id)
{
	if (p >= end || *p > PROLLY_HASH_MAX_LEN || end - p - 1 < *p)
		return NULL;
	memset(id, 0, sizeof(*id));
	id->len = *p;
	memcpy(id->bytes, p + 1, id->len);
	return p + 1 + id->len;
}

/* An id in an entry's value: its bytes complemented. */
static void
put_value_id(StringInfo out, const ProllyHash *id)
{
	appendStringInfoChar(out, (char) id->len);
	for (int i = 0; i < id->len; i++)
		appendStringInfoChar(out, (char) ~id->bytes[i]);
}

static const uint8 *
get_value_id(const uint8 *p, const uint8 *end, ProllyHash *id)
{
	if ((p = get_id(p, end, id)) != NULL)
		for (int i = 0; i < id->len; i++)
			id->bytes[i] = (uint8) ~id->bytes[i];
	return p;
}

static void
entry_encode(StringInfo out, const ProllyCommitIndexEntry *e)
{
	put_date(out, e->committed_at);
	put_be64(out, (uint64) e->height);
	appendStringInfoChar(out, (char) e->nparents);
	for (int i = 0; i < e->nparents; i++)
		put_value_id(out, &e->parents[i]);
}

static void
entry_corrupt(const ProllyHash *id)
{
	ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("the commit index's entry for commit %s is corrupt",
					prolly_hash_hex(id))));
}

static void
entry_decode(const uint8 *p, Size len, const ProllyHash *id,
			 ProllyCommitIndexEntry *e)
{
	const uint8 *end = p + len;

	if (len < 2 * DATE_LEN + 1)
		entry_corrupt(id);
	memset(e, 0, sizeof(*e));
	e->id = *id;
	e->committed_at = get_date(p);
	e->height = (int64) get_be64(p + DATE_LEN);
	e->nparents = p[2 * DATE_LEN];
	if (e->nparents > PROLLY_MAX_PARENTS)
		entry_corrupt(id);
	p += 2 * DATE_LEN + 1;
	for (int i = 0; i < e->nparents; i++)
		if ((p = get_value_id(p, end, &e->parents[i])) == NULL)
			entry_corrupt(id);
}

static bool
refs_index_lookup(const ProllyHash *id, ProllyCommitIndexEntry *entry)
{
	const ProllyRefsRoutine *refs = index_refs();
	StringInfoData k;
	StringInfoData v;
	ProllyKey	key;
	bool		found;

	if (id->len == 0)
		return false;
	initStringInfo(&k);
	initStringInfo(&v);
	put_id(&k, id);
	key.data = (const uint8 *) k.data;
	key.len = k.len;
	found = PROLLY_CALL(refs, side_get, SPACE_BY_ID, &key, &v);
	if (found && entry)
		entry_decode((const uint8 *) v.data, v.len, id, entry);
	pfree(k.data);
	pfree(v.data);
	return found;
}

static void
entry_put(const ProllyCommitIndexEntry *e)
{
	const ProllyRefsRoutine *refs = index_refs();
	StringInfoData k;
	StringInfoData v;
	ProllyKey	key;
	ProllyValue value;

	initStringInfo(&k);
	initStringInfo(&v);
	entry_encode(&v, e);
	value.data = (const uint8 *) v.data;
	value.len = v.len;

	put_id(&k, &e->id);
	key.data = (const uint8 *) k.data;
	key.len = k.len;
	PROLLY_CALL(refs, side_put, SPACE_BY_ID, &key, &value);

	resetStringInfo(&k);
	put_date(&k, e->committed_at);
	put_id(&k, &e->id);
	key.data = (const uint8 *) k.data;
	key.len = k.len;
	PROLLY_CALL(refs, side_put, SPACE_BY_DATE, &key, &value);
	pfree(k.data);
	pfree(v.data);
}

/* A commit of the walk: indexed (or not in the store) once done. */
typedef struct NoteItem
{
	ProllyHash	id;				/* the hash table's key */
	bool		done;
	bool		indexed;		/* (done) indexed, or was: has an entry */
	int64		height;			/* (indexed) */
} NoteItem;

static NoteItem *
note_item(HTAB *items, const ProllyHash *id)
{
	ProllyHash	key;
	NoteItem   *item;
	bool		found;

	memset(&key, 0, sizeof(key));
	key.len = id->len;
	memcpy(key.bytes, id->bytes, id->len);
	item = hash_search(items, &key, HASH_ENTER, &found);
	if (!found)
	{
		ProllyCommitIndexEntry e;

		item->done = false;
		item->indexed = false;
		item->height = 0;
		if (refs_index_lookup(id, &e))
		{
			item->done = true;
			item->indexed = true;
			item->height = e.height;
		}
	}
	return item;
}

static void
refs_index_note_head(const ProllyHash *head,
					 const ProllyCommitIndexEntry *known)
{
	const ProllyRefsRoutine *refs = index_refs();
	MemoryContext cxt;
	MemoryContext old;
	HTAB	   *items;
	HASHCTL		ctl;
	ProllyHash *stack;
	int			depth = 0;
	int			cap = 64;

	/* (the commit a head is at is indexed already, unless it is new) */
	if (head->len == 0 || refs_index_lookup(head, NULL))
		return;

	/* (a commit just made, on parents indexed: its entry as made) */
	if (known != NULL)
	{
		bool		parents_indexed = true;

		for (int i = 0; i < known->nparents && parents_indexed; i++)
			parents_indexed = refs_index_lookup(&known->parents[i], NULL);
		if (parents_indexed)
		{
			entry_put(known);
			return;
		}
	}

	cxt = AllocSetContextCreate(CurrentMemoryContext, "prolly commit index",
								ALLOCSET_DEFAULT_SIZES);
	old = MemoryContextSwitchTo(cxt);
	ctl.keysize = sizeof(ProllyHash);
	ctl.entrysize = sizeof(NoteItem);
	ctl.hcxt = cxt;
	items = hash_create("prolly commit index walk", 64, &ctl,
						HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	stack = palloc_array(ProllyHash, cap);
	stack[depth++] = *head;
	(void) note_item(items, head);
	while (depth > 0)
	{
		ProllyHash	id = stack[depth - 1];
		NoteItem   *item = note_item(items, &id);
		ProllyCommitInfo info;
		bool		pushed = false;

		CHECK_FOR_INTERRUPTS();
		if (item->done)
		{
			depth--;
			continue;
		}
		if (!PROLLY_CALL(refs, commit_get, &id, &info))
		{
			/* (not in the store: nothing below it is indexed from here) */
			item->done = true;
			depth--;
			continue;
		}
		for (int i = 0; i < info.nparents; i++)
		{
			NoteItem   *p = note_item(items, &info.parents[i]);

			if (p->done)
				continue;
			if (depth == cap)
			{
				cap *= 2;
				stack = repalloc_array(stack, ProllyHash, cap);
			}
			stack[depth++] = info.parents[i];
			pushed = true;
		}
		if (pushed)
			continue;

		/* (every parent is indexed, or not in the store: index it) */
		{
			ProllyCommitIndexEntry e;
			int64		height = 1;

			memset(&e, 0, sizeof(e));
			e.id = id;
			e.committed_at = info.committed_at;
			e.nparents = info.nparents;
			for (int i = 0; i < info.nparents; i++)
			{
				NoteItem   *p = note_item(items, &info.parents[i]);

				e.parents[i] = info.parents[i];
				if (p->indexed)
					height = Max(height, p->height + 1);
			}
			e.height = info.has_height ? info.height : height;
			entry_put(&e);
			item = note_item(items, &id);
			item->done = true;
			item->indexed = true;
			item->height = e.height;
		}
		depth--;
	}
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cxt);
}

typedef struct ScanState
{
	TimestampTz from;
	TimestampTz to;
	bool		backward;
	ProllyCommitIndexVisit visit;
	void	   *arg;
} ScanState;

static bool
scan_visit(const ProllyKey *key, const ProllyValue *value, void *arg)
{
	ScanState  *ss = arg;
	ProllyCommitIndexEntry e;
	ProllyHash	id;
	TimestampTz at;

	if (key->len < DATE_LEN + 1 ||
		get_id(key->data + DATE_LEN, key->data + key->len, &id) == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("the commit index has a corrupt key")));
	at = get_date(key->data);
	if (ss->backward ? at < ss->from : at > ss->to)
		return false;
	if (ss->backward ? at > ss->to : at < ss->from)
		return true;
	entry_decode(value->data, value->len, &id, &e);
	return ss->visit(&e, ss->arg);
}

static void
refs_index_scan(TimestampTz from, TimestampTz to, ScanDirection direction,
				ProllyCommitIndexVisit visit, void *arg)
{
	const ProllyRefsRoutine *refs = index_refs();
	ScanState	ss;
	StringInfoData k;
	ProllyKey	key;

	if (from > to)
		return;
	ss.from = from;
	ss.to = to;
	ss.backward = ScanDirectionIsBackward(direction);
	ss.visit = visit;
	ss.arg = arg;
	initStringInfo(&k);
	if (ss.backward)
	{
		/* (past every id of date to: a length byte no id has) */
		put_date(&k, to);
		appendStringInfoChar(&k, (char) 0xff);
	}
	else
		put_date(&k, from);
	key.data = (const uint8 *) k.data;
	key.len = k.len;
	PROLLY_CALL(refs, side_scan, SPACE_BY_DATE, &key, direction,
				scan_visit, &ss);
	pfree(k.data);
}

static void
refs_index_forget(const ProllyHash *id)
{
	const ProllyRefsRoutine *refs = index_refs();
	ProllyCommitIndexEntry e;
	StringInfoData k;
	ProllyKey	key;

	if (!refs_index_lookup(id, &e))
		return;
	initStringInfo(&k);
	put_id(&k, id);
	key.data = (const uint8 *) k.data;
	key.len = k.len;
	PROLLY_CALL(refs, side_put, SPACE_BY_ID, &key, NULL);
	resetStringInfo(&k);
	put_date(&k, e.committed_at);
	put_id(&k, id);
	key.data = (const uint8 *) k.data;
	key.len = k.len;
	PROLLY_CALL(refs, side_put, SPACE_BY_DATE, &key, NULL);
	pfree(k.data);
}

const ProllyCommitIndexRoutine prolly_commitindex_refs = {
	.info = {
		.iface = PROLLY_IFACE_COMMIT_INDEX,
		.impl_id = 1,
		.format_version = 1,
		.name = "refs",
		.description = "refs: commits by date and by id, as entries of the refs",
	},
	.note_head = refs_index_note_head,
	.lookup = refs_index_lookup,
	.scan = refs_index_scan,
	.forget = refs_index_forget,
};
