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
#include "catalog/pg_database.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
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
	old = MemoryContextSwitchTo(TopTransactionContext);
	spool = palloc0(sizeof(Spool));
	ctl.keysize = sizeof(ProllyHash);
	ctl.entrysize = sizeof(SpooledChunk);
	ctl.hcxt = TopTransactionContext;
	spool->chunks = hash_create("prolly pushed chunks", 1024, &ctl,
								HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	/* (the transaction's, not the call's: it outlives this one) */
	CurrentResourceOwner = TopTransactionResourceOwner;
	spool->file = BufFileCreateTemp(false);
	CurrentResourceOwner = owner;
	MemoryContextSwitchTo(old);
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

/* dolt_serve_format() returns text: the canonical format of the commits. */
Datum
dolt_serve_format(PG_FUNCTION_ARGS)
{
	ProllyDatabaseComponents dc;
	StringInfoData f;

	check_serving(false);
	prolly_database_components(&dc);
	initStringInfo(&f);
	PROLLY_CALL(dc.canonicalizer, format, &f);
	PG_RETURN_TEXT_P(cstring_to_text(f.data));
}

static void
head_row(const char *name, const ProllyHash *head, void *arg)
{
	ReturnSetInfo *rsinfo = arg;
	Datum		values[2];
	bool		nulls[2] = {0};

	/* (a branch without commits, or a session's at a commit: not served) */
	if (head->len == 0 ||
		strncmp(name, PROLLY_DETACHED_PREFIX, strlen(PROLLY_DETACHED_PREFIX)) == 0)
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
 * setof record: the chunks the store has of those, each with its hash.
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
		if (!PROLLY_CALL(prolly_chunk_store(), get, cs, &hashes[i], &data))
			continue;
		values[0] = PointerGetDatum(hash_bytea(&hashes[i]));
		values[1] = PointerGetDatum(bytes_bytea(data.data, data.len));
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
		CHECK_FOR_INTERRUPTS();
	}
	return (Datum) 0;
}

/*
 * dolt_serve_receive(hashes bytea[], chunks bytea[]) returns void: chunks
 * a push sends, kept for dolt_serve_move_head() in the same transaction.
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
		c = hash_search(s->chunks, &hashes[i], HASH_ENTER, &found);
		if (found)
			continue;
		chunk = DatumGetByteaPP(chunks[i]);
		c->offset = s->len;
		c->len = VARSIZE_ANY_EXHDR(chunk);
		if (BufFileSeek(s->file, 0, s->len, SEEK_SET) != 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not seek in a push's temporary file: %m")));
		BufFileWrite(s->file, VARDATA_ANY(chunk), c->len);
		s->len += c->len;
	}
	PG_RETURN_VOID();
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
	StringInfoData mine;

	check_serving(true);
	PreventCommandIfReadOnly("dolt_serve_move_head()");
	if (PG_ARGISNULL(0) || PG_ARGISNULL(1) || PG_ARGISNULL(3) || PG_ARGISNULL(4))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("only dolt_serve_move_head's head can be null")));
	branch = text_to_cstring(PG_GETARG_TEXT_PP(0));
	if (VARSIZE_ANY_EXHDR(PG_GETARG_BYTEA_PP(1)) > 0)
		bytea_hash(PG_GETARG_BYTEA_PP(1), &expected);
	prolly_database_components(&dc);
	initStringInfo(&mine);
	PROLLY_CALL(dc.canonicalizer, format, &mine);
	if (strcmp(text_to_cstring(PG_GETARG_TEXT_PP(3)), mine.data) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("this database holds commits of canonical format %s, not %s",
						mine.data, text_to_cstring(PG_GETARG_TEXT_PP(3)))));
	if (has_head)
	{
		bytea_hash(PG_GETARG_BYTEA_PP(2), &head);
		(void) PROLLY_CALL(dc.transfer, receive, &spool_remote, NULL, &head, 1);
	}
	PG_RETURN_BOOL(PROLLY_CALL(dc.refs, branch_move, branch, &expected,
							   has_head ? &head : NULL,
							   PG_GETARG_BOOL(4)));
}
