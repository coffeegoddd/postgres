/*-------------------------------------------------------------------------
 *
 * commit_serial.c
 *	  Commit pipeline implementation "serial": one committer per branch.
 *
 * The branch lock is a heavyweight lock (LOCKTAG_PROLLY_BRANCH) that a
 * committer keeps until its transaction ends, so the commits to a branch
 * happen one at a time, including their WAL flush.  That makes ordering
 * trivial: there is at most one enqueued, unpublished root per branch at a
 * time on a primary, and it is published or abandoned before the next
 * committer can call tip().  The ordered pipeline replaces this when
 * commit throughput matters.
 *
 * State, in shared memory under ProllyHeadsLock:
 *
 *	heads		(database, branch) -> published root hash
 *	pending		roots that were enqueued and whose transaction hasn't
 *				been seen to finish: (database, branch, xid, root, seq)
 *
 * enqueue() adds a pending entry and then writes a ProllyCommit record
 * carrying it; publish() moves the entry to heads; abandon() drops it.
 * Redo of the record adds the same pending entry.  Pending entries left by
 * recovery are settled lazily, by the commit log: a committed xid's root
 * becomes the head, an xid that is neither committed nor running is
 * dropped.  So a branch's head after a crash, or on a standby, is the
 * root of the last enqueued transaction that committed.
 *
 * CREATE DATABASE copies the template's heads as pending entries of the
 * new database under the creating transaction's xid, logged like any
 * enqueue; the creating backend publishes them when its transaction
 * commits and drops them when it aborts, and after a crash or on a
 * standby they are settled through the commit log like any other.  DROP
 * DATABASE removes the database's heads and pending entries and logs that.
 *
 * Snapshots.  A head replaced by a newer one goes into a history, with
 * the xid that committed it and the xid that replaced it, so a snapshot
 * that doesn't see the newer commit can still find the root it does see
 * (snapshot_head()).  An entry is dropped once the xid that replaced it
 * precedes the oldest xmin of any snapshot (every snapshot sees the newer
 * root); if the history is full of entries still needed, the oldest is
 * given up, and a snapshot that needed it gets "snapshot too old".  The
 * history lives only in shared memory: after a restart no snapshot is
 * older than the heads, and a standby keeps its own for its own snapshots.
 *
 * Settling lazily leaves entries behind that nobody reads again: those of
 * a database nobody opens after a crash, or of transactions that ended in
 * a crash.  Their xids are invisible to the commit log's truncation and
 * to xid wraparound, so a checkpoint on a primary settles every entry
 * whose transaction has finished.  Restartpoints and the end-of-recovery
 * checkpoint don't: during recovery a transaction whose commit hasn't been
 * replayed yet may look finished.  The sweep never raises an error; an
 * entry it can't settle (its status already truncated from the commit
 * log, or no room for a new head) is left, with a log message.
 *
 * The heads table has room for MAX_HEADS (database, branch) pairs across
 * the cluster (debug_prolly_max_branch_heads lowers that, for tests).  A
 * root for a pair with no head yet takes a slot when it is enqueued, so a
 * full table makes the transaction fail before it commits; publishing,
 * and replaying commits the primary accepted, never need a slot that
 * isn't there.
 *
 * Checkpoints write heads and pending entries to global/prolly_heads
 * (written to a temporary file, then durably renamed), which shmem_init
 * reads back.  Entries are added to shared memory before their WAL
 * record is inserted, so a checkpoint either sees an entry or has its
 * redo point before the entry's record.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/commit_serial.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <unistd.h>

#include "access/prolly_builtins.h"
#include "access/prolly_shmem.h"
#include "access/prolly_xlog.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "port/pg_crc32c.h"
#include "storage/fd.h"
#include "storage/lmgr.h"
#include "storage/lwlock.h"
#include "storage/procarray.h"
#include "utils/snapmgr.h"

#define SERIAL_IMPL_ID	1

/* ProllyCommit record types (high bits of info) */
#define XLOG_PROLLY_COMMIT_ENQUEUE	0x10
#define XLOG_PROLLY_COMMIT_DROP_DATABASE	0x20

#define MAX_HEADS		1024
#define MAX_PENDING		1024
#define MAX_HISTORY		4096

#define HEADS_FILE		"global/prolly_heads"
#define HEADS_TEMP		"global/prolly_heads.tmp"
#define HEADS_MAGIC		0x50484453	/* "PHDS" */
#define HEADS_VERSION	1

typedef struct HeadEntry
{
	Oid			dboid;
	ProllyBranchId branch;
	ProllyHash	root;
} HeadEntry;

typedef struct PendingEntry
{
	Oid			dboid;
	ProllyBranchId branch;
	TransactionId xid;
	uint64		seq;
	ProllyHash	root;
} PendingEntry;

/* A head that was replaced, kept for snapshots that don't see its successor. */
typedef struct HistoryEntry
{
	Oid			dboid;
	ProllyBranchId branch;
	TransactionId xid;			/* committed it; invalid: seen by everyone */
	TransactionId succ_xid;		/* committed its successor */
	bool		has_root;		/* false: the branch had no root yet */
	ProllyHash	root;
} HistoryEntry;

typedef struct SerialShared
{
	uint64		next_seq;
	int			nheads;
	int			npending;
	int			nhistory;
	HeadEntry	heads[MAX_HEADS];
	TransactionId head_xids[MAX_HEADS]; /* committed heads[i]; invalid: seen
										 * by everyone */
	PendingEntry pending[MAX_PENDING];	/* in seq order */
	HistoryEntry history[MAX_HISTORY];	/* oldest first */
} SerialShared;

/* On disk: this header, the entries, then a CRC of everything before it. */
typedef struct HeadsFileHeader
{
	uint32		magic;
	uint32		version;
	uint32		nheads;
	uint32		npending;
} HeadsFileHeader;

/* The WAL record's payload, after xl_prolly_header. */
typedef struct xl_serial_enqueue
{
	Oid			dboid;
	ProllyBranchId branch;
	TransactionId xid;
	uint8		hash_len;
	uint8		hash[FLEXIBLE_ARRAY_MEMBER];
} xl_serial_enqueue;

#define SizeOfSerialEnqueue	offsetof(xl_serial_enqueue, hash)

typedef struct xl_serial_drop_database
{
	Oid			dboid;
} xl_serial_drop_database;

static SerialShared *shared = NULL;

int			debug_prolly_max_branch_heads = 0;
int			debug_prolly_max_root_history = 0;

/*
 * Pending entries this backend's transaction added by copying a database,
 * published or dropped when it ends.
 */
static uint64 copied_seqs[MAX_HEADS];
static int	ncopied = 0;
static bool copy_callback_registered = false;

static SerialShared *
get_shared(void)
{
	if (shared == NULL)
	{
		shared = prolly_shmem_area(&prolly_commit_serial.info);
		if (shared == NULL)
			elog(ERROR, "prolly commit pipeline \"serial\" has no shared memory");
	}
	return shared;
}

static void
check_root(const ProllyNodeRef *root)
{
	if (root->hash.len == 0 || root->hash.len > PROLLY_HASH_MAX_LEN)
		elog(ERROR, "prolly branch root has an invalid hash length %u",
			 (unsigned) root->hash.len);
}

static void
root_ref(const ProllyHash *hash, ProllyNodeRef *root)
{
	memset(root, 0, sizeof(*root));
	root->hash = *hash;
	root->location.blkno = InvalidBlockNumber;
}

/* ------------------------------------------------------------------------
 * Heads and pending entries; caller holds ProllyHeadsLock exclusively
 * unless noted.
 * ------------------------------------------------------------------------
 */

/* Shared or exclusive lock. */
static HeadEntry *
find_head(SerialShared *s, Oid dboid, ProllyBranchId branch)
{
	for (int i = 0; i < s->nheads; i++)
	{
		if (s->heads[i].dboid == dboid && s->heads[i].branch == branch)
			return &s->heads[i];
	}
	return NULL;
}

static int
history_limit(void)
{
	if (debug_prolly_max_root_history > 0 &&
		debug_prolly_max_root_history < MAX_HISTORY)
		return debug_prolly_max_root_history;
	return MAX_HISTORY;
}

/*
 * Drop history entries no snapshot can need: those whose successor every
 * snapshot sees.  If that leaves no room, give up the oldest.
 */
static void
make_history_room(SerialShared *s)
{
	TransactionId horizon;
	int			n = 0;

	if (s->nhistory < history_limit())
		return;
	horizon = GetOldestNonRemovableTransactionId(NULL);
	for (int i = 0; i < s->nhistory; i++)
	{
		if (!TransactionIdPrecedes(s->history[i].succ_xid, horizon))
			s->history[n++] = s->history[i];
	}
	s->nhistory = n;
	while (s->nhistory >= history_limit())
	{
		memmove(&s->history[0], &s->history[1],
				sizeof(HistoryEntry) * (s->nhistory - 1));
		s->nhistory--;
	}
}

static void
add_history(SerialShared *s, Oid dboid, ProllyBranchId branch,
			TransactionId xid, TransactionId succ_xid, bool has_root,
			const ProllyHash *root)
{
	HistoryEntry *e;

	make_history_room(s);
	e = &s->history[s->nhistory++];
	memset(e, 0, sizeof(*e));
	e->dboid = dboid;
	e->branch = branch;
	e->xid = xid;
	e->succ_xid = succ_xid;
	e->has_root = has_root;
	if (has_root)
		e->root = *root;
}

/* root, committed by xid, becomes the head; the old one goes to history. */
static void
set_head(SerialShared *s, Oid dboid, ProllyBranchId branch,
		 const ProllyHash *root, TransactionId xid)
{
	HeadEntry  *h = find_head(s, dboid, branch);

	if (h == NULL)
	{
		/* Slots are reserved before commit; see reserve_slots(). */
		if (s->nheads >= MAX_HEADS)
			ereport(PANIC,
					(errmsg("too many prolly branch heads (limit %d)", MAX_HEADS)));
		add_history(s, dboid, branch, InvalidTransactionId, xid, false, NULL);
		h = &s->heads[s->nheads++];
		h->dboid = dboid;
		h->branch = branch;
	}
	else
		add_history(s, dboid, branch, s->head_xids[h - s->heads], xid, true,
					&h->root);
	h->root = *root;
	s->head_xids[h - s->heads] = xid;
}

static void
remove_pending(SerialShared *s, int i)
{
	Assert(i >= 0 && i < s->npending);
	memmove(&s->pending[i], &s->pending[i + 1],
			sizeof(PendingEntry) * (s->npending - i - 1));
	s->npending--;
}

static uint64
add_pending(SerialShared *s, Oid dboid, ProllyBranchId branch,
			TransactionId xid, const ProllyHash *root)
{
	PendingEntry *p;

	if (s->npending >= MAX_PENDING)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("too many prolly commits in progress (limit %d)",
						MAX_PENDING)));
	p = &s->pending[s->npending++];
	p->dboid = dboid;
	p->branch = branch;
	p->xid = xid;
	p->seq = s->next_seq++;
	p->root = *root;
	return p->seq;
}

/* Shared or exclusive lock. */
static bool
branch_has_pending(SerialShared *s, Oid dboid, ProllyBranchId branch)
{
	for (int i = 0; i < s->npending; i++)
	{
		if (s->pending[i].dboid == dboid && s->pending[i].branch == branch)
			return true;
	}
	return false;
}

/*
 * For the checkpoint's sweep: can p be settled without raising an error?
 * Its xid's status must still be in the commit log, and if it committed
 * there must be room for its head.  Caller holds XactTruncationLock.
 */
static bool
can_sweep(SerialShared *s, PendingEntry *p)
{
	if (TransactionIdPrecedes(p->xid, TransamVariables->oldestClogXid))
	{
		ereport(LOG,
				(errmsg("prolly commit of transaction %u to database %u can't be settled: its status is no longer in the commit log",
						p->xid, p->dboid)));
		return false;
	}
	if (s->nheads >= MAX_HEADS && find_head(s, p->dboid, p->branch) == NULL &&
		TransactionIdDidCommit(p->xid))
	{
		ereport(LOG,
				(errmsg("prolly commit of transaction %u to database %u can't be settled: too many prolly branch heads (limit %d)",
						p->xid, p->dboid, MAX_HEADS)));
		return false;
	}
	return true;
}

/*
 * Settle the branch's pending entries whose transactions are known to have
 * finished, oldest first: a committed one's root becomes the head, others
 * are dropped.  With check_running, stops at the first transaction still
 * running; redo passes false, because there every entry below below_seq
 * has finished (commits to a branch are serialized).  With sweeping (and
 * XactTruncationLock held), also stops at the first entry can_sweep()
 * refuses.
 */
static void
settle_pending(SerialShared *s, Oid dboid, ProllyBranchId branch,
			   bool check_running, uint64 below_seq, bool sweeping)
{
	int			i = 0;

	while (i < s->npending)
	{
		PendingEntry *p = &s->pending[i];

		if (p->dboid != dboid || p->branch != branch || p->seq >= below_seq)
		{
			i++;
			continue;
		}
		if (sweeping && !can_sweep(s, p))
			break;
		if (TransactionIdDidCommit(p->xid))
			set_head(s, dboid, branch, &p->root, p->xid);
		else if (check_running && TransactionIdIsInProgress(p->xid))
			break;
		remove_pending(s, i);
	}
}

/* Settle every branch of dboid as far as finished transactions allow. */
static void
settle_database(SerialShared *s, Oid dboid)
{
	ProllyBranchId *branches = palloc(sizeof(ProllyBranchId) * MAX_PENDING);
	int			n = 0;

	for (int i = 0; i < s->npending; i++)
	{
		if (s->pending[i].dboid == dboid)
			branches[n++] = s->pending[i].branch;
	}
	for (int i = 0; i < n; i++)
		settle_pending(s, dboid, branches[i], true, PG_UINT64_MAX, false);
	pfree(branches);
}

/*
 * Settle every branch's finished entries; with sweeping (and
 * XactTruncationLock held), without raising errors.
 */
static void
settle_all(SerialShared *s, bool sweeping)
{
	PendingEntry *keys = palloc(sizeof(PendingEntry) * MAX_PENDING);
	int			n = 0;

	for (int i = 0; i < s->npending; i++)
	{
		bool		seen = false;

		for (int j = 0; j < n && !seen; j++)
			seen = (keys[j].dboid == s->pending[i].dboid &&
					keys[j].branch == s->pending[i].branch);
		if (!seen)
			keys[n++] = s->pending[i];
	}
	for (int i = 0; i < n; i++)
		settle_pending(s, keys[i].dboid, keys[i].branch, true, PG_UINT64_MAX,
					   sweeping);
	pfree(keys);
}

static void
sweep_pending(SerialShared *s)
{
	LWLockAcquire(ProllyHeadsLock, LW_EXCLUSIVE);
	LWLockAcquire(XactTruncationLock, LW_SHARED);
	settle_all(s, true);
	LWLockRelease(XactTruncationLock);
	LWLockRelease(ProllyHeadsLock);
}

/*
 * During redo, with no room for another entry: settle the entries whose
 * commit or abort record has been replayed.  Each branch holds at most one
 * entry here, so this only leaves entries of transactions still open at
 * this point of the WAL.  Exclusive lock.
 */
static void
settle_replayed(SerialShared *s)
{
	int			i = 0;

	while (i < s->npending)
	{
		PendingEntry *p = &s->pending[i];

		if (TransactionIdDidCommit(p->xid))
			set_head(s, p->dboid, p->branch, &p->root, p->xid);
		else if (!TransactionIdDidAbort(p->xid))
		{
			i++;
			continue;
		}
		remove_pending(s, i);
	}
}

static int
head_limit(void)
{
	if (debug_prolly_max_branch_heads > 0 &&
		debug_prolly_max_branch_heads < MAX_HEADS)
		return debug_prolly_max_branch_heads;
	return MAX_HEADS;
}

/* Heads, plus the pairs with pending roots that will need one. */
static int
slots_in_use(SerialShared *s)
{
	int			n = s->nheads;

	for (int i = 0; i < s->npending; i++)
	{
		PendingEntry *p = &s->pending[i];
		bool		counted = false;

		if (find_head(s, p->dboid, p->branch) != NULL)
			continue;
		for (int j = 0; j < i && !counted; j++)
			counted = (s->pending[j].dboid == p->dboid &&
					   s->pending[j].branch == p->branch);
		if (!counted)
			n++;
	}
	return n;
}

/*
 * Make sure n more pairs can get a head, or raise an error (releasing
 * ProllyHeadsLock, which the caller holds exclusively).
 */
static void
reserve_slots(SerialShared *s, int n)
{
	if (slots_in_use(s) + n <= head_limit())
		return;

	/* Entries of transactions that have finished may be holding slots. */
	settle_all(s, false);
	if (slots_in_use(s) + n <= head_limit())
		return;
	LWLockRelease(ProllyHeadsLock);
	ereport(ERROR,
			(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
			 errmsg("too many prolly branch heads"),
			 errdetail("The cluster's databases can have at most %d branches with prolly data between them.",
					   head_limit()),
			 errhint("Drop databases that are no longer needed.")));
}

/* Shared or exclusive lock. */
static bool
database_has_pending(SerialShared *s, Oid dboid)
{
	for (int i = 0; i < s->npending; i++)
	{
		if (s->pending[i].dboid == dboid)
			return true;
	}
	return false;
}

static void
forget_database(SerialShared *s, Oid dboid)
{
	int			n = 0;

	for (int i = 0; i < s->nheads; i++)
	{
		if (s->heads[i].dboid != dboid)
		{
			s->head_xids[n] = s->head_xids[i];
			s->heads[n++] = s->heads[i];
		}
	}
	s->nheads = n;
	n = 0;
	for (int i = 0; i < s->nhistory; i++)
	{
		if (s->history[i].dboid != dboid)
			s->history[n++] = s->history[i];
	}
	s->nhistory = n;
	for (int i = 0; i < s->npending;)
	{
		if (s->pending[i].dboid == dboid)
			remove_pending(s, i);
		else
			i++;
	}
}

/* ------------------------------------------------------------------------
 * The heads file
 * ------------------------------------------------------------------------
 */

static void
read_heads_file(SerialShared *s)
{
	int			fd;
	HeadsFileHeader hdr;
	pg_crc32c	crc;
	pg_crc32c	file_crc;
	Size		hlen;
	Size		plen;

	fd = OpenTransientFile(HEADS_FILE, O_RDONLY | PG_BINARY);
	if (fd < 0)
	{
		if (errno == ENOENT)
			return;
		ereport(FATAL,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", HEADS_FILE)));
	}

	if (read(fd, &hdr, sizeof(hdr)) != sizeof(hdr) ||
		hdr.magic != HEADS_MAGIC || hdr.version != HEADS_VERSION ||
		hdr.nheads > MAX_HEADS || hdr.npending > MAX_PENDING)
		ereport(FATAL,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("prolly heads file \"%s\" is invalid", HEADS_FILE)));

	hlen = sizeof(HeadEntry) * hdr.nheads;
	plen = sizeof(PendingEntry) * hdr.npending;
	if (read(fd, s->heads, hlen) != hlen ||
		read(fd, s->pending, plen) != plen ||
		read(fd, &file_crc, sizeof(file_crc)) != sizeof(file_crc))
		ereport(FATAL,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("prolly heads file \"%s\" is truncated", HEADS_FILE)));
	CloseTransientFile(fd);

	INIT_CRC32C(crc);
	COMP_CRC32C(crc, &hdr, sizeof(hdr));
	COMP_CRC32C(crc, s->heads, hlen);
	COMP_CRC32C(crc, s->pending, plen);
	FIN_CRC32C(crc);
	if (!EQ_CRC32C(crc, file_crc))
		ereport(FATAL,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("prolly heads file \"%s\" has an incorrect checksum",
						HEADS_FILE)));

	s->nheads = hdr.nheads;
	s->npending = hdr.npending;
	for (int i = 0; i < s->npending; i++)
		s->next_seq = Max(s->next_seq, s->pending[i].seq + 1);
}

static void
write_all(int fd, const void *data, Size len)
{
	errno = 0;
	if (write(fd, data, len) != len)
	{
		if (errno == 0)
			errno = ENOSPC;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write file \"%s\": %m", HEADS_TEMP)));
	}
}

static void
serial_checkpoint(int flags)
{
	SerialShared *s = get_shared();
	HeadsFileHeader hdr;
	HeadEntry  *heads;
	PendingEntry *pending;
	pg_crc32c	crc;
	int			fd;

	/*
	 * On a primary only: during recovery, a transaction whose commit
	 * record is still to be replayed can look finished.
	 */
	if (!RecoveryInProgress() && !(flags & CHECKPOINT_END_OF_RECOVERY))
		sweep_pending(s);

	heads = palloc(sizeof(HeadEntry) * MAX_HEADS);
	pending = palloc(sizeof(PendingEntry) * MAX_PENDING);

	LWLockAcquire(ProllyHeadsLock, LW_SHARED);
	memset(&hdr, 0, sizeof(hdr));
	hdr.magic = HEADS_MAGIC;
	hdr.version = HEADS_VERSION;
	hdr.nheads = s->nheads;
	hdr.npending = s->npending;
	memcpy(heads, s->heads, sizeof(HeadEntry) * s->nheads);
	memcpy(pending, s->pending, sizeof(PendingEntry) * s->npending);
	LWLockRelease(ProllyHeadsLock);

	INIT_CRC32C(crc);
	COMP_CRC32C(crc, &hdr, sizeof(hdr));
	COMP_CRC32C(crc, heads, sizeof(HeadEntry) * hdr.nheads);
	COMP_CRC32C(crc, pending, sizeof(PendingEntry) * hdr.npending);
	FIN_CRC32C(crc);

	fd = OpenTransientFile(HEADS_TEMP, O_WRONLY | O_CREAT | O_TRUNC | PG_BINARY);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m", HEADS_TEMP)));
	write_all(fd, &hdr, sizeof(hdr));
	write_all(fd, heads, sizeof(HeadEntry) * hdr.nheads);
	write_all(fd, pending, sizeof(PendingEntry) * hdr.npending);
	write_all(fd, &crc, sizeof(crc));
	if (pg_fsync(fd) != 0)
		ereport(data_sync_elevel(ERROR),
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m", HEADS_TEMP)));
	if (CloseTransientFile(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", HEADS_TEMP)));
	durable_rename(HEADS_TEMP, HEADS_FILE, ERROR);

	pfree(heads);
	pfree(pending);
}

/* ------------------------------------------------------------------------
 * Callbacks
 * ------------------------------------------------------------------------
 */

static Size
serial_shmem_size(void)
{
	return sizeof(SerialShared);
}

static void
serial_shmem_init(void *area)
{
	SerialShared *s = area;

	memset(s, 0, sizeof(*s));
	s->next_seq = 1;
	read_heads_file(s);
	shared = s;
}

static void
serial_branch_lock(ProllyBranchId branch)
{
	LockProllyBranch(branch, ExclusiveLock);
}

static void
serial_branch_unlock(ProllyBranchId branch)
{
	/*
	 * Nothing: the lock is kept until the transaction ends, so the next
	 * committer can't build on a root that might still be abandoned.
	 */
}

static bool
serial_tip(ProllyBranchId branch, ProllyNodeRef *root)
{
	SerialShared *s = get_shared();
	bool		found = false;

	LWLockAcquire(ProllyHeadsLock, LW_EXCLUSIVE);
	settle_pending(s, MyDatabaseId, branch, true, PG_UINT64_MAX, false);
	for (int i = s->npending - 1; i >= 0; i--)
	{
		PendingEntry *p = &s->pending[i];

		if (p->dboid == MyDatabaseId && p->branch == branch)
		{
			root_ref(&p->root, root);
			found = true;
			break;
		}
	}
	if (!found)
	{
		HeadEntry  *h = find_head(s, MyDatabaseId, branch);

		if (h != NULL)
		{
			root_ref(&h->root, root);
			found = true;
		}
	}
	LWLockRelease(ProllyHeadsLock);
	return found;
}

/* Log a pending entry already added to shared memory. */
static void
log_enqueue(Oid dboid, ProllyBranchId branch, TransactionId xid,
			const ProllyHash *root)
{
	xl_prolly_header hdr;
	xl_serial_enqueue rec;

	hdr.impl_id = SERIAL_IMPL_ID;
	rec.dboid = dboid;
	rec.branch = branch;
	rec.xid = xid;
	rec.hash_len = root->len;
	XLogBeginInsert();
	XLogRegisterData(&hdr, SizeOfProllyHeader);
	XLogRegisterData(&rec, SizeOfSerialEnqueue);
	XLogRegisterData(root->bytes, root->len);
	(void) XLogInsert(RM_PROLLY_COMMIT_ID, XLOG_PROLLY_COMMIT_ENQUEUE);
}

static ProllyCommitTicket
serial_enqueue(ProllyBranchId branch, const ProllyNodeRef *new_root,
			   TransactionId xid)
{
	SerialShared *s = get_shared();
	uint64		seq;

	check_root(new_root);

	/* Into shared memory first, so a checkpoint can't miss it. */
	LWLockAcquire(ProllyHeadsLock, LW_EXCLUSIVE);
	if (find_head(s, MyDatabaseId, branch) == NULL &&
		!branch_has_pending(s, MyDatabaseId, branch))
		reserve_slots(s, 1);
	seq = add_pending(s, MyDatabaseId, branch, xid, &new_root->hash);
	LWLockRelease(ProllyHeadsLock);

	log_enqueue(MyDatabaseId, branch, xid, &new_root->hash);
	return seq;
}

static void
finish_ticket(ProllyCommitTicket ticket, bool committed)
{
	SerialShared *s = get_shared();

	LWLockAcquire(ProllyHeadsLock, LW_EXCLUSIVE);
	for (int i = 0; i < s->npending; i++)
	{
		PendingEntry *p = &s->pending[i];

		if (p->seq == ticket)
		{
			if (committed)
				set_head(s, p->dboid, p->branch, &p->root, p->xid);
			remove_pending(s, i);
			break;
		}
	}

	/*
	 * Not found: a concurrent head() settled it from the commit log
	 * already, which only happens once the commit is recorded.
	 */
	LWLockRelease(ProllyHeadsLock);
}

static void
serial_publish(ProllyCommitTicket ticket)
{
	finish_ticket(ticket, true);
}

static void
serial_abandon(ProllyCommitTicket ticket)
{
	finish_ticket(ticket, false);
}

static bool
serial_head(ProllyBranchId branch, ProllyNodeRef *root)
{
	SerialShared *s = get_shared();
	HeadEntry  *h;
	bool		found = false;

	LWLockAcquire(ProllyHeadsLock, LW_SHARED);
	if (branch_has_pending(s, MyDatabaseId, branch))
	{
		/* Settle anything left by recovery or by a commit in flight. */
		LWLockRelease(ProllyHeadsLock);
		LWLockAcquire(ProllyHeadsLock, LW_EXCLUSIVE);
		settle_pending(s, MyDatabaseId, branch, true, PG_UINT64_MAX, false);
	}
	h = find_head(s, MyDatabaseId, branch);
	if (h != NULL)
	{
		root_ref(&h->root, root);
		found = true;
	}
	LWLockRelease(ProllyHeadsLock);
	return found;
}

/* Does snapshot see xid as committed?  Invalid: seen by everyone. */
static bool
seen_committed(TransactionId xid, Snapshot snapshot)
{
	if (!TransactionIdIsValid(xid))
		return true;
	return !XidInMVCCSnapshot(xid, snapshot) && TransactionIdDidCommit(xid);
}

static bool
serial_snapshot_head(ProllyBranchId branch, Snapshot snapshot,
					 ProllyNodeRef *root)
{
	SerialShared *s = get_shared();
	HeadEntry  *h;
	bool		known = false;

	if (snapshot == NULL || snapshot->snapshot_type != SNAPSHOT_MVCC)
		elog(ERROR, "prolly snapshot_head needs an MVCC snapshot");

	LWLockAcquire(ProllyHeadsLock, LW_SHARED);

	/* Newest first: roots enqueued, then the head, then older heads. */
	for (int i = s->npending - 1; i >= 0; i--)
	{
		PendingEntry *p = &s->pending[i];

		if (p->dboid == MyDatabaseId && p->branch == branch &&
			seen_committed(p->xid, snapshot))
		{
			root_ref(&p->root, root);
			LWLockRelease(ProllyHeadsLock);
			return true;
		}
	}
	h = find_head(s, MyDatabaseId, branch);
	if (h != NULL)
	{
		known = true;
		if (seen_committed(s->head_xids[h - s->heads], snapshot))
		{
			root_ref(&h->root, root);
			LWLockRelease(ProllyHeadsLock);
			return true;
		}
	}
	for (int i = s->nhistory - 1; i >= 0; i--)
	{
		HistoryEntry *e = &s->history[i];

		if (e->dboid != MyDatabaseId || e->branch != branch)
			continue;
		known = true;
		if (seen_committed(e->xid, snapshot))
		{
			bool		has_root = e->has_root;

			if (has_root)
				root_ref(&e->root, root);
			LWLockRelease(ProllyHeadsLock);
			return has_root;
		}
	}
	LWLockRelease(ProllyHeadsLock);

	/*
	 * A branch with no head and no history has never had a root this
	 * snapshot could see.  One that has, with nothing the snapshot sees
	 * left, gave up the root it needed.
	 */
	if (!known)
		return false;
	ereport(ERROR,
			(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
			 errmsg("snapshot too old"),
			 errdetail("The prolly root this snapshot sees was given up to make room for newer ones.")));
	return false;				/* keep compiler quiet */
}

static void finish_ticket(ProllyCommitTicket ticket, bool committed);

static void
copy_xact_callback(XactEvent event, void *arg)
{
	switch (event)
	{
		case XACT_EVENT_COMMIT:
		case XACT_EVENT_ABORT:
			for (int i = 0; i < ncopied; i++)
				finish_ticket(copied_seqs[i], event == XACT_EVENT_COMMIT);
			ncopied = 0;
			break;
		case XACT_EVENT_PREPARE:
			/* Left to be settled through the commit log. */
			ncopied = 0;
			break;
		default:
			break;
	}
}

static void
serial_copy_database(Oid src_dboid, Oid dst_dboid)
{
	SerialShared *s = get_shared();
	TransactionId xid = GetCurrentTransactionId();
	HeadEntry  *heads;
	int			n = 0;

	heads = palloc(sizeof(HeadEntry) * MAX_HEADS);
	LWLockAcquire(ProllyHeadsLock, LW_EXCLUSIVE);
	settle_database(s, src_dboid);
	settle_database(s, dst_dboid);
	if (database_has_pending(s, src_dboid))
	{
		LWLockRelease(ProllyHeadsLock);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_IN_USE),
				 errmsg("source database %u has prolly commits in progress",
						src_dboid)));
	}
	for (int i = 0; i < s->nheads; i++)
	{
		if (s->heads[i].dboid == dst_dboid)
		{
			LWLockRelease(ProllyHeadsLock);
			elog(ERROR, "new database %u already has prolly branch heads",
				 dst_dboid);
		}
	}
	if (database_has_pending(s, dst_dboid))
	{
		LWLockRelease(ProllyHeadsLock);
		elog(ERROR, "new database %u already has prolly commits in progress",
			 dst_dboid);
	}
	for (int i = 0; i < s->nheads; i++)
	{
		if (s->heads[i].dboid == src_dboid)
			heads[n++] = s->heads[i];
	}
	if (s->npending + n > MAX_PENDING)
	{
		LWLockRelease(ProllyHeadsLock);
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("too many prolly commits in progress (limit %d)",
						MAX_PENDING)));
	}
	reserve_slots(s, n);
	if (ncopied + n > MAX_HEADS)
	{
		LWLockRelease(ProllyHeadsLock);
		elog(ERROR, "too many prolly branch heads copied in one transaction");
	}
	if (!copy_callback_registered)
	{
		RegisterXactCallback(copy_xact_callback, NULL);
		copy_callback_registered = true;
	}
	for (int i = 0; i < n; i++)
		copied_seqs[ncopied++] = add_pending(s, dst_dboid, heads[i].branch,
											 xid, &heads[i].root);
	LWLockRelease(ProllyHeadsLock);

	for (int i = 0; i < n; i++)
		log_enqueue(dst_dboid, heads[i].branch, xid, &heads[i].root);
	pfree(heads);
}

static void
serial_drop_database(Oid dboid)
{
	SerialShared *s = get_shared();
	xl_prolly_header hdr;
	xl_serial_drop_database rec;

	LWLockAcquire(ProllyHeadsLock, LW_EXCLUSIVE);
	forget_database(s, dboid);
	LWLockRelease(ProllyHeadsLock);

	hdr.impl_id = SERIAL_IMPL_ID;
	rec.dboid = dboid;
	XLogBeginInsert();
	XLogRegisterData(&hdr, SizeOfProllyHeader);
	XLogRegisterData(&rec, sizeof(rec));
	(void) XLogInsert(RM_PROLLY_COMMIT_ID, XLOG_PROLLY_COMMIT_DROP_DATABASE);
}

static void
serial_usage(int *nheads, int *npending)
{
	SerialShared *s = get_shared();

	LWLockAcquire(ProllyHeadsLock, LW_SHARED);
	*nheads = s->nheads;
	*npending = s->npending;
	LWLockRelease(ProllyHeadsLock);
}

static void
serial_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & PROLLY_XLOG_TYPE_MASK;
	SerialShared *s = get_shared();

	switch (info)
	{
		case XLOG_PROLLY_COMMIT_ENQUEUE:
			{
				char	   *data = XLogRecGetData(record) + SizeOfProllyHeader;
				Size		len = XLogRecGetDataLen(record) - SizeOfProllyHeader;
				xl_serial_enqueue rec;
				ProllyHash	root;

				if (len < SizeOfSerialEnqueue)
					elog(PANIC, "prolly commit record is too short");
				memcpy(&rec, data, SizeOfSerialEnqueue);
				if (rec.hash_len == 0 || rec.hash_len > PROLLY_HASH_MAX_LEN ||
					len != SizeOfSerialEnqueue + rec.hash_len)
					elog(PANIC, "prolly commit record has an invalid root");
				memset(&root, 0, sizeof(root));
				root.len = rec.hash_len;
				memcpy(root.bytes, data + SizeOfSerialEnqueue, rec.hash_len);

				LWLockAcquire(ProllyHeadsLock, LW_EXCLUSIVE);

				/*
				 * Commits to a branch are serialized, so every earlier
				 * transaction on it has finished by now.  Settled before
				 * adding, so a branch never holds more than one entry here.
				 */
				settle_pending(s, rec.dboid, rec.branch, false, PG_UINT64_MAX,
							   false);
				if (s->npending >= MAX_PENDING)
					settle_replayed(s);
				(void) add_pending(s, rec.dboid, rec.branch, rec.xid, &root);
				LWLockRelease(ProllyHeadsLock);
				break;
			}
		case XLOG_PROLLY_COMMIT_DROP_DATABASE:
			{
				xl_serial_drop_database rec;

				if (XLogRecGetDataLen(record) != SizeOfProllyHeader + sizeof(rec))
					elog(PANIC, "prolly drop database record has the wrong length");
				memcpy(&rec, XLogRecGetData(record) + SizeOfProllyHeader,
					   sizeof(rec));
				LWLockAcquire(ProllyHeadsLock, LW_EXCLUSIVE);
				forget_database(s, rec.dboid);
				LWLockRelease(ProllyHeadsLock);
				break;
			}
		default:
			elog(PANIC, "prolly commit pipeline \"serial\": unknown record type 0x%02X",
				 info);
	}
}

const ProllyCommitRoutine prolly_commit_serial = {
	.info = {
		.iface = PROLLY_IFACE_COMMIT_PIPELINE,
		.impl_id = SERIAL_IMPL_ID,
		.format_version = 1,
		.name = "serial",
		.description = "one committer per branch, holding the branch until its "
		"transaction ends",
	},
	.shmem_size = serial_shmem_size,
	.shmem_init = serial_shmem_init,
	.branch_lock = serial_branch_lock,
	.tip = serial_tip,
	.enqueue = serial_enqueue,
	.branch_unlock = serial_branch_unlock,
	.publish = serial_publish,
	.abandon = serial_abandon,
	.head = serial_head,
	.snapshot_head = serial_snapshot_head,
	.copy_database = serial_copy_database,
	.drop_database = serial_drop_database,
	.usage = serial_usage,
	.checkpoint = serial_checkpoint,
	.redo = serial_redo,
};
