/*-------------------------------------------------------------------------
 *
 * remote_serve.c
 *	  This database as another's remote: the functions a network remote
 *	  calls over a connection to it.
 *
 * A fetch asks for the canonical format, the heads (each branch with a
 * commit) and, by hash, which chunks the store has and their bytes.  A
 * push sends the chunks its commit reaches that the store lacks
 * (dolt_serve_receive(), any number of batches), kept for the transaction
 * in a temporary file, then asks for a branch to move
 * (dolt_serve_move_head()): only then is what it sent received, as a fetch
 * receives from a remote (each chunk re-hashed, every reference resolved,
 * the commit of this build's format), and added to the store, and the
 * branch moved as a compare-and-swap.  The caller commits.
 *
 * Fetching takes owning the database or the privileges of
 * pg_prolly_fetch, pushing those of pg_prolly_push: history holds every
 * row a versioned table ever had, whatever its privileges say.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/remote_serve.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_config.h"
#include "access/xact.h"
#include "catalog/pg_authid.h"
#include "catalog/branch.h"
#include "catalog/pg_database.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "lib/pairingheap.h"
#include "miscadmin.h"
#include "storage/buffile.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "utils/tuplestore.h"
#include "varatt.h"

/* A chunk a push sent: where its bytes are in the spool. */
typedef struct SpooledChunk
{
	ProllyHash	hash;			/* the key: zero-padded */
	uint64		offset;
	uint32		len;
} SpooledChunk;

/* What a push has sent in this transaction. */
typedef struct Spool
{
	HTAB	   *chunks;
	BufFile    *file;
	uint64		len;
} Spool;

static Spool *spool;
static bool callback_registered;

/* Fetching takes pg_prolly_fetch's privileges or pushing's; pushing, its own. */
static void
check_serving(bool push)
{
	if (!object_ownercheck(DatabaseRelationId, MyDatabaseId, GetUserId()) &&
		!has_privs_of_role(GetUserId(), ROLE_PG_PROLLY_PUSH) &&
		(push || !has_privs_of_role(GetUserId(), ROLE_PG_PROLLY_FETCH)))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to %s this database",
						push ? "push to" : "fetch from"),
				 push ?
				 errdetail("Only the database's owner and roles with the privileges of the \"pg_prolly_push\" role may.") :
				 errdetail("Only the database's owner and roles with the privileges of the \"pg_prolly_fetch\" or \"pg_prolly_push\" role may.")));
}

static ProllyChunkStore *
store(void)
{
	return PROLLY_CALL(prolly_chunk_store(), open, MyDatabaseId,
					   MyDatabaseTableSpace);
}

/* A hash given as bytea. */
static void
bytea_hash(bytea *b, ProllyHash *h)
{
	int			len = VARSIZE_ANY_EXHDR(b);

	if (len <= 0 || len > PROLLY_HASH_MAX_LEN)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("a chunk's hash can't be %d bytes", len)));
	memset(h, 0, sizeof(*h));
	h->len = len;
	memcpy(h->bytes, VARDATA_ANY(b), len);
}

static bytea *
bytes_bytea(const void *data, int len)
{
	bytea	   *b = palloc(VARHDRSZ + len);

	SET_VARSIZE(b, VARHDRSZ + len);
	memcpy(VARDATA(b), data, len);
	return b;
}

static bytea *
hash_bytea(const ProllyHash *h)
{
	return bytes_bytea(h->bytes, h->len);
}

/* The hashes of a bytea[], none null. */
static ProllyHash *
array_hashes(ArrayType *a, int *n)
{
	Datum	   *elems;
	bool	   *nulls;
	ProllyHash *hashes;

	deconstruct_array(a, BYTEAOID, -1, false, TYPALIGN_INT, &elems, &nulls, n);
	hashes = palloc(sizeof(ProllyHash) * Max(*n, 1));
	for (int i = 0; i < *n; i++)
	{
		if (nulls[i])
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("a chunk's hash can't be null")));
		bytea_hash(DatumGetByteaPP(elems[i]), &hashes[i]);
	}
	return hashes;
}

/* ------------------------------------------------------------------------
 * The spool
 * ------------------------------------------------------------------------
 */

static void
spool_xact_callback(XactEvent event, void *arg)
{
	if (spool == NULL)
		return;
	switch (event)
	{
		case XACT_EVENT_PRE_COMMIT:
		case XACT_EVENT_PARALLEL_PRE_COMMIT:
		case XACT_EVENT_PRE_PREPARE:
			BufFileClose(spool->file);
			spool = NULL;
			break;
		case XACT_EVENT_ABORT:
		case XACT_EVENT_PARALLEL_ABORT:
			/* (its file is the transaction's resources', closed with them) */
			spool = NULL;
			break;
		default:
			break;
	}
}

static Spool *
the_spool(void)
{
	MemoryContext old;
	ResourceOwner owner = CurrentResourceOwner;
	HASHCTL		ctl;

	if (spool)
		return spool;
	if (!callback_registered)
	{
		RegisterXactCallback(spool_xact_callback, NULL);
		callback_registered = true;
	}
	{
		Spool	   *made;

		old = MemoryContextSwitchTo(TopTransactionContext);
		made = palloc0(sizeof(Spool));
		ctl.keysize = sizeof(ProllyHash);
		ctl.entrysize = sizeof(SpooledChunk);
		ctl.hcxt = TopTransactionContext;
		made->chunks = hash_create("prolly pushed chunks", 1024, &ctl,
								   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
		/* (the transaction's, not the call's: it outlives this one) */
		CurrentResourceOwner = TopTransactionResourceOwner;
		PG_TRY();
		{
			made->file = BufFileCreateTemp(false);
		}
		PG_FINALLY();
		{
			CurrentResourceOwner = owner;
		}
		PG_END_TRY();
		MemoryContextSwitchTo(old);
		/* (only once it is whole: a failure above leaves none) */
		spool = made;
	}
	return spool;
}

static SpooledChunk *
spooled(const ProllyHash *hash)
{
	if (spool == NULL)
		return NULL;
	return hash_search(spool->chunks, hash, HASH_FIND, NULL);
}

/*
 * The spool as a remote, for the transfer's receive: it has what the push
 * sent.
 */
static bool
spool_get(ProllyRemote *remote, const ProllyHash *hash, StringInfo out)
{
	SpooledChunk *c = spooled(hash);

	if (c == NULL)
		return false;
	enlargeStringInfo(out, c->len);
	if (BufFileSeek(spool->file, 0, c->offset, SEEK_SET) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not seek in a push's temporary file: %m")));
	BufFileReadExact(spool->file, out->data + out->len, c->len);
	out->len += c->len;
	out->data[out->len] = '\0';
	return true;
}

/*
 * What the push sent: as a remote's has() answers a receive, whether it
 * can give the chunk, which the receive checks when it reads it (a read
 * a commit's has() leads to); nothing is added to the store until all of
 * it is checked.
 */
static bool
spool_has(ProllyRemote *remote, const ProllyHash *hash)
{
	return spooled(hash) != NULL;
}

static const ProllyRemoteRoutine spool_remote = {
	.info = {
		.iface = PROLLY_IFACE_REMOTE,
		.impl_id = 0,
		.format_version = 1,
		.name = "spool",
		.description = "the chunks a push sent this database, not yet received",
	},
	.get = spool_get,
	.has = spool_has,
};

/* ------------------------------------------------------------------------
 * The functions
 * ------------------------------------------------------------------------
 */

/*
 * dolt_serve_format() returns text: the canonical format of the commits,
 * then how this server sends and takes chunks: PROLLY_SERVE_FRAMED (as
 * stored, framed), and PROLLY_SERVE_LZ4 if it can decompress what is sent
 * it.  A client of another build, which reads the format before anything
 * else, refuses a server that doesn't say it frames.
 */
Datum
dolt_serve_format(PG_FUNCTION_ARGS)
{
	ProllyDatabaseComponents dc;
	StringInfoData f;

	check_serving(false);
	prolly_database_components(&dc);
	initStringInfo(&f);
	PROLLY_CALL(dc.canonicalizer, format, &f);
	appendStringInfoString(&f, PROLLY_SERVE_FRAMED);
#ifdef USE_LZ4
	appendStringInfoString(&f, PROLLY_SERVE_LZ4);
#endif
	PG_RETURN_TEXT_P(cstring_to_text(f.data));
}

static void
head_row(const char *name, const ProllyHash *head, void *arg)
{
	ReturnSetInfo *rsinfo = arg;
	Datum		values[2];
	bool		nulls[2] = {0};

	/*
	 * (a branch without commits, a session's at a commit, or a writer's
	 * branch, the database's own mechanism: not served)
	 */
	if (head->len == 0 ||
		strncmp(name, PROLLY_DETACHED_PREFIX, strlen(PROLLY_DETACHED_PREFIX)) == 0 ||
		strncmp(name, PROLLY_WRITER_PREFIX, strlen(PROLLY_WRITER_PREFIX)) == 0)
		return;
	values[0] = CStringGetTextDatum(name);
	values[1] = PointerGetDatum(hash_bytea(head));
	tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
}

/*
 * dolt_serve_heads(OUT branch text, OUT commit_id bytea) returns setof
 * record: each branch with a commit, and its head.
 */
Datum
dolt_serve_heads(PG_FUNCTION_ARGS)
{
	ProllyDatabaseComponents dc;

	check_serving(false);
	prolly_database_components(&dc);
	InitMaterializedSRF(fcinfo, 0);
	PROLLY_CALL(dc.refs, branches, head_row, fcinfo->resultinfo);
	return (Datum) 0;
}

/*
 * dolt_serve_has(hashes bytea[]) returns bool[]: whether the store has
 * each chunk (and so all it references).  A push's spooled chunks aren't
 * had until they are received.
 */
Datum
dolt_serve_has(PG_FUNCTION_ARGS)
{
	ProllyChunkStore *cs = store();
	int			n;
	ProllyHash *hashes;
	Datum	   *has;

	check_serving(false);
	hashes = array_hashes(PG_GETARG_ARRAYTYPE_P(0), &n);
	has = palloc(sizeof(Datum) * Max(n, 1));
	for (int i = 0; i < n; i++)
		has[i] = BoolGetDatum(PROLLY_CALL(prolly_chunk_store(), has, cs, &hashes[i]));
	PG_RETURN_ARRAYTYPE_P(construct_array(has, n, BOOLOID, 1, true, TYPALIGN_CHAR));
}

/*
 * dolt_serve_get(hashes bytea[], OUT hash bytea, OUT chunk bytea) returns
 * setof record: the chunks the store has of those, as stored (framed),
 * each with its hash.
 */
Datum
dolt_serve_get(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	ProllyChunkStore *cs = store();
	int			n;
	ProllyHash *hashes;
	StringInfoData data;

	check_serving(false);
	hashes = array_hashes(PG_GETARG_ARRAYTYPE_P(0), &n);
	InitMaterializedSRF(fcinfo, 0);
	initStringInfo(&data);
	for (int i = 0; i < n; i++)
	{
		Datum		values[2];
		bool		nulls[2] = {0};

		resetStringInfo(&data);
		if (!prolly_chunk_get_stored(prolly_chunk_store(), cs, &hashes[i], &data))
			continue;
		values[0] = PointerGetDatum(hash_bytea(&hashes[i]));
		values[1] = PointerGetDatum(bytes_bytea(data.data, data.len));
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
		CHECK_FOR_INTERRUPTS();
	}
	return (Datum) 0;
}

/*
 * dolt_serve_files(OUT key text, OUT chunks int4, OUT bytes int8, OUT
 * journal bool) returns setof record: the store's table files, which a
 * clone copies whole, and its journal (key "journal", read as one file).
 */
typedef struct ServedFiles
{
	ReturnSetInfo *rsinfo;
	uint64		journal_chunks;
	uint64		journal_bytes;
} ServedFiles;

static void
served_file(const char *name, int nchunks, uint64 size, bool settled, void *arg)
{
	ServedFiles *sf = arg;
	Datum		values[4];
	bool		nulls[4] = {0};

	values[0] = CStringGetTextDatum(name);
	values[1] = Int32GetDatum(nchunks);
	values[2] = Int64GetDatum((int64) size);
	values[3] = BoolGetDatum(false);
	tuplestore_putvalues(sf->rsinfo->setResult, sf->rsinfo->setDesc, values, nulls);
}

static void
served_segment(const char *name, int nchunks, uint64 size, bool settled, void *arg)
{
	ServedFiles *sf = arg;

	sf->journal_chunks += nchunks;
	sf->journal_bytes += size;
}

Datum
dolt_serve_files(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	const ProllyChunkStoreRoutine *csr = prolly_chunk_store();
	ProllyChunkStore *cs = store();
	ServedFiles sf = {rsinfo, 0, 0};

	check_serving(false);
	InitMaterializedSRF(fcinfo, 0);
	if (!PROLLY_HAS(csr, file_chunks))
		return (Datum) 0;
	PROLLY_CALL(csr, table_files, cs, served_file, &sf);
	PROLLY_CALL(csr, segments, cs, true, served_segment, &sf);
	{
		Datum		values[4];
		bool		nulls[4] = {0};

		values[0] = CStringGetTextDatum("journal");
		values[1] = Int32GetDatum((int32) Min(sf.journal_chunks, PG_INT32_MAX));
		values[2] = Int64GetDatum((int64) sf.journal_bytes);
		values[3] = BoolGetDatum(true);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	return (Datum) 0;
}

typedef struct ServedChunks
{
	ReturnSetInfo *rsinfo;
} ServedChunks;

static void
served_chunk(const ProllyHash *hash, const char *frame, Size len, void *arg)
{
	ServedChunks *sc = arg;
	Datum		values[2];
	bool		nulls[2] = {0};

	values[0] = PointerGetDatum(hash_bytea(hash));
	values[1] = PointerGetDatum(bytes_bytea(frame, (int) len));
	tuplestore_putvalues(sc->rsinfo->setResult, sc->rsinfo->setDesc, values, nulls);
	pfree(DatumGetPointer(values[0]));
	pfree(DatumGetPointer(values[1]));
}

/*
 * dolt_serve_file_chunks(key text, OUT hash bytea, OUT chunk bytea) returns
 * setof record: the chunks of the file dolt_serve_files() names key, as
 * stored (framed), in its order.
 */
Datum
dolt_serve_file_chunks(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	char	   *key = text_to_cstring(PG_GETARG_TEXT_PP(0));
	ServedChunks sc = {rsinfo};

	check_serving(false);
	InitMaterializedSRF(fcinfo, 0);
	PROLLY_CALL(prolly_chunk_store(), file_chunks, store(),
				strcmp(key, "journal") == 0 ? NULL : key, served_chunk, &sc);
	return (Datum) 0;
}

/* What dolt_serve_since() sends: each chunk once. */
typedef struct Since
{
	ReturnSetInfo *rsinfo;
	ProllyChunkStore *store;
	HTAB	   *sent;
	StringInfoData data;
} Since;

static bool
send_since(const ProllyNodeRef *node, uint16 level, void *arg)
{
	Since	   *sn = arg;
	ProllyHash	key = prolly_hash_key(&node->hash);
	bool		found;
	Datum		values[2];
	bool		nulls[2] = {0};

	(void) hash_search(sn->sent, &key, HASH_ENTER, &found);
	if (found)
		return false;
	resetStringInfo(&sn->data);
	if (!prolly_chunk_get_stored(prolly_chunk_store(), sn->store, &node->hash,
								 &sn->data))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("chunk %s is missing from the chunk store",
						prolly_hash_hex(&node->hash))));
	values[0] = PointerGetDatum(hash_bytea(&node->hash));
	values[1] = PointerGetDatum(bytes_bytea(sn->data.data, sn->data.len));
	tuplestore_putvalues(sn->rsinfo->setResult, sn->rsinfo->setDesc, values, nulls);
	/* (the tuplestore has its copy, spilled as it grows: not these) */
	pfree(DatumGetPointer(values[0]));
	pfree(DatumGetPointer(values[1]));
	CHECK_FOR_INTERRUPTS();
	return true;
}

/*
 * A commit dolt_serve_since() reached: what the store has of it, its height
 * (the walk's order), what reaches it, from the wanted, the known or both
 * (painted on it, as git's negotiation does), and whether it is waiting in
 * the queue.
 */
#define SINCE_WANTED	0x01
#define SINCE_KNOWN		0x02

typedef struct SinceCommit
{
	ProllyHash	id;				/* (the key) */
	bool		found;			/* the store has it */
	uint8		paint;
	uint8		walked;			/* the paint it was walked with */
	bool		queued;
	int64		height;			/* 0: not found yet */
	TimestampTz committed_at;
	int			nparents;
	ProllyHash	parents[PROLLY_MAX_PARENTS];
	ProllyNodeRef root;
	bool		has_dependencies;
	ProllyNodeRef dependencies;
	pairingheap_node node;
} SinceCommit;

typedef struct SinceWalk
{
	ProllyDatabaseComponents dc;
	HTAB	   *commits;
	HTAB	   *heights;
	pairingheap queue;			/* the highest first */
	int			wanted_queued;	/* queued, painted wanted only */
	MemoryContext scratch;		/* (what a commit's read allocates) */
} SinceWalk;

/*
 * The highest first: each commit is higher than its parents, so it is
 * walked after every commit it is reached from.  Of commits as high, the
 * latest, then the least id, so that what is sent is sent in one order.
 */
static int
since_cmp(const pairingheap_node *a, const pairingheap_node *b, void *arg)
{
	const SinceCommit *x = pairingheap_const_container(SinceCommit, node, a);
	const SinceCommit *y = pairingheap_const_container(SinceCommit, node, b);

	if (x->height != y->height)
		return x->height < y->height ? -1 : 1;
	if (x->committed_at != y->committed_at)
		return x->committed_at < y->committed_at ? -1 : 1;
	if (x->id.len != y->id.len)
		return x->id.len > y->id.len ? -1 : 1;
	return -memcmp(x->id.bytes, y->id.bytes, x->id.len);
}

/* Commit id as reached: read from the store once, its strings not kept. */
static SinceCommit *
since_commit(SinceWalk *sw, const ProllyHash *id)
{
	ProllyHash	key = prolly_hash_key(id);
	SinceCommit *c;
	bool		found;

	c = hash_search(sw->commits, &key, HASH_ENTER, &found);
	if (!found)
	{
		ProllyCommitInfo info;
		MemoryContext old = MemoryContextSwitchTo(sw->scratch);

		c->found = PROLLY_CALL(sw->dc.refs, commit_get, id, &info);
		MemoryContextSwitchTo(old);
		c->paint = c->walked = 0;
		c->queued = false;
		c->height = 0;
		c->committed_at = c->found ? info.committed_at : DT_NOBEGIN;
		c->nparents = c->found ? info.nparents : 0;
		for (int p = 0; p < c->nparents; p++)
			c->parents[p] = info.parents[p];
		c->root = c->found ? info.root : (ProllyNodeRef) {0};
		c->has_dependencies = c->found && info.has_dependencies;
		c->dependencies = c->has_dependencies ? info.dependencies : (ProllyNodeRef) {0};
		MemoryContextReset(sw->scratch);
	}
	return c;
}

/*
 * A commit's parents for its height: read, not kept (most commits a height
 * reaches are never walked, and keep only their heights' entries).
 */
static bool
since_lookup(const ProllyHash *id, ProllyCommitInfo *info, void *arg)
{
	SinceWalk  *sw = arg;
	MemoryContext old = MemoryContextSwitchTo(sw->scratch);
	bool		found = PROLLY_CALL(sw->dc.refs, commit_get, id, info);

	MemoryContextSwitchTo(old);
	MemoryContextReset(sw->scratch);
	info->author = info->message = NULL;
	return found;
}

/*
 * Paint commit id, and queue it if that's new paint on it.  One the store
 * hasn't isn't queued: nothing of it can be sent, nor told of its parents.
 */
static void
since_paint(SinceWalk *sw, const ProllyHash *id, uint8 paint)
{
	SinceCommit *c = since_commit(sw, id);

	if (!c->found || (c->paint | paint) == c->paint)
		return;
	if (c->queued && c->paint == SINCE_WANTED)
		sw->wanted_queued--;
	c->paint |= paint;
	if (!c->queued && (c->paint & ~c->walked) != 0)
	{
		if (c->height == 0)
			c->height = prolly_history_height(&c->id, sw->heights,
											  since_lookup, sw);
		pairingheap_add(&sw->queue, &c->node);
		c->queued = true;
	}
	if (c->queued && c->paint == SINCE_WANTED)
		sw->wanted_queued++;
}

/*
 * dolt_serve_since(wanted bytea[], known bytea[], OUT hash bytea, OUT chunk
 * bytea) returns setof record: what a receiver that has the known commits
 * (and all they reach) lacks of the wanted ones, as best it can be told in
 * one go: the commits the wanted reach and the known don't (by parents),
 * and of each, its own chunks and what its root changed from its first
 * parent's (each tree diffed, equal subtrees skipped).  The trees walked
 * are the new commits' and what they changed, not the history's; the
 * commits beneath them are read for their heights, each keeping only its
 * height's entry for the call (until commits record their heights).  The
 * receiver checks all it is sent, and reads what it still lacks as it
 * would without this.
 *
 * The commits are told apart as git's negotiation tells them: walked from
 * the wanted and the known together, the highest first, each painted with
 * what reaches it and its parents with its paint, until no commit waiting
 * is reached from the wanted only.  A commit is higher than its parents,
 * so each is walked after all it is reached from have painted it: those
 * sent are just the ones the known don't reach, whatever the commits'
 * dates (each is dated when it is made, after its parents, but the walk
 * doesn't rely on that: history fetched from elsewhere was dated by other
 * clocks, and the dates only break ties between commits as high).
 */
Datum
dolt_serve_since(PG_FUNCTION_ARGS)
{
	const ProllyTreeRoutine *tree = prolly_tree_lookup("cow");
	ProllyTreeEnv env;
	Since		sn;
	SinceWalk	sw;
	HASHCTL		ctl;
	int			nwanted;
	int			nknown;
	ProllyHash *wanted;
	ProllyHash *known_hashes;
	MemoryContext per_commit;

	check_serving(false);
	wanted = array_hashes(PG_GETARG_ARRAYTYPE_P(0), &nwanted);
	known_hashes = array_hashes(PG_GETARG_ARRAYTYPE_P(1), &nknown);
	prolly_database_components(&sw.dc);
	prolly_folded_tree_env(&env);
	InitMaterializedSRF(fcinfo, 0);
	sn.rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	sn.store = store();
	initStringInfo(&sn.data);
	ctl.keysize = sizeof(ProllyHash);
	ctl.entrysize = sizeof(ProllyHash);
	ctl.hcxt = CurrentMemoryContext;
	sn.sent = hash_create("prolly chunks sent since", 1024, &ctl,
						  HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	ctl.entrysize = sizeof(SinceCommit);
	sw.commits = hash_create("prolly commits since", 64, &ctl,
							 HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	sw.heights = prolly_history_heights();
	pairingheap_initialize(&sw.queue, since_cmp, NULL);
	sw.wanted_queued = 0;
	sw.scratch = AllocSetContextCreate(CurrentMemoryContext, "prolly since read",
									   ALLOCSET_DEFAULT_SIZES);
	per_commit = AllocSetContextCreate(CurrentMemoryContext, "prolly since commit",
									   ALLOCSET_DEFAULT_SIZES);

	for (int i = 0; i < nknown; i++)
		since_paint(&sw, &known_hashes[i], SINCE_KNOWN);
	for (int i = 0; i < nwanted; i++)
		since_paint(&sw, &wanted[i], SINCE_WANTED);

	while (sw.wanted_queued > 0)
	{
		SinceCommit *c = pairingheap_container(SinceCommit, node,
											   pairingheap_remove_first(&sw.queue));
		uint8		paint = c->paint;
		SinceCommit *parent;
		ProllyNodeRef commit;
		MemoryContext old;

		CHECK_FOR_INTERRUPTS();
		c->queued = false;
		if (paint == SINCE_WANTED)
			sw.wanted_queued--;
		c->walked = paint;
		for (int p = 0; p < c->nparents; p++)
			since_paint(&sw, &c->parents[p], paint);
		if (paint != SINCE_WANTED)
			continue;

		/* (what one commit's walks allocate, gone after it) */
		old = MemoryContextSwitchTo(per_commit);
		memset(&commit, 0, sizeof(commit));
		commit.hash = c->id;
		(void) PROLLY_CALL(tree, walk, &env, &commit, send_since, &sn);
		if (c->has_dependencies)
			(void) PROLLY_CALL(tree, walk, &env, &c->dependencies, send_since, &sn);
		parent = c->nparents > 0 ? since_commit(&sw, &c->parents[0]) : NULL;
		PROLLY_CALL(sw.dc.canonicalizer, walk_changes,
					parent != NULL && parent->found ? &parent->root : NULL,
					&c->root, send_since, &sn);
		MemoryContextSwitchTo(old);
		MemoryContextReset(per_commit);
	}
	return (Datum) 0;
}

/*
 * dolt_serve_receive(hashes bytea[], chunks bytea[]) returns void: chunks
 * a push sends, as stored (framed), kept for dolt_serve_move_head() in the
 * same transaction.
 */
Datum
dolt_serve_receive(PG_FUNCTION_ARGS)
{
	int			n;
	ProllyHash *hashes;
	Datum	   *chunks;
	bool	   *nulls;
	int			nchunks;
	Spool	   *s;

	check_serving(true);
	PreventCommandIfReadOnly("dolt_serve_receive()");
	hashes = array_hashes(PG_GETARG_ARRAYTYPE_P(0), &n);
	deconstruct_array(PG_GETARG_ARRAYTYPE_P(1), BYTEAOID, -1, false, TYPALIGN_INT,
					  &chunks, &nulls, &nchunks);
	if (nchunks != n)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("dolt_serve_receive needs a chunk for each hash")));
	s = the_spool();
	for (int i = 0; i < n; i++)
	{
		bytea	   *chunk;
		SpooledChunk *c;
		bool		found;

		if (nulls[i])
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("a chunk can't be null")));
		if (hash_search(s->chunks, &hashes[i], HASH_FIND, NULL) != NULL)
			continue;
		/* (the bytes first: an entry is only ever for what was written) */
		chunk = DatumGetByteaPP(chunks[i]);
		if (BufFileSeek(s->file, 0, s->len, SEEK_SET) != 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not seek in a push's temporary file: %m")));
		BufFileWrite(s->file, VARDATA_ANY(chunk), VARSIZE_ANY_EXHDR(chunk));
		c = hash_search(s->chunks, &hashes[i], HASH_ENTER, &found);
		c->offset = s->len;
		c->len = VARSIZE_ANY_EXHDR(chunk);
		s->len += c->len;
	}
	PG_RETURN_VOID();
}

/* A chunk the push sent, as stored, decoded and checked against its address. */
static void
check_sent(const ProllyHash *hash)
{
	const ProllyHashRoutine *hr = prolly_content_hash();
	StringInfoData frame;
	StringInfoData data;
	ProllyHashCtx *ctx;
	ProllyHash	actual;

	initStringInfo(&frame);
	initStringInfo(&data);
	if (!spool_get(NULL, hash, &frame))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("the push didn't send commit %s", prolly_hash_hex(hash))));
	if (!prolly_chunk_unframe(frame.data, frame.len, &data))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("the remote's chunk %s isn't framed as stored chunks are",
						prolly_hash_hex(hash))));
	pfree(frame.data);
	ctx = PROLLY_CALL(hr, begin, CurrentMemoryContext);
	PROLLY_CALL(hr, update, ctx, (const uint8 *) data.data, data.len);
	PROLLY_CALL(hr, finish, ctx, &actual);
	if (actual.len != hash->len || memcmp(actual.bytes, hash->bytes, hash->len) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("the remote's chunk %s doesn't match its hash",
						prolly_hash_hex(hash)),
				 errdetail("Its bytes hash to %s.", prolly_hash_hex(&actual))));
	pfree(data.data);
}

/*
 * dolt_serve_move_head(branch text, expected bytea, head bytea, format
 * text, force bool) returns bool: what the push sent received (checked as
 * a fetch checks what it receives, then added to the store), and branch
 * moved from expected (empty: no head) to head (null: deleted), unless it
 * is no longer at expected (false).  A branch with uncommitted changes is
 * refused unless force.
 */
Datum
dolt_serve_move_head(PG_FUNCTION_ARGS)
{
	ProllyDatabaseComponents dc;
	char	   *branch;
	ProllyHash	expected = {0};
	ProllyHash	head;
	bool		has_head = !PG_ARGISNULL(2);
	bool		moved;
	StringInfoData mine;

	check_serving(true);
	PreventCommandIfReadOnly("dolt_serve_move_head()");
	if (PG_ARGISNULL(0) || PG_ARGISNULL(1) || PG_ARGISNULL(3) || PG_ARGISNULL(4))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("only dolt_serve_move_head's head can be null")));
	branch = text_to_cstring(PG_GETARG_TEXT_PP(0));

	/*
	 * (a commit's hidden branch, not served: moved, the sessions detached
	 * on it would see another commit's rows while at theirs)
	 */
	if (strncmp(branch, PROLLY_DETACHED_PREFIX, strlen(PROLLY_DETACHED_PREFIX)) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_RESERVED_NAME),
				 errmsg("branch names beginning \"%s\" are reserved", PROLLY_DETACHED_PREFIX)));
	if (VARSIZE_ANY_EXHDR(PG_GETARG_BYTEA_PP(1)) > 0)
		bytea_hash(PG_GETARG_BYTEA_PP(1), &expected);
	prolly_database_components(&dc);
	/* (a push moving a protected branch integrates: the owner's to make) */
	if (PROLLY_CALL(dc.writers, protected, branch) &&
		!object_ownercheck(DatabaseRelationId, MyDatabaseId, GetUserId()))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("branch \"%s\" is protected", branch),
				 errhint("Only the database's owner pushes to a protected branch.")));
	initStringInfo(&mine);
	PROLLY_CALL(dc.canonicalizer, format, &mine);
	{
		/* (the canonical format, as dolt_serve_format() gives it too) */
		char	   *theirs = text_to_cstring(PG_GETARG_TEXT_PP(3));
		char	   *framed = strstr(theirs, PROLLY_SERVE_FRAMED);

		if (framed != NULL)
			*framed = '\0';
		if (strcmp(theirs, mine.data) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("this database holds commits of canonical format %s, not %s",
							mine.data, theirs)));
	}
	/*
	 * (a branch already elsewhere than expected: false before anything is
	 * received, as the move would say after)
	 */
	{
		ProllyHash	at = {0};
		ProllyBranchId id;

		if (PROLLY_CALL(dc.refs, branch_lookup, branch, &id))
			(void) PROLLY_CALL(dc.refs, resolve, branch, &at);
		if (at.len != expected.len || memcmp(at.bytes, expected.bytes, at.len) != 0)
			PG_RETURN_BOOL(false);
	}
	if (has_head)
	{
		bytea_hash(PG_GETARG_BYTEA_PP(2), &head);
		/* (the head the store lacks: sent, and its bytes its address's) */
		if (!PROLLY_CALL(prolly_chunk_store(), has, store(), &head))
			check_sent(&head);
		(void) PROLLY_CALL(dc.transfer, receive, &spool_remote, NULL, &head, 1);
	}
	/* (the branch's rows, moved on the pusher's behalf) */
	BRANCH_WRITES_HUSHED(moved = PROLLY_CALL(dc.refs, branch_move, branch,
											 &expected,
											 has_head ? &head : NULL,
											 PG_GETARG_BOOL(4)));

	/* (a protected branch deleted takes its protection with it) */
	if (moved && !has_head && PROLLY_CALL(dc.writers, protected, branch))
		PROLLY_CALL(dc.writers, set_protected, branch, PROLLY_PROTECT_DEFAULT);
	PG_RETURN_BOOL(moved);
}
