/*-------------------------------------------------------------------------
 *
 * window_archive.c
 *	  History window implementation "archive": history past a window
 *	  severed at a horizon kept in the refs.
 *
 * The horizon is entries of the refs (their side_put(), space 'h'):
 *
 *	'e' parent boundary	-> a boundary edge: the boundary commit keeps no
 *						   parent (each an id: its hash's length, a byte,
 *						   then the hash); the fragment's number (8 bytes
 *						   big-endian)
 *	'f' number			-> a fragment (8 bytes big-endian; see
 *						   fragment_encode())
 *	'n'					-> the next fragment's number
 *
 * None names a chunk: a collection asks edges() which parents not to
 * follow from which commits, and keeps nothing for a fragment.
 *
 * What is kept.  Every commit the refs name is kept (a branch's head, a
 * tag's, a remote-tracking branch's, a stash's, a merge's or rebase's in
 * progress, a pin's, a kept commit's).  From each branch head (but a
 * session's detached one, which is a ref only) a walk keeps the commits
 * its window does (dated at or after the cut), and every commit on the
 * way to its merge base with each other branch head (one at least as high
 * as the lowest of those bases): merging two live branches never needs a
 * severed commit.  The commits kept are the window; an edge from one to a
 * parent outside it is a boundary edge, and the parent a top of the
 * fragment, which is every commit the tops reach outside the window and
 * the horizon there was already.
 *
 * A fragment's identity (its root) is the root hash of a tree of its
 * commits' index entries, keyed by date then id, built as any tree is:
 * the same commits give the same root wherever they are severed.  Its
 * digest is a hash over the sorted hashes of the chunks its commits reach
 * (their own trees, their roots' and dependencies'), which repair
 * recomputes, from the same tops down to the same stops (the commits
 * outside the fragment its commits name), once the chunks are back.
 *
 * Severing changes no commit: hashes stay, and the boundary commits still
 * name their parents.  Readers that list history (dolt_log, history
 * tables) stop at the horizon, as a shallow clone's do; one that needs a
 * commit past it (dolt_at, a diff, a merge's base, a cherry-pick) finds
 * it missing once a collection freed it, and is told it is past the
 * horizon and how to repair it (prolly_window_check_missing()).
 *
 * A sever, a repair and a collection exclude one another: a collection
 * holds the horizon's lock shared, and those that change the horizon
 * exclusively, so a collection never frees by a horizon a repair has just
 * let go of.  A collection in the transaction that changed the horizon is
 * refused: the horizon might yet roll back, and what it freed with it.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/window_archive.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_builtins.h"
#include "access/prolly_datum.h"
#include "access/xact.h"
#include "catalog/pg_database.h"
#include "miscadmin.h"
#include "storage/lmgr.h"
#include "storage/lock.h"
#include "storage/proc.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"

#define SPACE_HORIZON	'h'
#define KEY_EDGE		'e'
#define KEY_FRAGMENT	'f'
#define KEY_NEXT		'n'

#define FRAGMENT_VERSION	1
#define FRAGMENT_DELETED	0x01

/* The horizon's lock: an object lock in the database, of this class. */
#define HORIZON_LOCK_CLASSID	((Oid) 0x50524848)

/* ------------------------------------------------------------------------
 * Components and encodings
 * ------------------------------------------------------------------------
 */

static ProllyDatabaseComponents dc;
static bool dc_ready = false;
static ProllyTreeEnv tree_env;
static const ProllyTreeRoutine *tree;

static void
window_init(void)
{
	if (dc_ready)
		return;
	prolly_database_components(&dc);
	tree = prolly_tree_lookup("cow");
	memset(&tree_env, 0, sizeof(tree_env));
	tree_env.chunk_format = prolly_chunkformat_lookup("basic");
	tree_env.chunker = prolly_chunker_lookup("bytes");
	tree_env.hash = prolly_content_hash();
	tree_env.chunk_store = prolly_chunk_store();
	tree_env.chunks = PROLLY_CALL(tree_env.chunk_store, open, MyDatabaseId,
								  MyDatabaseTableSpace);
	tree_env.compare = prolly_bytes_compare;
	dc_ready = true;
}

static void
put_be64(StringInfo out, uint64 v)
{
	for (int i = 7; i >= 0; i--)
		appendStringInfoChar(out, (char) (v >> (i * 8)));
}

static uint64
get_be64(const char *p)
{
	uint64		v = 0;

	for (int i = 0; i < 8; i++)
		v = (v << 8) | (uint8) p[i];
	return v;
}

static void
put_id(StringInfo out, const ProllyHash *id)
{
	appendStringInfoChar(out, (char) id->len);
	appendBinaryStringInfo(out, (const char *) id->bytes, id->len);
}

/*
 * An id in a horizon entry's value: its bytes complemented, so that no
 * bytes of the value spell a reference as a value naming a chunk does.
 */
static void
put_value_id(StringInfo out, const ProllyHash *id)
{
	appendStringInfoChar(out, (char) id->len);
	for (int i = 0; i < id->len; i++)
		appendStringInfoChar(out, (char) ~id->bytes[i]);
}

static void
horizon_corrupt(void)
{
	ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("the history horizon's record is corrupt")));
}

static const char *
get_id(const char *p, const char *end, ProllyHash *id)
{
	if (p >= end || (uint8) *p > PROLLY_HASH_MAX_LEN ||
		end - p - 1 < (uint8) *p)
		horizon_corrupt();
	memset(id, 0, sizeof(*id));
	id->len = (uint8) *p;
	memcpy(id->bytes, p + 1, id->len);
	return p + 1 + id->len;
}

static const char *
get_value_id(const char *p, const char *end, ProllyHash *id)
{
	p = get_id(p, end, id);
	for (int i = 0; i < id->len; i++)
		id->bytes[i] = (uint8) ~id->bytes[i];
	return p;
}

static ProllyHash
hash_key(const ProllyHash *h)
{
	ProllyHash	k;

	memset(&k, 0, sizeof(k));
	k.len = h->len;
	memcpy(k.bytes, h->bytes, h->len);
	return k;
}

static bool
same_id(const ProllyHash *a, const ProllyHash *b)
{
	return a->len == b->len && memcmp(a->bytes, b->bytes, a->len) == 0;
}

static HTAB *
id_set(const char *name, Size entrysize)
{
	HASHCTL		ctl;

	ctl.keysize = sizeof(ProllyHash);
	ctl.entrysize = entrysize;
	ctl.hcxt = CurrentMemoryContext;
	return hash_create(name, 256, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
}

static bool
set_has(HTAB *set, const ProllyHash *id)
{
	ProllyHash	k = hash_key(id);

	return hash_search(set, &k, HASH_FIND, NULL) != NULL;
}

static bool
set_add(HTAB *set, const ProllyHash *id)
{
	ProllyHash	k = hash_key(id);
	bool		found;

	(void) hash_search(set, &k, HASH_ENTER, &found);
	return !found;
}

/* A fragment's record, encoded. */
static void
fragment_encode(StringInfo out, const ProllyWindowFragment *f, uint32 generation,
				int nstops, const ProllyHash *stops)
{
	appendStringInfoChar(out, FRAGMENT_VERSION);
	appendStringInfoChar(out, f->deleted ? FRAGMENT_DELETED : 0);
	put_be64(out, (uint64) f->severed_at);
	put_be64(out, (uint64) f->oldest);
	put_be64(out, (uint64) f->newest);
	put_be64(out, (uint64) f->commits);
	put_be64(out, (uint64) f->chunks);
	put_be64(out, (uint64) f->bytes);
	put_be64(out, generation);
	put_value_id(out, &f->root);
	put_value_id(out, &f->digest);
	prolly_put_varint(out, f->location ? strlen(f->location) : 0);
	if (f->location)
		appendStringInfoString(out, f->location);
	prolly_put_varint(out, f->ntops);
	for (int i = 0; i < f->ntops; i++)
		put_value_id(out, &f->tops[i]);
	prolly_put_varint(out, nstops);
	for (int i = 0; i < nstops; i++)
		put_value_id(out, &stops[i]);
}

typedef struct FragmentRecord
{
	ProllyWindowFragment f;
	uint32		generation;
	int			nstops;
	ProllyHash *stops;
} FragmentRecord;

static void
fragment_decode(const char *p, Size len, int64 number, FragmentRecord *r)
{
	const char *end = p + len;
	uint64		n;
	ProllyHash *tops;

	memset(r, 0, sizeof(*r));
	if (len < 2 + 7 * 8 + 8 || p[0] != FRAGMENT_VERSION)
		horizon_corrupt();
	r->f.number = number;
	r->f.deleted = (p[1] & FRAGMENT_DELETED) != 0;
	p += 2;
	r->f.severed_at = (TimestampTz) get_be64(p);
	r->f.oldest = (TimestampTz) get_be64(p + 8);
	r->f.newest = (TimestampTz) get_be64(p + 16);
	r->f.commits = (int64) get_be64(p + 24);
	r->f.chunks = (int64) get_be64(p + 32);
	r->f.bytes = (int64) get_be64(p + 40);
	r->generation = (uint32) get_be64(p + 48);
	p += 56;
	p = get_value_id(p, end, &r->f.root);
	p = get_value_id(p, end, &r->f.digest);
	n = prolly_get_varint(&p, end);
	if (n > (uint64) (end - p))
		horizon_corrupt();
	r->f.location = n > 0 ? pnstrdup(p, n) : NULL;
	p += n;
	n = prolly_get_varint(&p, end);
	if (n > (uint64) (end - p))
		horizon_corrupt();
	tops = palloc_array(ProllyHash, Max(n, 1));
	for (uint64 i = 0; i < n; i++)
		p = get_value_id(p, end, &tops[i]);
	r->f.ntops = (int) n;
	r->f.tops = tops;
	n = prolly_get_varint(&p, end);
	if (n > (uint64) (end - p))
		horizon_corrupt();
	r->stops = palloc_array(ProllyHash, Max(n, 1));
	for (uint64 i = 0; i < n; i++)
		p = get_value_id(p, end, &r->stops[i]);
	r->nstops = (int) n;
}

static void
number_key(StringInfo k, char what, int64 number)
{
	appendStringInfoChar(k, what);
	put_be64(k, (uint64) number);
}

/* ------------------------------------------------------------------------
 * The horizon as read
 * ------------------------------------------------------------------------
 */

/*
 * The parents the horizon names (ProllyHash), read once a transaction:
 * the horizon changes only holding every ref and this lock, and a reader
 * of a horizon a sever moves meanwhile is no worse off than one that read
 * before it.
 */
static HTAB *archived_set = NULL;
static MemoryContext archived_cxt = NULL;
static LocalTransactionId archived_lxid = InvalidLocalTransactionId;
static LocalTransactionId changed_lxid = InvalidLocalTransactionId;

static bool
collect_edge(const ProllyKey *key, const ProllyValue *value, void *arg)
{
	ProllyWindowEdgeVisit visit = ((void **) arg)[0];
	void	   *varg = ((void **) arg)[1];
	const char *p = (const char *) key->data;
	const char *end = p + key->len;
	ProllyHash	parent;
	ProllyHash	boundary;

	if (p >= end || *p != KEY_EDGE)
		return false;
	p = get_id(p + 1, end, &parent);
	(void) get_id(p, end, &boundary);
	visit(&boundary, &parent, varg);
	return true;
}

static void
visit_edges(const ProllyHash *root, ProllyChunkRead read, void *read_arg,
			ProllyWindowEdgeVisit visit, void *arg)
{
	char		e = KEY_EDGE;
	ProllyKey	from = {(const uint8 *) &e, 1};
	void	   *args[2] = {(void *) visit, arg};

	window_init();
	if (root != NULL)
		PROLLY_CALL(dc.refs, side_scan_at, root, read, read_arg,
					SPACE_HORIZON, &from, ForwardScanDirection, collect_edge,
					args);
	else
		PROLLY_CALL(dc.refs, side_scan, SPACE_HORIZON, &from,
					ForwardScanDirection, collect_edge, args);
}

static void
note_archived(const ProllyHash *boundary, const ProllyHash *parent, void *arg)
{
	(void) set_add((HTAB *) arg, parent);
}

static HTAB *
archived_parents(void)
{
	if (archived_set != NULL && archived_lxid == MyProc->vxid.lxid)
		return archived_set;
	if (archived_cxt == NULL)
		archived_cxt = AllocSetContextCreate(TopMemoryContext,
											 "prolly history horizon",
											 ALLOCSET_SMALL_SIZES);
	MemoryContextReset(archived_cxt);
	{
		MemoryContext old = MemoryContextSwitchTo(archived_cxt);

		archived_set = id_set("prolly horizon parents", sizeof(ProllyHash));
		archived_lxid = InvalidLocalTransactionId;
		visit_edges(NULL, NULL, NULL, note_archived, archived_set);
		MemoryContextSwitchTo(old);
	}
	archived_lxid = MyProc->vxid.lxid;
	return archived_set;
}

/* The horizon changed in this transaction: read it again from here. */
static void
horizon_changed(void)
{
	changed_lxid = MyProc->vxid.lxid;
	archived_lxid = InvalidLocalTransactionId;
}

static bool
archive_archived(const ProllyHash *id)
{
	if (!IsTransactionState() || id->len == 0)
		return false;
	window_init();
	if (!PROLLY_HAS(dc.refs, side_scan))
		return false;
	return set_has(archived_parents(), id);
}

static void
horizon_lock(LOCKMODE mode)
{
	LOCKTAG		tag;

	SET_LOCKTAG_OBJECT(tag, MyDatabaseId, HORIZON_LOCK_CLASSID, 0, 0);
	(void) LockAcquire(&tag, mode, false, false);
}

static void
archive_edges(const ProllyHash *refs_root, ProllyChunkRead read,
			  void *read_arg, ProllyWindowEdgeVisit visit, void *arg)
{
	if (changed_lxid == MyProc->vxid.lxid)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("a collection can't run in the transaction that changed the history horizon"),
				 errhint("Commit the transaction first.")));
	/* (until the transaction ends: no sever or repair meanwhile) */
	horizon_lock(ShareLock);
	visit_edges(refs_root, read, read_arg, visit, arg);
}

static bool
collect_fragment(const ProllyKey *key, const ProllyValue *value, void *arg)
{
	ProllyWindowFragmentVisit visit = ((void **) arg)[0];
	void	   *varg = ((void **) arg)[1];
	FragmentRecord r;

	if (key->len != 9 || key->data[0] != KEY_FRAGMENT)
		return false;
	fragment_decode((const char *) value->data, value->len,
					(int64) get_be64((const char *) key->data + 1), &r);
	visit(&r.f, varg);
	return true;
}

/* Fragments, newest first. */
static void
archive_fragments(ProllyWindowFragmentVisit visit, void *arg)
{
	StringInfoData k;
	ProllyKey	from;
	void	   *args[2] = {(void *) visit, arg};

	window_init();
	initStringInfo(&k);
	number_key(&k, KEY_FRAGMENT, PG_INT64_MAX);
	from.data = (const uint8 *) k.data;
	from.len = k.len;
	PROLLY_CALL(dc.refs, side_scan, SPACE_HORIZON, &from,
				BackwardScanDirection, collect_fragment, args);
	pfree(k.data);
}

/* ------------------------------------------------------------------------
 * Plans
 * ------------------------------------------------------------------------
 */

/* What a plan knows of a commit: as the commit records it. */
typedef struct PlanEntry
{
	TimestampTz committed_at;
	int64		height;
	int			nparents;
	ProllyHash	parents[PROLLY_MAX_PARENTS];
} PlanEntry;

/* A commit of a plan's walk. */
typedef struct PlanCommit
{
	ProllyHash	id;				/* key */
	PlanEntry	e;
	bool		kept;
	bool		pinned;			/* kept as a ref's commit, not a window's */
} PlanCommit;

typedef struct Head
{
	char	   *name;
	ProllyHash	head;
	int64		floor;			/* keeps every commit at least this high */
} Head;

typedef struct Plan
{
	TimestampTz cut;			/* a branch's window keeps commits dated at
								 * or after it */
	HTAB	   *commits;		/* PlanCommit, by id */
	List	   *heads;			/* Head *, branch heads */
	List	   *pins;			/* ProllyHash *, the other commits refs name */
	List	   *kept;			/* PlanCommit *, the window */
	List	   *edges;			/* ProllyHash *, pairs: boundary, parent */
	List	   *severed;		/* PlanCommit *, the fragment */
	List	   *stops;			/* ProllyHash *, outside it, its commits name */
	HTAB	   *archived;		/* the horizon's parents now */
	HTAB	   *heights;		/* of commits recording none */
} Plan;

static bool
lookup_commit(const ProllyHash *id, ProllyCommitInfo *info, void *arg)
{
	return PROLLY_CALL(dc.refs, commit_get, id, info);
}

static PlanCommit *
plan_commit(Plan *plan, const ProllyHash *id)
{
	ProllyHash	k = hash_key(id);
	PlanCommit *pc;
	bool		found;

	pc = hash_search(plan->commits, &k, HASH_ENTER, &found);
	if (!found)
	{
		ProllyCommitInfo info;

		pc->kept = false;
		pc->pinned = false;
		if (!PROLLY_CALL(dc.refs, commit_get, id, &info))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("commit %s is missing from the chunk store",
							prolly_hash_hex(id))));
		pc->e.committed_at = info.committed_at;
		pc->e.nparents = info.nparents;
		for (int i = 0; i < info.nparents; i++)
			pc->e.parents[i] = info.parents[i];
		if (info.has_height)
			pc->e.height = info.height;
		else
		{
			if (plan->heights == NULL)
				plan->heights = prolly_history_heights();
			pc->e.height = prolly_history_height(id, plan->heights,
												 lookup_commit, NULL);
		}
	}
	return pc;
}

static void
note_branch(const char *name, const ProllyHash *head, void *arg)
{
	Plan	   *plan = arg;

	if (head->len == 0)
		return;
	if (strncmp(name, PROLLY_DETACHED_PREFIX,
				strlen(PROLLY_DETACHED_PREFIX)) == 0)
	{
		plan->pins = lappend(plan->pins,
							 memcpy(palloc(sizeof(ProllyHash)), head,
									sizeof(ProllyHash)));
		return;
	}
	{
		Head	   *h = palloc0(sizeof(Head));

		h->name = pstrdup(name);
		h->head = *head;
		h->floor = PG_INT64_MAX;
		plan->heads = lappend(plan->heads, h);
	}
}

/* Every chunk the refs' entries name, as a collection reads them. */
typedef struct RefsNamed
{
	List	  **named;
	StringInfoData buf;
} RefsNamed;

static void
note_named(const ProllyHash *ref, void *arg)
{
	RefsNamed  *rn = arg;

	*rn->named = lappend(*rn->named, memcpy(palloc(sizeof(ProllyHash)), ref,
											sizeof(ProllyHash)));
}

static bool
read_refs_node(const ProllyNodeRef *node, uint16 level, void *arg)
{
	RefsNamed  *rn = arg;

	resetStringInfo(&rn->buf);
	if (!PROLLY_CALL(tree_env.chunk_store, get, tree_env.chunks, &node->hash,
					 &rn->buf))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("the refs' node %s is missing from the chunk store",
						prolly_hash_hex(&node->hash))));
	/* (a leaf's: an inner node's are its children, walked anyway) */
	if (level == 0)
		prolly_chunk_references(rn->buf.data, rn->buf.len, note_named, rn);
	return true;
}

/*
 * The commits the refs name that aren't branch heads: each a pin of the
 * window.  A tag's object names its commit.
 */
static void
note_pins(Plan *plan)
{
	ProllyHash	root;
	ProllyNodeRef ref;
	List	   *named = NIL;
	RefsNamed	rn = {&named};
	ListCell   *lc;

	PROLLY_CALL(tree_env.chunk_store, root, tree_env.chunks, &root);
	if (root.len == 0)
		return;
	ref.hash = root;
	initStringInfo(&rn.buf);
	(void) PROLLY_CALL(tree, walk, &tree_env, &ref, read_refs_node, &rn);
	foreach(lc, named)
	{
		ProllyHash *h = lfirst(lc);
		ProllyCommitInfo info;
		ProllyTagInfo tag;

		if (!PROLLY_CALL(tree_env.chunk_store, has, tree_env.chunks, h))
			continue;
		if (PROLLY_CALL(dc.refs, commit_get, h, &info))
			plan->pins = lappend(plan->pins, h);
		else if (PROLLY_HAS(dc.refs, tag_get) &&
				 PROLLY_CALL(dc.refs, tag_get, h, &tag))
			plan->pins = lappend(plan->pins,
								 memcpy(palloc(sizeof(ProllyHash)), &tag.commit,
										sizeof(ProllyHash)));
	}
	pfree(rn.buf.data);
}

/* Each branch head keeps the commits down to its merge bases with others. */
static void
note_merge_bases(Plan *plan)
{
	int			n = list_length(plan->heads);

	for (int i = 0; i < n; i++)
	{
		Head	   *a = list_nth(plan->heads, i);

		for (int j = i + 1; j < n; j++)
		{
			Head	   *b = list_nth(plan->heads, j);
			ProllyHash	base;
			PlanCommit *pc;

			if (!PROLLY_CALL(dc.refs, merge_base, &a->head, &b->head, &base) ||
				base.len == 0)
				continue;
			pc = plan_commit(plan, &base);
			a->floor = Min(a->floor, pc->e.height);
			b->floor = Min(b->floor, pc->e.height);
		}
	}
}

static void
keep(Plan *plan, PlanCommit *pc)
{
	if (pc->kept)
		return;
	pc->kept = true;
	plan->kept = lappend(plan->kept, pc);
}

/* A branch head's window: walked from it down to what it doesn't keep. */
static void
walk_window(Plan *plan, Head *h)
{
	List	   *todo = list_make1(&h->head);
	HTAB	   *seen = id_set("prolly window walk", sizeof(ProllyHash));

	(void) set_add(seen, &h->head);
	while (todo != NIL)
	{
		ProllyHash *id = llast(todo);
		PlanCommit *pc;

		todo = list_delete_last(todo);
		CHECK_FOR_INTERRUPTS();
		pc = plan_commit(plan, id);
		if (!(same_id(id, &h->head) || pc->e.committed_at >= plan->cut ||
			  pc->e.height >= h->floor))
			continue;
		keep(plan, pc);
		for (int i = 0; i < pc->e.nparents; i++)
		{
			ProllyHash *p = &pc->e.parents[i];

			if (set_has(plan->archived, p) || !set_add(seen, p))
				continue;
			todo = lappend(todo, p);
		}
	}
	hash_destroy(seen);
}

static void
add_pair(List **list, const ProllyHash *a, const ProllyHash *b)
{
	ProllyHash *pair = palloc_array(ProllyHash, 2);

	pair[0] = *a;
	pair[1] = *b;
	*list = lappend(*list, pair);
}

/*
 * The plan with cut: the window, the boundary edges from it, and the
 * fragment beyond them.  The caller holds every ref.
 */
static void
make_plan(Plan *plan, TimestampTz cut)
{
	ListCell   *lc;
	HTAB	   *fragment;
	List	   *todo = NIL;

	memset(plan, 0, sizeof(*plan));
	plan->cut = cut;
	plan->commits = id_set("prolly window plan", sizeof(PlanCommit));
	plan->archived = archived_parents();
	PROLLY_CALL(dc.refs, branches, note_branch, plan);
	note_pins(plan);
	note_merge_bases(plan);
	foreach(lc, plan->heads)
		walk_window(plan, lfirst(lc));
	foreach(lc, plan->pins)
	{
		PlanCommit *pc = plan_commit(plan, lfirst(lc));

		if (!pc->kept)
		{
			pc->pinned = true;
			keep(plan, pc);
		}
	}

	/* the boundary: each parent of a kept commit that isn't kept */
	foreach(lc, plan->kept)
	{
		PlanCommit *pc = lfirst(lc);

		for (int i = 0; i < pc->e.nparents; i++)
		{
			ProllyHash *p = &pc->e.parents[i];
			PlanCommit *parent;

			if (set_has(plan->archived, p))
				continue;
			parent = plan_commit(plan, p);
			if (!parent->kept)
			{
				add_pair(&plan->edges, &pc->id, p);
				todo = lappend(todo, p);
			}
		}
	}

	/* the fragment: what the new tops reach that isn't kept or past */
	fragment = id_set("prolly window fragment", sizeof(ProllyHash));
	{
		HTAB	   *stops = id_set("prolly window stops", sizeof(ProllyHash));
		List	   *tops = todo;

		/* (a top two boundary commits name is walked from once) */
		todo = NIL;
		foreach(lc, tops)
			if (set_add(fragment, lfirst(lc)))
				todo = lappend(todo, lfirst(lc));
		while (todo != NIL)
		{
			ProllyHash *id = llast(todo);
			PlanCommit *pc;

			todo = list_delete_last(todo);
			CHECK_FOR_INTERRUPTS();
			pc = plan_commit(plan, id);
			plan->severed = lappend(plan->severed, pc);
			for (int i = 0; i < pc->e.nparents; i++)
			{
				ProllyHash *p = &pc->e.parents[i];

				if (set_has(fragment, p))
					continue;
				if (set_has(plan->archived, p) || plan_commit(plan, p)->kept)
				{
					if (set_add(stops, p))
						plan->stops = lappend(plan->stops, p);
					continue;
				}
				(void) set_add(fragment, p);
				todo = lappend(todo, p);
			}
		}
		hash_destroy(stops);
	}
	hash_destroy(fragment);
}

static void
archive_plan(const TimestampTz *before, ProllyWindowItemVisit visit, void *arg)
{
	Plan		plan;
	ListCell   *lc;

	window_init();
	if (before == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("a history window needs a date to cut before")));
	PROLLY_CALL(dc.refs, lock);
	make_plan(&plan, *before);
	foreach(lc, plan.severed)
		visit(PROLLY_WINDOW_SEVERED, &((PlanCommit *) lfirst(lc))->id, NULL,
			  NULL, arg);
	foreach(lc, plan.edges)
	{
		ProllyHash *pair = lfirst(lc);

		visit(PROLLY_WINDOW_EDGE, &pair[0], &pair[1], NULL, arg);
	}
	foreach(lc, plan.kept)
	{
		PlanCommit *pc = lfirst(lc);

		if (pc->pinned || pc->e.committed_at < plan.cut)
			visit(PROLLY_WINDOW_PINNED, &pc->id, NULL,
				  pc->pinned ? "ref" : "merge base", arg);
	}
}

/* ------------------------------------------------------------------------
 * Fragments
 * ------------------------------------------------------------------------
 */

typedef struct Closure
{
	HTAB	   *seen;
	List	   *chunks;			/* ProllyHash * */
	int64		bytes;
	StringInfoData buf;
} Closure;

static bool
closure_visit(const ProllyNodeRef *node, uint16 level, void *arg)
{
	Closure    *c = arg;

	if (!set_add(c->seen, &node->hash))
		return false;
	c->chunks = lappend(c->chunks, memcpy(palloc(sizeof(ProllyHash)),
										  &node->hash, sizeof(ProllyHash)));
	resetStringInfo(&c->buf);
	if (PROLLY_CALL(tree_env.chunk_store, get, tree_env.chunks, &node->hash,
					&c->buf))
		c->bytes += c->buf.len;
	return true;
}

static int
hash_cmp(const ListCell *a, const ListCell *b)
{
	const ProllyHash *x = lfirst(a);
	const ProllyHash *y = lfirst(b);

	if (x->len != y->len)
		return x->len < y->len ? -1 : 1;
	return memcmp(x->bytes, y->bytes, x->len);
}

/*
 * What a fragment's commits reach, without crossing to other commits:
 * each one's own tree, its root's (the canonicalizer's walk) and its
 * dependencies'.  The digest is a hash over their hashes, sorted.
 */
static void
fragment_closure(List *commits, ProllyHash *digest, int64 *chunks,
				 int64 *bytes)
{
	Closure		c = {0};
	ListCell   *lc;
	ProllyHashCtx *ctx;

	c.seen = id_set("prolly fragment chunks", sizeof(ProllyHash));
	initStringInfo(&c.buf);
	foreach(lc, commits)
	{
		ProllyHash *id = lfirst(lc);
		ProllyCommitInfo info;
		ProllyNodeRef ref;

		CHECK_FOR_INTERRUPTS();
		if (!PROLLY_CALL(dc.refs, commit_get, id, &info))
		{
			prolly_window_check_missing(id);
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("commit %s is missing from the chunk store",
							prolly_hash_hex(id))));
		}
		ref.hash = *id;
		if (!PROLLY_CALL(tree, walk, &tree_env, &ref, closure_visit, &c))
			continue;
		PROLLY_CALL(dc.canonicalizer, walk, &info.root, closure_visit, &c);
		if (info.has_dependencies)
			(void) PROLLY_CALL(tree, walk, &tree_env, &info.dependencies,
							   closure_visit, &c);
	}
	list_sort(c.chunks, hash_cmp);
	ctx = PROLLY_CALL(tree_env.hash, begin, CurrentMemoryContext);
	foreach(lc, c.chunks)
	{
		ProllyHash *h = lfirst(lc);

		PROLLY_CALL(tree_env.hash, update, ctx, &h->len, 1);
		PROLLY_CALL(tree_env.hash, update, ctx, h->bytes, h->len);
	}
	PROLLY_CALL(tree_env.hash, finish, ctx, digest);
	*chunks = list_length(c.chunks);
	*bytes = c.bytes;
	pfree(c.buf.data);
	hash_destroy(c.seen);
}

static int
entry_cmp(const ListCell *a, const ListCell *b)
{
	const PlanCommit *x = lfirst(a);
	const PlanCommit *y = lfirst(b);

	if (x->e.committed_at != y->e.committed_at)
		return x->e.committed_at < y->e.committed_at ? -1 : 1;
	if (x->id.len != y->id.len)
		return x->id.len < y->id.len ? -1 : 1;
	return memcmp(x->id.bytes, y->id.bytes, x->id.len);
}

/*
 * A fragment's identity: the root of a tree of its commits' index entries
 * (keyed by date, then id; each its height and parents), as any tree of
 * those entries is built.
 */
static void
fragment_root(List *severed, ProllyHash *root)
{
	ProllyTreeBuilder *b;
	ProllyNodeRef ref;
	StringInfoData k;
	StringInfoData v;
	List	   *sorted = list_copy(severed);
	ListCell   *lc;

	list_sort(sorted, entry_cmp);
	initStringInfo(&k);
	initStringInfo(&v);
	b = PROLLY_CALL(tree, build_begin, &tree_env, CurrentMemoryContext);
	foreach(lc, sorted)
	{
		PlanCommit *pc = lfirst(lc);
		ProllyKey	key;
		ProllyValue value;

		resetStringInfo(&k);
		resetStringInfo(&v);
		put_be64(&k, ((uint64) pc->e.committed_at) ^ ((uint64) 1 << 63));
		put_id(&k, &pc->id);
		put_be64(&v, (uint64) pc->e.height);
		appendStringInfoChar(&v, (char) pc->e.nparents);
		for (int i = 0; i < pc->e.nparents; i++)
			put_id(&v, &pc->e.parents[i]);
		key.data = (const uint8 *) k.data;
		key.len = k.len;
		value.data = (const uint8 *) v.data;
		value.len = v.len;
		PROLLY_CALL(tree, build_add, b, &key, &value);
	}
	PROLLY_CALL(tree, build_finish, b, &ref);
	*root = ref.hash;
	pfree(k.data);
	pfree(v.data);
	list_free(sorted);
}

static int64
next_number(void)
{
	char		n = KEY_NEXT;
	ProllyKey	key = {(const uint8 *) &n, 1};
	StringInfoData v;
	int64		next = 1;

	initStringInfo(&v);
	if (PROLLY_CALL(dc.refs, side_get, SPACE_HORIZON, &key, &v))
	{
		if (v.len != 8)
			horizon_corrupt();
		next = (int64) get_be64(v.data);
	}
	resetStringInfo(&v);
	put_be64(&v, (uint64) (next + 1));
	{
		ProllyValue value = {(const uint8 *) v.data, v.len};

		PROLLY_CALL(dc.refs, side_put, SPACE_HORIZON, &key, &value);
	}
	pfree(v.data);
	return next;
}

static void
check_may_sever(void)
{
	if (!object_ownercheck(DatabaseRelationId, MyDatabaseId, GetUserId()))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to sever history of database \"%s\"",
						get_database_name(MyDatabaseId)),
				 errdetail("Only the database's owner or a superuser may.")));
}

static void
archive_sever(ProllyWindowAction action, const TimestampTz *before,
			  ProllyWindowResult *result)
{
	Plan		plan;
	ListCell   *lc;
	List	   *ids = NIL;
	ProllyWindowFragment f;
	ProllyHash *tops;
	ProllyHash *stops;
	int			ntops;
	int			nstops;
	int			i;
	StringInfoData k;
	StringInfoData v;

	window_init();
	memset(result, 0, sizeof(*result));
	result->action = action;
	if (action == PROLLY_WINDOW_NONE)
		return;
	if (action != PROLLY_WINDOW_DELETE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("history can only be severed by deleting it so far")));
	if (before == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("a history window needs a date to cut before")));
	check_may_sever();
	PreventCommandIfReadOnly("severing history");
	horizon_lock(ExclusiveLock);
	PROLLY_CALL(dc.refs, lock);
	make_plan(&plan, *before);
	if (plan.severed == NIL)
		return;

	memset(&f, 0, sizeof(f));
	f.deleted = (action == PROLLY_WINDOW_DELETE);
	f.severed_at = GetCurrentTimestamp();
	f.oldest = DT_NOEND;
	f.newest = DT_NOBEGIN;
	foreach(lc, plan.severed)
	{
		PlanCommit *pc = lfirst(lc);

		ids = lappend(ids, &pc->id);
		f.oldest = Min(f.oldest, pc->e.committed_at);
		f.newest = Max(f.newest, pc->e.committed_at);
	}
	f.commits = list_length(plan.severed);
	fragment_root(plan.severed, &f.root);
	fragment_closure(ids, &f.digest, &f.chunks, &f.bytes);

	/* its tops: each parent the boundary keeps no more, once */
	tops = palloc_array(ProllyHash, Max(list_length(plan.edges), 1));
	ntops = 0;
	{
		HTAB	   *seen = id_set("prolly fragment tops", sizeof(ProllyHash));

		foreach(lc, plan.edges)
		{
			ProllyHash *pair = lfirst(lc);

			if (set_add(seen, &pair[1]))
				tops[ntops++] = pair[1];
		}
		hash_destroy(seen);
	}
	f.ntops = ntops;
	f.tops = tops;
	nstops = list_length(plan.stops);
	stops = palloc_array(ProllyHash, Max(nstops, 1));
	i = 0;
	foreach(lc, plan.stops)
		stops[i++] = *(ProllyHash *) lfirst(lc);

	f.number = next_number();
	initStringInfo(&k);
	initStringInfo(&v);
	number_key(&k, KEY_FRAGMENT, f.number);
	fragment_encode(&v, &f,
					PROLLY_HAS(tree_env.chunk_store, generation) ?
					PROLLY_CALL(tree_env.chunk_store, generation,
								tree_env.chunks) : 0,
					nstops, stops);
	{
		ProllyKey	key = {(const uint8 *) k.data, k.len};
		ProllyValue value = {(const uint8 *) v.data, v.len};

		PROLLY_CALL(dc.refs, side_put, SPACE_HORIZON, &key, &value);
	}
	foreach(lc, plan.edges)
	{
		ProllyHash *pair = lfirst(lc);
		ProllyKey	key;
		ProllyValue value;

		resetStringInfo(&k);
		resetStringInfo(&v);
		appendStringInfoChar(&k, KEY_EDGE);
		put_id(&k, &pair[1]);
		put_id(&k, &pair[0]);
		put_be64(&v, (uint64) f.number);
		key.data = (const uint8 *) k.data;
		key.len = k.len;
		value.data = (const uint8 *) v.data;
		value.len = v.len;
		PROLLY_CALL(dc.refs, side_put, SPACE_HORIZON, &key, &value);
	}

	horizon_changed();

	result->commits = f.commits;
	result->edges = list_length(plan.edges);
	result->chunks = f.chunks;
	result->bytes = f.bytes;
	result->fragment = f.number;
	{
		HTAB	   *seen = id_set("prolly boundary", sizeof(ProllyHash));

		result->boundary = palloc_array(ProllyHash,
										Max(list_length(plan.edges), 1));
		foreach(lc, plan.edges)
		{
			ProllyHash *pair = lfirst(lc);

			if (set_add(seen, &pair[0]))
				result->boundary[result->nboundary++] = pair[0];
		}
		hash_destroy(seen);
	}
	{
		StringInfoData boundary;

		initStringInfo(&boundary);
		foreach(lc, plan.edges)
		{
			ProllyHash *pair = lfirst(lc);

			appendStringInfo(&boundary, "%s%s", boundary.len > 0 ? ", " : "",
							 prolly_hash_hex(&pair[0]));
		}
		ereport(LOG,
				(errmsg("history of database \"%s\" severed (%s): %lld commits dated %s to %s, fragment %lld",
						get_database_name(MyDatabaseId),
						f.deleted ? "deleted" : "archived",
						(long long) f.commits, timestamptz_to_str(f.oldest),
						timestamptz_to_str(f.newest), (long long) f.number),
				 errdetail("The boundary is %s.  The fragment's commits reach %lld chunks (%lld bytes), those no kept commit reaches freed by the next collection.",
						   boundary.data, (long long) f.chunks,
						   (long long) f.bytes)));
		pfree(boundary.data);
	}
	pfree(k.data);
	pfree(v.data);
}

/* ------------------------------------------------------------------------
 * Readers at the horizon
 * ------------------------------------------------------------------------
 */

bool
prolly_window_archived(const ProllyHash *id)
{
	ProllyDatabaseComponents c;

	prolly_database_components(&c);
	if (c.history_window == NULL || !PROLLY_HAS(c.history_window, archived))
		return false;
	return PROLLY_CALL(c.history_window, archived, id);
}

void
prolly_window_check_missing(const ProllyHash *id)
{
	if (prolly_window_archived(id))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("commit %s is past the history horizon",
						prolly_hash_hex(id)),
				 errdetail("History before it was severed from this database: archived, or deleted."),
				 errhint("Run dolt_history_repair() to reattach it.")));
}

bool
prolly_window_stops_at(const ProllyHash *parent)
{
	static LocalTransactionId told = InvalidLocalTransactionId;

	if (!prolly_window_archived(parent))
		return false;
	if (told != MyProc->vxid.lxid)
	{
		told = MyProc->vxid.lxid;
		ereport(NOTICE,
				(errmsg("history stops at the history horizon"),
				 errdetail("Commit %s and the history before it were severed from this database.",
						   prolly_hash_hex(parent)),
				 errhint("Run dolt_history_repair() to reattach it.")));
	}
	return true;
}

const ProllyWindowRoutine prolly_window_archive = {
	.info = {
		.iface = PROLLY_IFACE_HISTORY_WINDOW,
		.impl_id = 1,
		.format_version = 1,
		.name = "archive",
		.description = "archive: history past a window severed at a horizon in the refs",
	},
	.plan = archive_plan,
	.sever = archive_sever,
	.archived = archive_archived,
	.edges = archive_edges,
	.fragments = archive_fragments,
};
