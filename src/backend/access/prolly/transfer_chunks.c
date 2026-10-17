/*-------------------------------------------------------------------------
 *
 * transfer_chunks.c
 *	  Transfer implementation "chunks": commits moved as the chunks they
 *	  reach that the other side lacks.
 *
 * What to move is found by the refs' walk from each commit (its tree, its
 * root's trees, its ancestors'), declining every chunk the receiver has: a
 * store that has a chunk has all it references, so nothing below one is
 * looked at.  Chunks are found once each.
 *
 * Receiving.  The walk reads through this database's store: a chunk the
 * store hasn't is asked of the remote (the store's read_through(), which
 * answers has() from the remote too), and kept, as received, in a
 * temporary file.  Each is re-hashed and compared
 * with the address it was asked by before anything reads it, so what the
 * walk follows is what the addresses say; a chunk the remote lacks is an
 * error, so every reference resolves, in the store or in what was
 * received.  Then each commit received is read, and its root checked to be
 * of this build's format.  Only then is what was received added to the
 * store, as one table file: nothing is found before the whole is checked,
 * and an error before leaves the store as it was.
 *
 * Sending.  The walk reads this store; a chunk the remote has is declined.
 * What it took is sent at once (the remote's add_chunks()).
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/transfer_chunks.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_builtins.h"
#include "access/prolly_config.h"
#include "miscadmin.h"
#include "storage/buffile.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"

/* a chunk received: where its bytes are in the spool */
typedef struct Received
{
	ProllyHash	hash;			/* the key (hash_key()'s) */
	uint64		offset;
	uint32		len;
	bool		visited;
} Received;

typedef struct Receiving
{
	const ProllyRemoteRoutine *rr;
	ProllyRemote *remote;
	const ProllyChunkStoreRoutine *cs;
	ProllyChunkStore *store;
	const ProllyHashRoutine *hash;
	HTAB	   *received;		/* Received, by hash */
	List	   *order;			/* Received *, as received */
	BufFile    *spool;
	uint64		spooled;		/* the spool's length */
	ListCell   *next;			/* add_table_file()'s */
} Receiving;

/* A hash, zero-padded, as a hash table's key. */
static void
hash_key(ProllyHash *key, const ProllyHash *hash)
{
	memset(key, 0, sizeof(*key));
	key->len = hash->len;
	memcpy(key->bytes, hash->bytes, hash->len);
}

static char *
hash_hex(const ProllyHash *h)
{
	StringInfoData s;

	initStringInfo(&s);
	for (int i = 0; i < h->len; i++)
		appendStringInfo(&s, "%02x", h->bytes[i]);
	return s.data;
}

static void
spool_read(Receiving *rv, const Received *r, StringInfo out)
{
	enlargeStringInfo(out, r->len);
	if (BufFileSeek(rv->spool, 0, r->offset, SEEK_SET) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not seek in a transfer's temporary file: %m")));
	BufFileReadExact(rv->spool, out->data + out->len, r->len);
	out->len += r->len;
	out->data[out->len] = '\0';
}

/*
 * The store's read_through() source: a chunk received already, or asked of
 * the remote, checked against its address and kept.
 */
static bool
receive_chunk(void *arg, const ProllyHash *hash, StringInfo out)
{
	Receiving  *rv = arg;
	ProllyHash	key;
	Received   *r;
	bool		found PG_USED_FOR_ASSERTS_ONLY;
	StringInfoData data;
	ProllyHashCtx *ctx;
	ProllyHash	actual;

	hash_key(&key, hash);
	r = hash_search(rv->received, &key, HASH_FIND, NULL);
	if (r)
	{
		if (out)
			spool_read(rv, r, out);
		return true;
	}
	if (out == NULL)
		return PROLLY_CALL(rv->rr, has, rv->remote, hash);
	initStringInfo(&data);
	if (!PROLLY_CALL(rv->rr, get, rv->remote, hash, &data))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("the remote lacks chunk %s, which it references",
						hash_hex(hash))));
	ctx = PROLLY_CALL(rv->hash, begin, CurrentMemoryContext);
	PROLLY_CALL(rv->hash, update, ctx, (const uint8 *) data.data, data.len);
	PROLLY_CALL(rv->hash, finish, ctx, &actual);
	if (actual.len != hash->len ||
		memcmp(actual.bytes, hash->bytes, hash->len) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("the remote's chunk %s doesn't match its hash",
						hash_hex(hash)),
				 errdetail("Its bytes hash to %s.", hash_hex(&actual))));

	r = hash_search(rv->received, &key, HASH_ENTER, &found);
	Assert(!found);
	r->offset = rv->spooled;
	r->len = data.len;
	r->visited = false;

	/* (at the end: a read since may have left the file elsewhere) */
	if (BufFileSeek(rv->spool, 0, rv->spooled, SEEK_SET) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not seek in a transfer's temporary file: %m")));
	BufFileWrite(rv->spool, data.data, data.len);
	rv->spooled += data.len;
	rv->order = lappend(rv->order, r);
	appendBinaryStringInfo(out, data.data, data.len);
	pfree(data.data);
	return true;
}

/* The walk's visitor: decline what the store has, or was received. */
static bool
receive_visit(const ProllyNodeRef *node, uint16 level, void *arg)
{
	Receiving  *rv = arg;
	ProllyHash	key;
	Received   *r;

	hash_key(&key, &node->hash);
	r = hash_search(rv->received, &key, HASH_FIND, NULL);

	/* (a chunk is read, so received, before it is visited) */
	if (r == NULL || r->visited)
		return false;			/* the store's, or seen */
	r->visited = true;
	return true;
}

/* add_table_file()'s source: what was received, in order. */
static bool
received_next(void *arg, ProllyHash *hash, StringInfo data)
{
	Receiving  *rv = arg;
	Received   *r;

	if (rv->next == NULL)
		return false;
	r = lfirst(rv->next);
	rv->next = lnext(rv->order, rv->next);
	*hash = r->hash;
	spool_read(rv, r, data);
	return true;
}

/* Every commit received from those given: those the store hasn't. */
static void
check_formats(Receiving *rv, const ProllyHash *commits, int ncommits)
{
	ProllyDatabaseComponents dc;
	List	   *queue = NIL;
	HTAB	   *seen;
	HASHCTL		ctl;

	prolly_copies_components(&dc);
	ctl.keysize = sizeof(ProllyHash);
	ctl.entrysize = sizeof(ProllyHash);
	ctl.hcxt = CurrentMemoryContext;
	seen = hash_create("prolly received commits", 64, &ctl,
					   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	for (int i = 0; i < ncommits; i++)
	{
		ProllyCommitInfo info;

		/* (each given commit is one: read through what was received) */
		if (!PROLLY_CALL(dc.refs, commit_get, &commits[i], &info))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("the remote's head %s isn't a commit",
							hash_hex(&commits[i]))));
		queue = lappend(queue, (void *) &commits[i]);
	}
	while (queue != NIL)
	{
		ProllyHash *id = linitial(queue);
		ProllyHash	key;
		bool		found;
		ProllyCommitInfo info;

		queue = list_delete_first(queue);
		hash_key(&key, id);
		if (hash_search(rv->received, &key, HASH_FIND, NULL) == NULL)
			continue;			/* the store's: checked when it came */
		(void) hash_search(seen, &key, HASH_ENTER, &found);
		if (found)
			continue;
		if (!PROLLY_CALL(dc.refs, commit_get, id, &info))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("the remote's commit %s can't be read", hash_hex(id))));
		if (!PROLLY_CALL(dc.canonicalizer, in_format, &info.root))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("the remote's commit %s is of another canonical format",
							hash_hex(id)),
					 errdetail("A database takes only commits of the format it makes.")));
		for (int i = 0; i < info.nparents; i++)
		{
			ProllyHash *p = palloc(sizeof(ProllyHash));

			*p = info.parents[i];
			queue = lappend(queue, p);
		}
	}
	hash_destroy(seen);
}

static uint64
chunks_receive(const ProllyRemoteRoutine *rr, ProllyRemote *remote,
			   const ProllyHash *commits, int ncommits)
{
	ProllyDatabaseComponents dc;
	Receiving	rv;
	HASHCTL		ctl;
	uint64		count;
	bool	   *walk;

	memset(&rv, 0, sizeof(rv));
	rv.rr = rr;
	rv.remote = remote;
	rv.cs = prolly_chunk_store();
	rv.store = PROLLY_CALL(rv.cs, open, MyDatabaseId, MyDatabaseTableSpace);
	rv.hash = prolly_hash_lookup("sha256");
	ctl.keysize = sizeof(ProllyHash);
	ctl.entrysize = sizeof(Received);
	ctl.hcxt = CurrentMemoryContext;
	rv.received = hash_create("prolly chunks received", 1024, &ctl,
							  HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	rv.spool = BufFileCreateTemp(false);
	prolly_copies_components(&dc);

	/* (one the store has is taken as checked, and all it reaches) */
	walk = palloc0(sizeof(bool) * Max(ncommits, 1));
	for (int i = 0; i < ncommits; i++)
		walk[i] = !PROLLY_CALL(rv.cs, has, rv.store, &commits[i]);

	PROLLY_CALL(rv.cs, read_through, rv.store, receive_chunk, &rv);
	PG_TRY();
	{
		for (int i = 0; i < ncommits; i++)
		{
			if (walk[i])
				PROLLY_CALL(dc.refs, walk_commit, &commits[i], receive_visit, &rv);
		}
		check_formats(&rv, commits, ncommits);
	}
	PG_FINALLY();
	{
		PROLLY_CALL(rv.cs, read_through, rv.store, NULL, NULL);
	}
	PG_END_TRY();

	count = list_length(rv.order);
	rv.next = list_head(rv.order);
	PROLLY_CALL(rv.cs, add_table_file, rv.store, received_next, &rv);
	BufFileClose(rv.spool);
	hash_destroy(rv.received);
	return count;
}

/* Sending */

typedef struct Sending
{
	const ProllyRemoteRoutine *rr;
	ProllyRemote *remote;
	const ProllyChunkStoreRoutine *cs;
	ProllyChunkStore *store;
	HTAB	   *seen;			/* ProllyHash: visited */
	List	   *order;			/* ProllyHash *, to send */
	ListCell   *next;
} Sending;

static bool
send_visit(const ProllyNodeRef *node, uint16 level, void *arg)
{
	Sending    *sd = arg;
	ProllyHash	key;
	bool		found;
	ProllyHash *h;

	hash_key(&key, &node->hash);
	h = hash_search(sd->seen, &key, HASH_ENTER, &found);
	if (found || PROLLY_CALL(sd->rr, has, sd->remote, &node->hash))
		return false;
	sd->order = lappend(sd->order, h);
	return true;
}

static bool
sent_next(void *arg, ProllyHash *hash, StringInfo data)
{
	Sending    *sd = arg;
	ProllyHash *h;

	if (sd->next == NULL)
		return false;
	h = lfirst(sd->next);
	sd->next = lnext(sd->order, sd->next);
	*hash = *h;
	if (!PROLLY_CALL(sd->cs, get, sd->store, h, data))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("chunk %s is missing from the chunk store", hash_hex(h))));
	return true;
}

static uint64
chunks_send(const ProllyRemoteRoutine *rr, ProllyRemote *remote,
			const ProllyHash *commits, int ncommits)
{
	ProllyDatabaseComponents dc;
	Sending		sd;
	HASHCTL		ctl;

	memset(&sd, 0, sizeof(sd));
	sd.rr = rr;
	sd.remote = remote;
	sd.cs = prolly_chunk_store();
	sd.store = PROLLY_CALL(sd.cs, open, MyDatabaseId, MyDatabaseTableSpace);
	ctl.keysize = sizeof(ProllyHash);
	ctl.entrysize = sizeof(ProllyHash);
	ctl.hcxt = CurrentMemoryContext;
	sd.seen = hash_create("prolly chunks sent", 1024, &ctl,
						  HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	prolly_copies_components(&dc);
	for (int i = 0; i < ncommits; i++)
		PROLLY_CALL(dc.refs, walk_commit, &commits[i], send_visit, &sd);
	sd.next = list_head(sd.order);
	PROLLY_CALL(rr, add_chunks, remote, sent_next, &sd);
	hash_destroy(sd.seen);
	return list_length(sd.order);
}

const ProllyTransferRoutine prolly_transfer_chunks = {
	.info = {
		.iface = PROLLY_IFACE_TRANSFER,
		.impl_id = 1,
		.format_version = 1,
		.name = "chunks",
		.description = "commits moved as the chunks they reach that the "
		"other side lacks, checked on receipt",
	},
	.receive = chunks_receive,
	.send = chunks_send,
};
