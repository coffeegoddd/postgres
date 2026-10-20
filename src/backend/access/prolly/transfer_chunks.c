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
 * Chunks move as stored (prolly_chunk_frame()): sent as the store holds
 * them, compressed where they were made, and added as received; only the
 * receiver's checks decode them.
 *
 * Receiving.  The walk reads through this database's store: a chunk the
 * store hasn't is asked of the remote (the store's read_through(), which
 * answers has() from the remote too), and kept, as received, in a
 * temporary file.  Each is decoded, re-hashed and compared
 * with the address it was asked by before anything reads it, so what the
 * walk follows is what the addresses say; a chunk the remote lacks is an
 * error, so every reference resolves, in the store or in what was
 * received.  Then each commit received is read, and its root checked to be
 * of this build's format.  Only then is what was received added to the
 * store, as one table file: nothing is found before the whole is checked,
 * and an error before leaves the store as it was.
 *
 * Cloning.  A database with nothing to walk against copies the remote's
 * files whole instead (as Dolt's clone does), a file at a time, each chunk
 * re-hashed as it comes; each kept whole, out of what the store reads, so
 * a clone that fails and is tried again copies only the files it lacks.
 * Then every reference in them is resolved, in them or the store, and each
 * commit the heads reach checked to be of this build's format, before the
 * store takes them all at once.  A remote that can't list its files, or
 * whose journal (which grows as it is read) is a fifth of it or more, is
 * walked instead.  A clone brings the remote's garbage too, until the
 * store's next collection.
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
#include "access/prolly_seen.h"
#include "catalog/pg_database.h"
#include "miscadmin.h"
#include "utils/injection_point.h"
#include "storage/buffile.h"
#include "storage/lmgr.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"

/*
 * A chunk received: where its bytes are in the spool.  The spool holds
 * each chunk received after a header (its hash, as a key, its length, and
 * whether the walk visited it, set in place), in the order received.
 */
typedef struct Received
{
	uint64		offset;
	uint32		len;
} Received;

typedef struct SpoolHeader
{
	ProllyHash	hash;
	uint32		len;
	bool		visited;
} SpoolHeader;

/*
 * The budget of each of a transfer's maps: those that live at once share
 * maintenance_work_mem.
 */
#define TRANSFER_BUDGET(n)	((Size) maintenance_work_mem * 1024 / (n))

typedef struct Receiving
{
	const ProllyRemoteRoutine *rr;
	ProllyRemote *remote;
	const ProllyChunkStoreRoutine *cs;
	ProllyChunkStore *store;
	const ProllyHashRoutine *hash;
	ProllySeen *received;		/* Received, by hash */
	BufFile    *spool;
	uint64		spooled;		/* the spool's length */
	uint64		next;			/* the table file's: the next header */
	uint64		visited;		/* chunks the walk visited */
	bool		hinting;		/* receive_hint() asking the store */
} Receiving;

/* A hash, zero-padded, as a hash table's key. */
static void
hash_key(ProllyHash *key, const ProllyHash *hash)
{
	memset(key, 0, sizeof(*key));
	key->len = hash->len;
	memcpy(key->bytes, hash->bytes, hash->len);
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
 * A chunk the remote gave, as stored (frame), decoded into chunk and
 * checked against the address it was asked by.
 */
static void
check_chunk(Receiving *rv, const ProllyHash *hash, const char *frame, int len,
			StringInfo chunk)
{
	ProllyHashCtx *ctx;
	ProllyHash	actual;

	if (!prolly_chunk_unframe(frame, len, chunk))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("the remote's chunk %s isn't framed as stored chunks are",
						prolly_hash_hex(hash))));
	ctx = PROLLY_CALL(rv->hash, begin, CurrentMemoryContext);
	PROLLY_CALL(rv->hash, update, ctx, (const uint8 *) chunk->data, chunk->len);
	PROLLY_CALL(rv->hash, finish, ctx, &actual);
	if (actual.len != hash->len ||
		memcmp(actual.bytes, hash->bytes, hash->len) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("the remote's chunk %s doesn't match its hash",
						prolly_hash_hex(hash)),
				 errdetail("Its bytes hash to %s.", prolly_hash_hex(&actual))));
}

/*
 * Keep a chunk the remote gave, as stored, checked against the address it
 * was asked by first: what the walk follows is what the addresses say.
 * If chunk is given, the chunk decoded is appended to it.
 */
static void
keep_chunk(Receiving *rv, const ProllyHash *hash, const char *data, int len,
		   StringInfo chunk)
{
	Received	r;
	SpoolHeader head;
	StringInfoData decoded;

	initStringInfo(&decoded);
	check_chunk(rv, hash, data, len, &decoded);
	if (chunk)
		appendBinaryStringInfo(chunk, decoded.data, decoded.len);
	pfree(decoded.data);

	memset(&head, 0, sizeof(head));
	hash_key(&head.hash, hash);
	head.len = len;
	r.offset = rv->spooled + sizeof(head);
	r.len = len;
	/* (entered first: one lookup, and nothing spooled for one kept before) */
	if (!prolly_seen_enter(rv->received, hash, &r))
		return;

	/* (at the end: a read since may have left the file elsewhere) */
	if (BufFileSeek(rv->spool, 0, rv->spooled, SEEK_SET) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not seek in a transfer's temporary file: %m")));
	BufFileWrite(rv->spool, &head, sizeof(head));
	BufFileWrite(rv->spool, data, len);
	rv->spooled += sizeof(head) + len;
}

/*
 * The store's read_through() source: a chunk received already, or asked of
 * the remote, checked against its address and kept.  A chunk the remote
 * lacks is an error, so every reference resolves, in the store or in what
 * was received.
 */
static bool
receive_chunk(void *arg, const ProllyHash *hash, StringInfo out)
{
	Receiving  *rv = arg;
	Received	r;
	StringInfoData data;

	if (prolly_seen_find(rv->received, hash, &r))
	{
		if (out)
		{
			StringInfoData frame;

			initStringInfo(&frame);
			spool_read(rv, &r, &frame);
			if (!prolly_chunk_unframe(frame.data, frame.len, out))
				elog(ERROR, "a received chunk %s was spooled unframed",
					 prolly_hash_hex(hash));
			pfree(frame.data);
		}
		return true;
	}
	if (out == NULL)
		return !rv->hinting && PROLLY_CALL(rv->rr, has, rv->remote, hash);
	initStringInfo(&data);
	if (!PROLLY_CALL(rv->rr, get, rv->remote, hash, &data))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("the remote lacks chunk %s, which it references",
						prolly_hash_hex(hash))));
	keep_chunk(rv, hash, data.data, data.len, out);
	pfree(data.data);
	return true;
}

/* A batch asked of the remote: what it may give. */
typedef struct Asked
{
	Receiving  *rv;
	HTAB	   *hashes;
} Asked;

/* (a chunk not asked for isn't taken: nothing says what it references is) */
static void
receive_many_visit(const ProllyHash *hash, const char *data, int len, void *arg)
{
	Asked	   *asked = arg;
	ProllyHash	key;

	hash_key(&key, hash);
	if (hash_search(asked->hashes, &key, HASH_FIND, NULL) != NULL)
		keep_chunk(asked->rv, hash, data, len, NULL);
}

/*
 * The store's hint: chunks the walk reads next, those neither the store
 * nor the transfer has asked of the remote at once.  (One the remote
 * lacks is found when the walk reads it.)
 */
static void
receive_hint(void *arg, const ProllyHash *hashes, int n)
{
	Receiving  *rv = arg;
	ProllyHash *ask;
	int			nask = 0;

	if (!PROLLY_HAS(rv->rr, get_many))
		return;
	ask = palloc(sizeof(ProllyHash) * n);
	/* (the store's own has(), not the remote's through it) */
	rv->hinting = true;
	PG_TRY();
	{
		for (int i = 0; i < n; i++)
		{
			if (!prolly_seen_find(rv->received, &hashes[i], NULL) &&
				!PROLLY_CALL(rv->cs, has, rv->store, &hashes[i]))
				ask[nask++] = hashes[i];
		}
	}
	PG_FINALLY();
	{
		rv->hinting = false;
	}
	PG_END_TRY();
	if (nask > 0)
	{
		HASHCTL		ctl;
		Asked		asked = {rv, NULL};

		ctl.keysize = sizeof(ProllyHash);
		ctl.entrysize = sizeof(ProllyHash);
		ctl.hcxt = CurrentMemoryContext;
		asked.hashes = hash_create("prolly chunks asked", nask, &ctl,
								   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
		for (int i = 0; i < nask; i++)
		{
			ProllyHash	key;

			hash_key(&key, &ask[i]);
			(void) hash_search(asked.hashes, &key, HASH_ENTER, NULL);
		}
		PROLLY_CALL(rv->rr, get_many, rv->remote, ask, nask, receive_many_visit, &asked);
		hash_destroy(asked.hashes);
	}
	pfree(ask);
}

/* The walk's visitor: decline what the store has, or was received. */
static bool
receive_visit(const ProllyNodeRef *node, uint16 level, void *arg)
{
	Receiving  *rv = arg;
	Received	r;
	SpoolHeader head;
	off_t		at;

	/* (a chunk is read, so received, before it is visited) */
	if (!prolly_seen_find(rv->received, &node->hash, &r))
		return false;			/* the store's */
	/* (whether it was visited: in its header, set there) */
	at = r.offset - sizeof(SpoolHeader);
	if (BufFileSeek(rv->spool, 0, at, SEEK_SET) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not seek in a transfer's temporary file: %m")));
	BufFileReadExact(rv->spool, &head, sizeof(head));
	if (head.visited)
		return false;			/* seen */
	head.visited = true;
	if (BufFileSeek(rv->spool, 0, at, SEEK_SET) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not seek in a transfer's temporary file: %m")));
	BufFileWrite(rv->spool, &head, sizeof(head));
	rv->visited++;
	return true;
}

/* The table file's source: what was received, in order, as stored. */
static bool
received_next(void *arg, ProllyHash *hash, StringInfo data)
{
	Receiving  *rv = arg;
	SpoolHeader head;
	Received	r;

	/*
	 * (only what the walk reached, so had its references resolved: a chunk
	 * a remote sent that no wanted commit reaches isn't taken)
	 */
	do
	{
		if (rv->next >= rv->spooled)
			return false;
		if (BufFileSeek(rv->spool, 0, rv->next, SEEK_SET) != 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not seek in a transfer's temporary file: %m")));
		BufFileReadExact(rv->spool, &head, sizeof(head));
		r.offset = rv->next + sizeof(head);
		r.len = head.len;
		rv->next += sizeof(head) + head.len;
	} while (!head.visited);
	*hash = head.hash;
	spool_read(rv, &r, data);
	return true;
}

/* What a remote sent since known heads: kept, but not what the store has. */
static void
receive_since_visit(const ProllyHash *hash, const char *data, int len, void *arg)
{
	Receiving  *rv = arg;

	if (!PROLLY_CALL(rv->cs, has, rv->store, hash))
		keep_chunk(rv, hash, data, len, NULL);
}

/* The heads this database has, which a remote can send what changed since. */
typedef struct Known
{
	const ProllyChunkStoreRoutine *cs;
	ProllyChunkStore *store;
	ProllyHash *hashes;
	int			n;
	int			size;
} Known;

static void
add_known(Known *k, const ProllyHash *commit)
{
	if (commit->len == 0 || !PROLLY_CALL(k->cs, has, k->store, commit))
		return;
	if (k->n == k->size)
	{
		k->size *= 2;
		k->hashes = repalloc(k->hashes, sizeof(ProllyHash) * k->size);
	}
	hash_key(&k->hashes[k->n++], commit);
}

static void
known_branch(const char *name, const ProllyHash *head, void *arg)
{
	add_known(arg, head);
}

static void
known_tracking(const char *remote, const char *branch, const ProllyHash *commit,
			   void *arg)
{
	add_known(arg, commit);
}

/* Every commit received from those given: those the store hasn't. */
static void
check_formats(Receiving *rv, const ProllyHash *commits, int ncommits)
{
	ProllyDatabaseComponents dc;
	List	   *queue = NIL;
	ProllySeen *seen;

	prolly_database_components(&dc);
	seen = prolly_seen_create("prolly received commits", 0, TRANSFER_BUDGET(2));
	for (int i = 0; i < ncommits; i++)
	{
		ProllyCommitInfo info;

		/* (each given commit is one: read through what was received) */
		if (!PROLLY_CALL(dc.refs, commit_get, &commits[i], &info))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("the remote's head %s isn't a commit",
							prolly_hash_hex(&commits[i]))));
		queue = lappend(queue, (void *) &commits[i]);
	}
	while (queue != NIL)
	{
		ProllyHash *id = linitial(queue);
		ProllyCommitInfo info;

		queue = list_delete_first(queue);
		if (!prolly_seen_find(rv->received, id, NULL))
			continue;			/* the store's: checked when it came */
		if (!prolly_seen_enter(seen, id, NULL))
			continue;
		if (!PROLLY_CALL(dc.refs, commit_get, id, &info))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("the remote's commit %s can't be read", prolly_hash_hex(id))));
		if (!PROLLY_CALL(dc.canonicalizer, in_format, &info.root))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("the remote's commit %s is of another canonical format",
							prolly_hash_hex(id)),
					 errdetail("A database takes only commits of the format it makes.")));
		for (int i = 0; i < info.nparents; i++)
		{
			ProllyHash *p = palloc(sizeof(ProllyHash));

			*p = info.parents[i];
			queue = lappend(queue, p);
		}
	}
	prolly_seen_destroy(seen);
}

static uint64
chunks_receive(const ProllyRemoteRoutine *rr, ProllyRemote *remote,
			   const ProllyHash *commits, int ncommits)
{
	ProllyDatabaseComponents dc;
	Receiving	rv;
	uint64		count;
	bool	   *walk;

	memset(&rv, 0, sizeof(rv));
	rv.rr = rr;
	rv.remote = remote;
	rv.cs = prolly_chunk_store();
	rv.store = PROLLY_CALL(rv.cs, open, MyDatabaseId, MyDatabaseTableSpace);
	rv.hash = prolly_content_hash();
	rv.received = prolly_seen_create("prolly chunks received", sizeof(Received),
									 TRANSFER_BUDGET(2));
	rv.spool = BufFileCreateTemp(false);
	prolly_database_components(&dc);

	/* (one the store has is taken as checked, and all it reaches) */
	walk = palloc0(sizeof(bool) * Max(ncommits, 1));
	for (int i = 0; i < ncommits; i++)
		walk[i] = !PROLLY_CALL(rv.cs, has, rv.store, &commits[i]);

	/*
	 * A remote that runs this code is told the heads this database has, and
	 * sends what changed since them in one exchange: kept as received, and
	 * checked by the walk below, which asks for what it didn't send.
	 */
	if (PROLLY_HAS(rr, get_since))
	{
		Known		k = {rv.cs, rv.store, palloc(sizeof(ProllyHash) * 16), 0, 16};
		ProllyHash *wanted = palloc(sizeof(ProllyHash) * Max(ncommits, 1));
		int			nwanted = 0;

		PROLLY_CALL(dc.refs, branches, known_branch, &k);
		if (PROLLY_HAS(dc.refs, trackings))
			PROLLY_CALL(dc.refs, trackings, NULL, known_tracking, &k);
		for (int i = 0; i < ncommits; i++)
			if (walk[i])
				wanted[nwanted++] = commits[i];
		if (nwanted > 0)
			PROLLY_CALL(rr, get_since, remote, wanted, nwanted, k.hashes, k.n,
						receive_since_visit, &rv);
	}

	PROLLY_CALL(rv.cs, read_through, rv.store, receive_chunk, receive_hint, &rv);
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
		PROLLY_CALL(rv.cs, read_through, rv.store, NULL, NULL, NULL);
	}
	PG_END_TRY();

	count = rv.visited;
	rv.next = 0;
	prolly_chunk_add_stored(rv.cs, rv.store, received_next, &rv);
	BufFileClose(rv.spool);
	prolly_seen_destroy(rv.received);
	return count;
}

/* Cloning */

typedef struct RemoteFiles
{
	List	   *keys;			/* char * */
	List	   *counts;			/* their chunk counts */
	List	   *sizes;			/* uint64 *: their sizes */
	List	   *journal;		/* int: the journal's (its counts are of
								 * copies, which a copy of it doesn't keep) */
	uint64		table_bytes;
	uint64		journal_bytes;
} RemoteFiles;

static void
note_file(const char *key, uint32 nchunks, uint64 size, bool journal, void *arg)
{
	RemoteFiles *rf = arg;
	uint64	   *sz = palloc(sizeof(uint64));

	*sz = size;
	rf->keys = lappend(rf->keys, pstrdup(key));
	rf->counts = lappend_int(rf->counts, (int) nchunks);
	rf->sizes = lappend(rf->sizes, sz);
	rf->journal = lappend_int(rf->journal, journal);
	if (journal)
		rf->journal_bytes += size;
	else
		rf->table_bytes += size;
}

/*
 * The key a remote's file is received under: its key there, its size, and
 * the first commit cloned (the remote's, as of this clone), so a file kept
 * from a clone of another remote, or of this one before it changed, isn't
 * taken for it.
 */
static char *
received_key(const char *key, uint64 size, const ProllyHash *first)
{
	return psprintf("%s %llu %s", key, (unsigned long long) size,
					first ? prolly_hash_hex(first) : "-");
}

/* A remote file's chunks, as stored, each checked against its hash. */
typedef struct Copying
{
	const ProllyRemoteRoutine *rr;
	ProllyRemoteFile *file;
	const ProllyHashRoutine *hash;
	StringInfoData chunk;
	uint64		count;
} Copying;

static bool
copied_next(void *arg, ProllyHash *hash, StringInfo data)
{
	Copying    *cp = arg;
	ProllyHashCtx *ctx;
	ProllyHash	actual;

	if (!PROLLY_CALL(cp->rr, file_next, cp->file, hash, data))
		return false;
	resetStringInfo(&cp->chunk);
	if (!prolly_chunk_unframe(data->data, data->len, &cp->chunk))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("the remote's chunk %s isn't framed as stored chunks are",
						prolly_hash_hex(hash))));
	ctx = PROLLY_CALL(cp->hash, begin, CurrentMemoryContext);
	PROLLY_CALL(cp->hash, update, ctx, (const uint8 *) cp->chunk.data, cp->chunk.len);
	PROLLY_CALL(cp->hash, finish, ctx, &actual);
	if (actual.len != hash->len || memcmp(actual.bytes, hash->bytes, hash->len) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("the remote's chunk %s doesn't match its hash",
						prolly_hash_hex(hash)),
				 errdetail("Its bytes hash to %s.", prolly_hash_hex(&actual))));
	cp->count++;
	return true;
}

/* What the received files are checked with. */
typedef struct CloneCheck
{
	const ProllyChunkStoreRoutine *cs;
	ProllyChunkStore *store;
	StringInfoData chunk;
	const ProllyHash *from;		/* the chunk whose references are checked */
	uint64		count;
} CloneCheck;

static void
check_reference(const ProllyHash *ref, void *arg)
{
	CloneCheck *cc = arg;

	if (!PROLLY_CALL(cc->cs, received_get, cc->store, ref, NULL) &&
		!PROLLY_CALL(cc->cs, has, cc->store, ref))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("the remote's chunk %s names chunk %s, which it lacks",
						prolly_hash_hex(cc->from), prolly_hash_hex(ref))));
}

static void
check_received(const ProllyHash *hash, const char *frame, Size len, void *arg)
{
	CloneCheck *cc = arg;

	resetStringInfo(&cc->chunk);
	if (!prolly_chunk_unframe(frame, len, &cc->chunk))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("a received chunk %s isn't framed as stored chunks are",
						prolly_hash_hex(hash))));
	cc->from = hash;
	prolly_chunk_references(cc->chunk.data, cc->chunk.len, check_reference, cc);
	cc->count++;
}

/* The store's read_through() source while cloning: the files received. */
static bool
received_source(void *arg, const ProllyHash *hash, StringInfo out)
{
	CloneCheck *cc = arg;

	return PROLLY_CALL(cc->cs, received_get, cc->store, hash, out);
}

/* Every commit the heads reach: one, of this build's format. */
static void
check_cloned_commits(const ProllyHash *commits, int ncommits)
{
	ProllyDatabaseComponents dc;
	List	   *queue = NIL;
	ProllySeen *seen;

	prolly_database_components(&dc);
	seen = prolly_seen_create("prolly cloned commits", 0, 0);
	for (int i = 0; i < ncommits; i++)
		queue = lappend(queue, (void *) &commits[i]);
	while (queue != NIL)
	{
		ProllyHash *id = linitial(queue);
		ProllyCommitInfo info;

		queue = list_delete_first(queue);
		if (!prolly_seen_enter(seen, id, NULL))
			continue;
		if (!PROLLY_CALL(dc.refs, commit_get, id, &info))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("the remote's commit %s isn't in its files",
							prolly_hash_hex(id))));
		if (!PROLLY_CALL(dc.canonicalizer, in_format, &info.root))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("the remote's commit %s is of another canonical format",
							prolly_hash_hex(id)),
					 errdetail("A database takes only commits of the format it makes.")));
		for (int i = 0; i < info.nparents; i++)
		{
			ProllyHash *p = palloc(sizeof(ProllyHash));

			*p = info.parents[i];
			queue = lappend(queue, p);
		}
	}
	prolly_seen_destroy(seen);
}

static bool
chunks_clone(const ProllyRemoteRoutine *rr, ProllyRemote *remote,
			 const ProllyHash *commits, int ncommits, uint64 *count)
{
	const ProllyChunkStoreRoutine *cs = prolly_chunk_store();
	ProllyChunkStore *store;
	RemoteFiles rf = {0};
	CloneCheck	cc;
	const char **keys;
	int			nkeys;

	if (!PROLLY_HAS(rr, files) || !PROLLY_HAS(cs, receive_file) ||
		!PROLLY_CALL(rr, files, remote, note_file, &rf))
		return false;
	/* (a remote that lists no files, but has commits, can't serve them) */
	if (rf.keys == NIL)
		return false;
	/* (a journal a fifth of the remote or more: the walk reads less) */
	if (rf.journal_bytes * 5 >= rf.table_bytes + rf.journal_bytes &&
		rf.journal_bytes > 0)
		return false;
	store = PROLLY_CALL(cs, open, MyDatabaseId, MyDatabaseTableSpace);

	/*
	 * (one clone of the database at a time: another's files, being written
	 * or kept, are this one's to remove or take)
	 */
	LockDatabaseObject(DatabaseRelationId, MyDatabaseId, 1, ExclusiveLock);

	/* the files kept from a clone before that this one doesn't name: gone */
	nkeys = list_length(rf.keys);
	keys = palloc(sizeof(char *) * nkeys);
	for (int i = 0; i < nkeys; i++)
		keys[i] = received_key(list_nth(rf.keys, i),
							   *(uint64 *) list_nth(rf.sizes, i),
							   ncommits > 0 ? &commits[0] : NULL);
	PROLLY_CALL(cs, forget_received, store, keys, nkeys);

	for (int i = 0; i < nkeys; i++)
	{
		uint32		have;
		Copying		cp = {rr, NULL, prolly_content_hash()};

		/*
		 * (one whole from a clone tried before: checked as it came; never
		 * the journal, whose count of copies a copy of it doesn't keep)
		 */
		if (!list_nth_int(rf.journal, i) &&
			PROLLY_CALL(cs, received_file, store, keys[i], &have) &&
			have == (uint32) list_nth_int(rf.counts, i))
			continue;
		initStringInfo(&cp.chunk);
		cp.file = PROLLY_CALL(rr, file_open, remote, list_nth(rf.keys, i));
		PG_TRY();
		{
			PROLLY_CALL(cs, receive_file, store, keys[i], copied_next, &cp);
		}
		PG_FINALLY();
		{
			PROLLY_CALL(rr, file_close, cp.file);
		}
		PG_END_TRY();
		pfree(cp.chunk.data);
		INJECTION_POINT("prolly-clone-file-received", NULL);
	}

	/* every reference resolved, every commit of this format: then taken */
	memset(&cc, 0, sizeof(cc));
	cc.cs = cs;
	cc.store = store;
	initStringInfo(&cc.chunk);
	PROLLY_CALL(cs, received_chunks, store, check_received, &cc);
	PROLLY_CALL(cs, read_through, store, received_source, NULL, &cc);
	PG_TRY();
	{
		check_cloned_commits(commits, ncommits);
	}
	PG_FINALLY();
	{
		PROLLY_CALL(cs, read_through, store, NULL, NULL, NULL);
	}
	PG_END_TRY();
	PROLLY_CALL(cs, adopt_received, store);
	*count = cc.count;
	return true;
}

/* Sending */

typedef struct Sending
{
	const ProllyRemoteRoutine *rr;
	ProllyRemote *remote;
	const ProllyChunkStoreRoutine *cs;
	ProllyChunkStore *store;
	ProllySeen *seen;			/* visited */
	ProllySeen *remote_has;		/* bool: has_many()'s answers */
	/* ProllyHash, to send, in order: the first in memory, past it a file */
	ProllyHash *mem;
	uint64		nmem;
	uint64		capmem;			/* of mem */
	uint64		maxmem;			/* the budget's worth */
	BufFile    *order;
	uint64		norder;
	uint64		next;			/* the next to send's place in order */
} Sending;

/*
 * The store's hint: chunks the walk reads next, asked of the remote at
 * once whether it has them.
 */
static void
send_hint(void *arg, const ProllyHash *hashes, int n)
{
	Sending    *sd = arg;
	ProllyHash *ask = palloc(sizeof(ProllyHash) * n);
	bool	   *has = palloc(sizeof(bool) * n);
	int			nask = 0;

	for (int i = 0; i < n; i++)
	{
		if (!prolly_seen_find(sd->seen, &hashes[i], NULL) &&
			!prolly_seen_find(sd->remote_has, &hashes[i], NULL))
			hash_key(&ask[nask++], &hashes[i]);
	}
	if (nask > 0)
	{
		PROLLY_CALL(sd->rr, has_many, sd->remote, ask, nask, has);
		for (int i = 0; i < nask; i++)
			(void) prolly_seen_enter(sd->remote_has, &ask[i], &has[i]);
	}
	pfree(ask);
	pfree(has);
}

static bool
send_visit(const ProllyNodeRef *node, uint16 level, void *arg)
{
	Sending    *sd = arg;
	ProllyHash	key;
	bool		has;

	if (!prolly_seen_enter(sd->seen, &node->hash, NULL))
		return false;
	if (!prolly_seen_find(sd->remote_has, &node->hash, &has))
		has = PROLLY_CALL(sd->rr, has, sd->remote, &node->hash);
	if (has)
		return false;
	hash_key(&key, &node->hash);
	if (sd->nmem < sd->maxmem && sd->order == NULL)
	{
		if (sd->nmem == sd->capmem)
		{
			sd->capmem = Min(sd->maxmem, Max(sd->capmem * 2, 1024));
			sd->mem = sd->mem == NULL ?
				MemoryContextAllocHuge(CurrentMemoryContext,
									   sizeof(ProllyHash) * sd->capmem) :
				repalloc_huge(sd->mem, sizeof(ProllyHash) * sd->capmem);
		}
		sd->mem[sd->nmem++] = key;
	}
	else
	{
		if (sd->order == NULL)
			sd->order = BufFileCreateTemp(false);
		if (BufFileSeek(sd->order, 0, (sd->norder - sd->nmem) * sizeof(ProllyHash),
						SEEK_SET) != 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not seek in a transfer's temporary file: %m")));
		BufFileWrite(sd->order, &key, sizeof(key));
	}
	sd->norder++;
	return true;
}

static bool
sent_next(void *arg, ProllyHash *hash, StringInfo data)
{
	Sending    *sd = arg;

	if (sd->next >= sd->norder)
		return false;
	if (sd->next < sd->nmem)
		*hash = sd->mem[sd->next];
	else
	{
		if (BufFileSeek(sd->order, 0, (sd->next - sd->nmem) * sizeof(ProllyHash),
						SEEK_SET) != 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not seek in a transfer's temporary file: %m")));
		BufFileReadExact(sd->order, hash, sizeof(ProllyHash));
	}
	sd->next++;
	if (!prolly_chunk_get_stored(sd->cs, sd->store, hash, data))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("chunk %s is missing from the chunk store", prolly_hash_hex(hash))));
	return true;
}

static uint64
chunks_send(const ProllyRemoteRoutine *rr, ProllyRemote *remote,
			const ProllyHash *commits, int ncommits)
{
	ProllyDatabaseComponents dc;
	Sending		sd;

	memset(&sd, 0, sizeof(sd));
	sd.rr = rr;
	sd.remote = remote;
	sd.cs = prolly_chunk_store();
	sd.store = PROLLY_CALL(sd.cs, open, MyDatabaseId, MyDatabaseTableSpace);
	sd.seen = prolly_seen_create("prolly chunks sent", 0, TRANSFER_BUDGET(3));
	sd.remote_has = prolly_seen_create("prolly chunks the remote has",
									   sizeof(bool), TRANSFER_BUDGET(3));
	sd.maxmem = Max(TRANSFER_BUDGET(3) / sizeof(ProllyHash), 1);
	prolly_database_components(&dc);
	if (PROLLY_HAS(rr, has_many))
		PROLLY_CALL(sd.cs, read_through, sd.store, NULL, send_hint, &sd);
	PG_TRY();
	{
		for (int i = 0; i < ncommits; i++)
			PROLLY_CALL(dc.refs, walk_commit, &commits[i], send_visit, &sd);
	}
	PG_FINALLY();
	{
		PROLLY_CALL(sd.cs, read_through, sd.store, NULL, NULL, NULL);
	}
	PG_END_TRY();
	PROLLY_CALL(rr, add_chunks, remote, sent_next, &sd);
	if (sd.order)
		BufFileClose(sd.order);
	if (sd.mem)
		pfree(sd.mem);
	prolly_seen_destroy(sd.seen);
	prolly_seen_destroy(sd.remote_has);
	return sd.norder;
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
	.clone = chunks_clone,
	.send = chunks_send,
};
