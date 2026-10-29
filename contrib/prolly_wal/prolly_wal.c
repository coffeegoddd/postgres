/*-------------------------------------------------------------------------
 *
 * prolly_wal.c
 *	  A spike on stock Postgres: a chunk store and named roots, logged
 *	  through a custom WAL resource manager, with a root that moves exactly
 *	  when the SQL transaction that set it commits.
 *
 * Chunks are content-addressed files under pg_prolly/chunks, each written
 * after its WAL record (a chunk is the same bytes wherever it is written,
 * so redo only writes the ones missing).  A custom resource manager has
 * no checkpoint callback, so nothing would flush these files before a
 * checkpoint lets their WAL go: the transaction flushes the files it
 * wrote before it commits.
 *
 * A root is a name and a chunk hash under pg_prolly/roots.  set_root logs
 * the change and holds a lock on the name until the transaction ends.
 * Before the commit record, a pending file (pg_prolly/pending/<xid>)
 * names the roots, durably; after it, the roots are written and the
 * pending file goes; on abort it goes too.  At the end of recovery, the
 * pending files of committed transactions are applied and the rest
 * dropped, so a crash between the commit record and the roots' files, or
 * a checkpoint between set_root's record and the commit, loses nothing.
 * Redo of a root record writes the pending file too, so a standby or a
 * point-in-time recovery applies roots when it ends recovery.
 *
 * contrib/prolly_wal/prolly_wal.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/stat.h>
#include <unistd.h>

#include "access/transam.h"
#include "access/xact.h"
#include "access/xlog_internal.h"
#include "access/xloginsert.h"
#include "access/xlogreader.h"
#include "common/cryptohash.h"
#include "common/hashfn.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "storage/lock.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/memutils.h"

PG_MODULE_MAGIC_EXT(.name = "prolly_wal",.version = PG_VERSION);

PG_FUNCTION_INFO_V1(prolly_wal_put_chunk);
PG_FUNCTION_INFO_V1(prolly_wal_get_chunk);
PG_FUNCTION_INFO_V1(prolly_wal_set_root);
PG_FUNCTION_INFO_V1(prolly_wal_get_root);

#define RM_PROLLY_ID		RM_EXPERIMENTAL_ID
#define XLOG_PROLLY_CHUNK	0x00
#define XLOG_PROLLY_ROOT	0x10

#define BASE_DIR	"pg_prolly"
#define CHUNK_DIR	"pg_prolly/chunks"
#define ROOT_DIR	"pg_prolly/roots"
#define PENDING_DIR "pg_prolly/pending"

#define HASH_LEN	32
#define NAME_MAX_LEN 63

/* For the crash tests: fail hard after the commit record, before the roots. */
static bool crash_after_commit = false;

typedef struct RootSet
{
	char		name[NAME_MAX_LEN + 1];
	char		hash[HASH_LEN * 2 + 1];
	SubTransactionId subxid;
} RootSet;

/* This transaction's root changes and the chunk files it wrote. */
static List *pending_roots = NIL;
static List *written_chunks = NIL;

/* ----------------------------------------------------------------- files */

static void
ensure_dirs(void)
{
	const char *dirs[] = {BASE_DIR, CHUNK_DIR, ROOT_DIR, PENDING_DIR};

	for (int i = 0; i < lengthof(dirs); i++)
		if (MakePGDirectory(dirs[i]) < 0 && errno != EEXIST)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not create directory \"%s\": %m", dirs[i])));
}

static void
write_all(const char *path, const char *data, size_t len, bool sync)
{
	int			fd = OpenTransientFile(path, O_WRONLY | O_CREAT | O_TRUNC | PG_BINARY);

	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m", path)));
	if (write(fd, data, len) != (ssize_t) len)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write file \"%s\": %m", path)));
	if (sync && pg_fsync(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m", path)));
	CloseTransientFile(fd);
}

/* Writes path whole: a temporary file, synced and renamed into place. */
static void
write_durable(const char *path, const char *data, size_t len)
{
	char		tmp[MAXPGPATH];

	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	write_all(tmp, data, len, true);
	durable_rename(tmp, path, ERROR);
}

static bool
read_whole(const char *path, StringInfo out)
{
	int			fd = OpenTransientFile(path, O_RDONLY | PG_BINARY);
	char		buf[8192];
	ssize_t		n;

	if (fd < 0)
	{
		if (errno == ENOENT)
			return false;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", path)));
	}
	while ((n = read(fd, buf, sizeof(buf))) > 0)
		appendBinaryStringInfo(out, buf, n);
	CloseTransientFile(fd);
	return true;
}

static void
hex_of(const uint8 *bytes, char *out)
{
	static const char digits[] = "0123456789abcdef";

	for (int i = 0; i < HASH_LEN; i++)
	{
		out[2 * i] = digits[bytes[i] >> 4];
		out[2 * i + 1] = digits[bytes[i] & 15];
	}
	out[2 * HASH_LEN] = '\0';
}

static void
check_name(const char *name)
{
	size_t		len = strlen(name);

	if (len == 0 || len > NAME_MAX_LEN || strspn(name, "abcdefghijklmnopqrstuvwxyz0123456789_") != len)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid root name \"%s\"", name)));
}

static void
check_hash(const char *hash)
{
	if (strlen(hash) != HASH_LEN * 2 || strspn(hash, "0123456789abcdef") != HASH_LEN * 2)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid chunk hash \"%s\"", hash)));
}

/* Writes a chunk's file if it is missing; returns whether it wrote one. */
static bool
store_chunk(const char *hex, const char *data, size_t len, bool sync)
{
	char		path[MAXPGPATH],
				tmp[MAXPGPATH];
	struct stat st;

	snprintf(path, sizeof(path), CHUNK_DIR "/%s", hex);
	if (stat(path, &st) == 0)
		return false;
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	write_all(tmp, data, len, sync);
	if (sync)
		durable_rename(tmp, path, ERROR);
	else if (rename(tmp, path) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not rename file \"%s\": %m", tmp)));
	return true;
}

/* Applies the "name hash" lines of a pending file to the roots. */
static void
apply_pending(const char *text)
{
	char	   *copy = pstrdup(text),
			   *save = NULL;

	for (char *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
	{
		char	   *sp = strchr(line, ' ');
		char		path[MAXPGPATH];

		if (sp == NULL)
			continue;
		*sp = '\0';
		snprintf(path, sizeof(path), ROOT_DIR "/%s", line);
		write_durable(path, sp + 1, strlen(sp + 1));
	}
	pfree(copy);
}

static void
pending_path(TransactionId xid, char *path)
{
	snprintf(path, MAXPGPATH, PENDING_DIR "/%u", xid);
}

/* ---------------------------------------------------------- transactions */

static void
prolly_xact(XactEvent event, void *arg)
{
	char		path[MAXPGPATH];
	ListCell   *lc;

	switch (event)
	{
		case XACT_EVENT_PRE_COMMIT:
			/* the chunks this transaction wrote, durable before it commits */
			foreach(lc, written_chunks)
				fsync_fname((char *) lfirst(lc), false);
			if (written_chunks != NIL)
				fsync_fname(CHUNK_DIR, true);
			if (pending_roots != NIL)
			{
				StringInfoData buf;

				initStringInfo(&buf);
				foreach(lc, pending_roots)
				{
					RootSet    *r = lfirst(lc);

					appendStringInfo(&buf, "%s %s\n", r->name, r->hash);
				}
				pending_path(GetTopTransactionId(), path);
				write_durable(path, buf.data, buf.len);
			}
			break;
		case XACT_EVENT_COMMIT:
			if (pending_roots != NIL)
			{
				TransactionId xid = GetTopTransactionIdIfAny();
				StringInfoData buf;

				if (crash_after_commit)
					elog(PANIC, "prolly_wal: crashing after the commit record, before the roots");
				pending_path(xid, path);
				initStringInfo(&buf);
				if (read_whole(path, &buf))
					apply_pending(buf.data);
				durable_unlink(path, WARNING);
			}
			pending_roots = NIL;
			written_chunks = NIL;
			break;
		case XACT_EVENT_ABORT:
			if (pending_roots != NIL && TransactionIdIsValid(GetTopTransactionIdIfAny()))
			{
				pending_path(GetTopTransactionIdIfAny(), path);
				unlink(path);
			}
			pending_roots = NIL;
			written_chunks = NIL;
			break;
		case XACT_EVENT_PREPARE:
		case XACT_EVENT_PRE_PREPARE:
			if (pending_roots != NIL)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot prepare a transaction that set a root")));
			break;
		default:
			break;
	}
}

/* A subtransaction rolled back takes back the roots it set. */
static void
prolly_subxact(SubXactEvent event, SubTransactionId mySubid, SubTransactionId parentSubid,
			   void *arg)
{
	if (event == SUBXACT_EVENT_ABORT_SUB)
	{
		ListCell   *lc;

		foreach(lc, pending_roots)
		{
			RootSet    *r = lfirst(lc);

			if (r->subxid == mySubid)
				pending_roots = foreach_delete_current(pending_roots, lc);
		}
	}
	else if (event == SUBXACT_EVENT_COMMIT_SUB)
	{
		ListCell   *lc;

		foreach(lc, pending_roots)
		{
			RootSet    *r = lfirst(lc);

			if (r->subxid == mySubid)
				r->subxid = parentSubid;
		}
	}
}

/* --------------------------------------------------------- the functions */

/*
 * put_chunk(data bytea) returns text
 *
 * Stores a chunk under its SHA-256 and returns the hash: logged, then
 * written; made durable when the transaction commits.
 */
Datum
prolly_wal_put_chunk(PG_FUNCTION_ARGS)
{
	bytea	   *data = PG_GETARG_BYTEA_PP(0);
	const char *bytes = VARDATA_ANY(data);
	size_t		len = VARSIZE_ANY_EXHDR(data);
	pg_cryptohash_ctx *ctx = pg_cryptohash_create(PG_SHA256);
	uint8		digest[HASH_LEN];
	char		hex[HASH_LEN * 2 + 1];

	if (pg_cryptohash_init(ctx) < 0 ||
		pg_cryptohash_update(ctx, (const uint8 *) bytes, len) < 0 ||
		pg_cryptohash_final(ctx, digest, sizeof(digest)) < 0)
		elog(ERROR, "could not hash a chunk");
	pg_cryptohash_free(ctx);
	hex_of(digest, hex);

	ensure_dirs();
	XLogBeginInsert();
	XLogRegisterData(digest, HASH_LEN);
	XLogRegisterData(bytes, len);
	XLogInsert(RM_PROLLY_ID, XLOG_PROLLY_CHUNK);

	if (store_chunk(hex, bytes, len, false))
	{
		MemoryContext old = MemoryContextSwitchTo(TopTransactionContext);

		written_chunks = lappend(written_chunks, psprintf(CHUNK_DIR "/%s", hex));
		MemoryContextSwitchTo(old);
	}
	PG_RETURN_TEXT_P(cstring_to_text(hex));
}

Datum
prolly_wal_get_chunk(PG_FUNCTION_ARGS)
{
	char	   *hash = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char		path[MAXPGPATH];
	StringInfoData buf;
	bytea	   *out;

	check_hash(hash);
	snprintf(path, sizeof(path), CHUNK_DIR "/%s", hash);
	initStringInfo(&buf);
	if (!read_whole(path, &buf))
		PG_RETURN_NULL();
	out = palloc(VARHDRSZ + buf.len);
	SET_VARSIZE(out, VARHDRSZ + buf.len);
	memcpy(VARDATA(out), buf.data, buf.len);
	PG_RETURN_BYTEA_P(out);
}

/*
 * set_root(name text, hash text)
 *
 * Moves root name to hash when this transaction commits, and not if it
 * aborts.  The name stays locked until then, so roots move in commit order.
 */
Datum
prolly_wal_set_root(PG_FUNCTION_ARGS)
{
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char	   *hash = text_to_cstring(PG_GETARG_TEXT_PP(1));
	LOCKTAG		tag;
	TransactionId xid;
	RootSet    *r;
	MemoryContext old;

	check_name(name);
	check_hash(hash);
	ensure_dirs();
	SET_LOCKTAG_ADVISORY(tag, MyDatabaseId, hash_bytes((const unsigned char *) name, strlen(name)),
						 0, 99);
	(void) LockAcquire(&tag, ExclusiveLock, false, false);

	xid = GetTopTransactionId();
	XLogBeginInsert();
	XLogRegisterData(&xid, sizeof(xid));
	XLogRegisterData(name, strlen(name) + 1);
	XLogRegisterData(hash, strlen(hash) + 1);
	XLogInsert(RM_PROLLY_ID, XLOG_PROLLY_ROOT);

	old = MemoryContextSwitchTo(TopTransactionContext);
	r = palloc0(sizeof(RootSet));
	strlcpy(r->name, name, sizeof(r->name));
	strlcpy(r->hash, hash, sizeof(r->hash));
	r->subxid = GetCurrentSubTransactionId();
	pending_roots = lappend(pending_roots, r);
	MemoryContextSwitchTo(old);
	PG_RETURN_VOID();
}

Datum
prolly_wal_get_root(PG_FUNCTION_ARGS)
{
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char		path[MAXPGPATH];
	StringInfoData buf;

	check_name(name);
	snprintf(path, sizeof(path), ROOT_DIR "/%s", name);
	initStringInfo(&buf);
	if (!read_whole(path, &buf))
		PG_RETURN_NULL();
	PG_RETURN_TEXT_P(cstring_to_text(buf.data));
}

/* -------------------------------------------------------- the resource manager */

static void
prolly_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
	char	   *data = XLogRecGetData(record);
	size_t		len = XLogRecGetDataLen(record);

	ensure_dirs();
	if (info == XLOG_PROLLY_CHUNK)
	{
		char		hex[HASH_LEN * 2 + 1];

		hex_of((const uint8 *) data, hex);
		store_chunk(hex, data + HASH_LEN, len - HASH_LEN, true);
	}
	else if (info == XLOG_PROLLY_ROOT)
	{
		TransactionId xid;
		const char *name,
				   *hash;
		char		path[MAXPGPATH];
		StringInfoData buf;
		StringInfoData line;

		memcpy(&xid, data, sizeof(xid));
		name = data + sizeof(xid);
		hash = name + strlen(name) + 1;
		pending_path(xid, path);
		initStringInfo(&buf);
		read_whole(path, &buf);
		initStringInfo(&line);
		appendStringInfo(&line, "%s %s\n", name, hash);
		if (strstr(buf.data, line.data) == NULL)
		{
			appendStringInfoString(&buf, line.data);
			write_durable(path, buf.data, buf.len);
		}
	}
	else
		elog(PANIC, "prolly_redo: unknown op code %u", info);
}

static int
xid_order(const ListCell *a, const ListCell *b)
{
	TransactionId x = lfirst_xid(a),
				y = lfirst_xid(b);

	return (x > y) - (x < y);
}

/*
 * At the end of recovery: the roots of every transaction that committed,
 * applied; the rest dropped.  A name's lock is held until its transaction
 * has written its roots and dropped its pending file, so at most one
 * committed pending file names any root.
 */
static void
prolly_cleanup(void)
{
	DIR		   *dir;
	struct dirent *de;
	List	   *xids = NIL;
	ListCell   *lc;

	ensure_dirs();
	dir = AllocateDir(PENDING_DIR);
	while ((de = ReadDir(dir, PENDING_DIR)) != NULL)
	{
		char	   *end;
		unsigned long xid = strtoul(de->d_name, &end, 10);

		if (de->d_name[0] == '.' || *end != '\0')
			continue;
		xids = lappend_xid(xids, (TransactionId) xid);
	}
	FreeDir(dir);
	list_sort(xids, xid_order);
	foreach(lc, xids)
	{
		TransactionId xid = lfirst_xid(lc);
		char		path[MAXPGPATH];
		StringInfoData buf;

		pending_path(xid, path);
		if (TransactionIdDidCommit(xid))
		{
			initStringInfo(&buf);
			if (read_whole(path, &buf))
				apply_pending(buf.data);
			elog(LOG, "prolly_wal: applied the roots of committed transaction %u", xid);
		}
		else
			elog(LOG, "prolly_wal: dropped the roots of transaction %u, which did not commit", xid);
		durable_unlink(path, WARNING);
	}
}

static void
prolly_desc(StringInfo buf, XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
	char	   *data = XLogRecGetData(record);

	if (info == XLOG_PROLLY_CHUNK)
	{
		char		hex[HASH_LEN * 2 + 1];

		hex_of((const uint8 *) data, hex);
		appendStringInfo(buf, "chunk %s, %u bytes", hex, XLogRecGetDataLen(record) - HASH_LEN);
	}
	else if (info == XLOG_PROLLY_ROOT)
	{
		TransactionId xid;

		memcpy(&xid, data, sizeof(xid));
		appendStringInfo(buf, "root %s -> %s, xid %u", data + sizeof(xid),
						 data + sizeof(xid) + strlen(data + sizeof(xid)) + 1, xid);
	}
}

static const char *
prolly_identify(uint8 info)
{
	switch (info & ~XLR_INFO_MASK)
	{
		case XLOG_PROLLY_CHUNK:
			return "CHUNK";
		case XLOG_PROLLY_ROOT:
			return "ROOT";
	}
	return NULL;
}

static const RmgrData prolly_rmgr = {
	.rm_name = "prolly",
	.rm_redo = prolly_redo,
	.rm_desc = prolly_desc,
	.rm_identify = prolly_identify,
	.rm_cleanup = prolly_cleanup,
};

void
_PG_init(void)
{
	if (!process_shared_preload_libraries_in_progress)
		ereport(ERROR,
				(errmsg("prolly_wal must be loaded through shared_preload_libraries")));
	DefineCustomBoolVariable("prolly_wal.crash_after_commit",
							 "Crashes after a commit record, before the roots it set are written (tests).",
							 NULL, &crash_after_commit, false, PGC_SUSET, 0, NULL, NULL, NULL);
	MarkGUCPrefixReserved("prolly_wal");
	RegisterCustomRmgr(RM_PROLLY_ID, &prolly_rmgr);
	RegisterXactCallback(prolly_xact, NULL);
	RegisterSubXactCallback(prolly_subxact, NULL);
}
