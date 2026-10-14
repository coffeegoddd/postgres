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
#include "access/xloginsert.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "port/pg_crc32c.h"
#include "storage/fd.h"
#include "storage/lmgr.h"
#include "storage/lwlock.h"
#include "storage/procarray.h"

#define SERIAL_IMPL_ID	1

/* ProllyCommit record types (high bits of info) */
#define XLOG_PROLLY_COMMIT_ENQUEUE	0x10

#define MAX_HEADS		1024
#define MAX_PENDING		1024

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

typedef struct SerialShared
{
	uint64		next_seq;
	int			nheads;
	int			npending;
	HeadEntry	heads[MAX_HEADS];
	PendingEntry pending[MAX_PENDING];	/* in seq order */
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

static SerialShared *shared = NULL;

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

static void
set_head(SerialShared *s, Oid dboid, ProllyBranchId branch,
		 const ProllyHash *root)
{
	HeadEntry  *h = find_head(s, dboid, branch);

	if (h == NULL)
	{
		if (s->nheads >= MAX_HEADS)
			ereport(PANIC,
					(errmsg("too many prolly branch heads (limit %d)", MAX_HEADS)));
		h = &s->heads[s->nheads++];
		h->dboid = dboid;
		h->branch = branch;
	}
	h->root = *root;
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
 * Settle the branch's pending entries whose transactions are known to have
 * finished, oldest first: a committed one's root becomes the head, others
 * are dropped.  With check_running, stops at the first transaction still
 * running; redo passes false, because there every entry below below_seq
 * has finished (commits to a branch are serialized).
 */
static void
settle_pending(SerialShared *s, Oid dboid, ProllyBranchId branch,
			   bool check_running, uint64 below_seq)
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
		if (TransactionIdDidCommit(p->xid))
			set_head(s, dboid, branch, &p->root);
		else if (check_running && TransactionIdIsInProgress(p->xid))
			break;
		remove_pending(s, i);
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
	settle_pending(s, MyDatabaseId, branch, true, PG_UINT64_MAX);
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

static ProllyCommitTicket
serial_enqueue(ProllyBranchId branch, const ProllyNodeRef *new_root)
{
	SerialShared *s = get_shared();
	TransactionId xid = GetCurrentTransactionId();
	xl_prolly_header hdr;
	xl_serial_enqueue rec;
	uint64		seq;

	check_root(new_root);

	/* Into shared memory first, so a checkpoint can't miss it. */
	LWLockAcquire(ProllyHeadsLock, LW_EXCLUSIVE);
	seq = add_pending(s, MyDatabaseId, branch, xid, &new_root->hash);
	LWLockRelease(ProllyHeadsLock);

	hdr.impl_id = SERIAL_IMPL_ID;
	rec.dboid = MyDatabaseId;
	rec.branch = branch;
	rec.xid = xid;
	rec.hash_len = new_root->hash.len;
	XLogBeginInsert();
	XLogRegisterData(&hdr, SizeOfProllyHeader);
	XLogRegisterData(&rec, SizeOfSerialEnqueue);
	XLogRegisterData(new_root->hash.bytes, new_root->hash.len);
	(void) XLogInsert(RM_PROLLY_COMMIT_ID, XLOG_PROLLY_COMMIT_ENQUEUE);

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
				set_head(s, p->dboid, p->branch, &p->root);
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
		settle_pending(s, MyDatabaseId, branch, true, PG_UINT64_MAX);
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
				uint64		seq;

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
				seq = add_pending(s, rec.dboid, rec.branch, rec.xid, &root);

				/*
				 * Commits to a branch are serialized, so every earlier
				 * transaction on it has finished by now.
				 */
				settle_pending(s, rec.dboid, rec.branch, false, seq);
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
	.checkpoint = serial_checkpoint,
	.redo = serial_redo,
};
