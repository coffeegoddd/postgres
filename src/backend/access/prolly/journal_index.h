/*-------------------------------------------------------------------------
 *
 * journal_index.h
 *	  The chunk journal's index, shared: where each chunk of a database's
 *	  journal segments is, and how far the segments have been read to say.
 *
 * Private to the "journal" chunk store implementation (chunkstore_journal.c).
 *
 * src/backend/access/prolly/journal_index.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef JOURNAL_INDEX_H
#define JOURNAL_INDEX_H

#include "access/prolly_types.h"

typedef struct JournalIndex JournalIndex;

/* the longest hash the index keys chunks by */
#define JINDEX_MAX_HASH		32

/* a range of a segment reserved for an append, not written when read */
typedef struct JournalHole
{
	uint64		offset;
	uint64		len;
} JournalHole;

/*
 * A segment, as the backends reading it have together: read (and its
 * chunks indexed) up to scanned, but for the holes before that; the PADs
 * seen, which may become ROOTs.
 */
typedef struct JournalSegScan
{
	uint64		id;
	uint64		scanned;
	int			npads;
	int			nholes;
	/* then (jindex_pads(), jindex_holes()) capacity of each */
} JournalSegScan;

/*
 * What the index's readers share besides its chunks, under its lock: the
 * segments the manifest last read named, and the root their ROOT records
 * (or the manifest) give.
 */
typedef struct JournalScan
{
	uint64		manifests_seen; /* the manifest count when last read */
	uint32		gc_generation;
	ProllyHash	root;
	uint64		root_segment;	/* where the last ROOT record seen is; 0, 0: */
	uint64		root_offset;	/* the manifest's root, or none */
	int			nsegments;
} JournalScan;

/*
 * The index of database dboid's journal (in tablespace spcoid), attached;
 * made if it has none (*made).
 */
extern JournalIndex *jindex_attach(Oid dboid, Oid spcoid, int capacity,
								   bool *made);

/*
 * The index as a file in the database's directory, a cache of it: written
 * at checkpoints (every index changed since), and read by the first to use
 * an index after the server starts, for the segments the manifest names
 * (ids), if it is of this format; the rest is read from the journal.
 */
#define JINDEX_FILE			"prolly_index"
extern void jindex_checkpoint(const char *format, int format_len);
extern bool jindex_load(JournalIndex *ix, const char *format, int format_len,
						const uint64 *ids, const uint64 *sizes, int n,
						uint64 start);

/* Forget database dboid's index (dropped: an OID used again starts anew). */
extern void jindex_forget(Oid dboid);

/* The scan state, under the index's lock (exclusive: to change it). */
extern void jindex_lock(JournalIndex *ix, bool exclusive);
extern void jindex_unlock(JournalIndex *ix);
extern JournalScan *jindex_scan(JournalIndex *ix);
extern JournalSegScan *jindex_segment(JournalIndex *ix, int i);
extern JournalSegScan *jindex_segment_of(JournalIndex *ix, uint64 id);
extern uint64 *jindex_pads(JournalIndex *ix, JournalSegScan *g);
extern JournalHole *jindex_holes(JournalIndex *ix, JournalSegScan *g);
extern int	jindex_capacity(JournalIndex *ix);

/*
 * Name these segments, oldest first (exclusive lock held): one already
 * named keeps its state, a new one is read from its start (header bytes
 * in); the chunks of one no longer named are forgotten.
 */
extern void jindex_set_segments(JournalIndex *ix, const uint64 *ids, int n,
								uint64 start);

/* Chunks. */
extern void jindex_add(JournalIndex *ix, const ProllyHash *hash,
					   uint64 segment, uint64 offset, uint32 len,
					   TransactionId xid);
extern bool jindex_find(JournalIndex *ix, const ProllyHash *hash, bool wait,
						uint64 *segment, uint64 *offset, uint32 *len);
extern bool jindex_has(JournalIndex *ix, const ProllyHash *hash);

/* Each copy of each chunk, by the segment it is in. */
extern void jindex_visit(JournalIndex *ix,
						 void (*visit) (const ProllyHash *hash, uint64 segment,
										int ordinal, void *arg),
						 void *arg);

#endif							/* JOURNAL_INDEX_H */
