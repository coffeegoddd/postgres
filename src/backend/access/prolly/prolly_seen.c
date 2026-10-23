/*-------------------------------------------------------------------------
 *
 * prolly_seen.c
 *	  What a walk has seen: a map of hashes held to a budget of memory,
 *	  spilling to temporary files past it (prolly_seen.h).
 *
 * Entries are kept in a table in memory, an array of slots of its own
 * (open addressing), so what it takes is what it has room for.  When it
 * has no more room, its entries are sorted in place and written to a
 * temporary file of their own as a run (each record the hash, zero-padded
 * as a key, then the value), the table emptied, and a Bloom filter of the
 * run kept in memory.
 *
 * Runs are merged as a log-structured tree's are: a new run is of level 0,
 * and when the newest MERGE_FANIN runs are of one level, they are merged,
 * read in order a buffer at a time, into one run of the next level, their
 * files closed.  A key is in one run only (a hash is entered once), so a
 * merge is a union.  So there are at most MERGE_FANIN - 1 runs a level,
 * and about log4 of the hashes spilled over the table's capacity levels:
 * a lookup checks a number of filters logarithmic in the map's size, and
 * each record is written once a level.
 *
 * The filters share half the budget.  Each has a power of two of bits, so
 * it can be halved by folding its upper half onto its lower (a bit of the
 * half the OR of the two it stands for), which keeps every hash it held
 * and loses only precision.  A new filter is sized for ten bits a hash, or
 * fewer once the share would hold fewer for every hash spilled; to make
 * room for it, the filter with the most bits a hash is folded until the
 * filters fit their share.  So the filters' memory is bounded and only
 * their false positives rise as the map grows; a filter rules a run out,
 * never in, and a run it doesn't rule out is searched.
 *
 * A run is searched by interpolation: its keys are content hashes, spread
 * evenly, so a key's place in the run is about where its first bytes put
 * it between the run's first and last keys.  A read takes the block of
 * records around that place into the map's one window, and the next few
 * probes mostly fall in it.  Every other probe after the first few halves
 * the range instead, so a run of unevenly spread keys is still searched in
 * logarithmic reads.  The run's first and last keys, kept, rule out a key
 * outside them without a read.
 *
 * The rest of the budget is the table's, less the window and a write
 * buffer, or a quarter of the budget if they leave less (so before it
 * spills, nearly all of it); the table is
 * freed while runs merge, so a merge's buffers take its place.  Those
 * buffers are a block each, or a 32nd of a small budget, so a merge's
 * five fit in the quarter.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/prolly_seen.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/prolly_seen.h"
#include "common/hashfn.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "utils/memutils.h"
#include "utils/wait_event.h"

#define FILTER_BITS		10		/* a hash's bits in a filter, at most */
#define FILTER_MAX_PROBES 7
#define FILTER_MIN_BITS 64		/* a filter folds no smaller */
#define FILTER_SHARE	2		/* the filters take at most 1/FILTER_SHARE of
								 * the budget */
#define TABLE_FLOOR		4		/* the table has at least 1/TABLE_FLOOR */
#define TABLE_MIN_SLOTS 128		/* the table's least size */
#define MERGE_FANIN		4		/* runs of a level merged into one */
#define IO_SIZE			BLCKSZ	/* a buffer of a file's, at most */
#define IO_SHARE		32		/* a buffer is at most 1/IO_SHARE of the
								 * budget */

typedef struct Run
{
	File		file;			/* its records, sorted, alone */
	uint64		n;
	int			level;
	uint64		first;			/* its first and last keys' leading bytes */
	uint64		last;
	uint64		nbits;			/* a power of two */
	int			nprobes;
	uint8	   *filter;
} Run;

/* A run's file written in order, through a buffer. */
typedef struct RunWriter
{
	File		file;
	off_t		off;
	char	   *buf;
	Size		len;
} RunWriter;

/* A run's file read in order, through a buffer. */
typedef struct RunReader
{
	File		file;
	off_t		off;
	off_t		end;
	char	   *buf;
	Size		len;
	Size		pos;
} RunReader;

struct ProllySeen
{
	MemoryContext cxt;
	Size		value_size;
	Size		entry_size;		/* in memory: key, then value */
	Size		record_size;	/* in a file: the same */
	Size		io_records;		/* records a buffer holds */
	Size		budget;
	Size		filter_budget;	/* bytes */
	char	   *table;			/* slots of entry_size */
	uint64		slots;
	uint64		in_table;
	Run		   *runs;			/* oldest first */
	int			nruns;
	int			maxruns;		/* allocated */
	uint64		spilled;		/* records in runs */
	Size		filter_bytes;
	char	   *window;			/* records of a run read for lookups */
	File		window_file;	/* the run's, or -1 */
	uint64		window_first;	/* the first record's place in the run */
	uint64		window_n;
	char	   *record;			/* the record a lookup found */
	ProllySeenStats stats;
};

static ProllyHash
key_of(const ProllyHash *hash)
{
	ProllyHash	key;

	memset(&key, 0, sizeof(key));
	key.len = hash->len;
	memcpy(key.bytes, hash->bytes, hash->len);
	return key;
}

/*
 * A key's (or record's) first eight bytes, as a number in key order: its
 * place among keys spread evenly.
 */
static uint64
leading(const void *key)
{
	const uint8 *b = key;
	uint64		v = 0;

	for (int i = 0; i < 8; i++)
		v = (v << 8) | b[i];
	return v;
}

/*
 * The table: entries in slots, a slot whose key is of length 0 empty (no
 * hash is), a hash in the first empty slot from where its bytes put it.
 */
static char *
slot_at(ProllySeen *s, uint64 i)
{
	return s->table + i * s->entry_size;
}

static uint64
slot_of(ProllySeen *s, const ProllyHash *key)
{
	uint64		h;

	memcpy(&h, key->bytes, sizeof(h));
	return murmurhash64(h) % s->slots;
}

static char *
table_find(ProllySeen *s, const ProllyHash *key)
{
	if (s->in_table == 0)
		return NULL;
	for (uint64 i = slot_of(s, key);; i = (i + 1 == s->slots ? 0 : i + 1))
	{
		char	   *slot = slot_at(s, i);

		if (((ProllyHash *) slot)->len == 0)
			return NULL;
		if (memcmp(slot, key, sizeof(ProllyHash)) == 0)
			return slot;
	}
}

/* A slot for key, which isn't in the table, which has an empty one. */
static char *
table_slot(ProllySeen *s, const ProllyHash *key)
{
	for (uint64 i = slot_of(s, key);; i = (i + 1 == s->slots ? 0 : i + 1))
	{
		char	   *slot = slot_at(s, i);

		if (((ProllyHash *) slot)->len == 0)
			return slot;
	}
}

/* The table made of the given size, its entries moved into it. */
static void
table_resize(ProllySeen *s, uint64 slots)
{
	char	   *old = s->table;
	uint64		nold = s->slots;

	s->table = MemoryContextAllocHuge(s->cxt, slots * s->entry_size);
	memset(s->table, 0, slots * s->entry_size);
	s->slots = slots;
	for (uint64 i = 0; i < nold; i++)
	{
		char	   *slot = old + i * s->entry_size;

		if (((ProllyHash *) slot)->len != 0)
			memcpy(table_slot(s, (ProllyHash *) slot), slot, s->entry_size);
	}
	if (old)
		pfree(old);
}

/*
 * The table's room, in slots: what the rest of the map (its filters, the
 * window, the runs' descriptions) leaves of the budget, less a write
 * buffer, room for the descriptions to double and for blocks' headers,
 * each slot with room for its share of the filter its run will have (up
 * to twice ten bits an entry, three quarters of the slots full), or a
 * quarter of the budget if that leaves less.  The filters grow into the
 * table's room run by run, to half the budget, and the table, made again
 * after each run, gives it up.
 */
static uint64
table_room(ProllySeen *s)
{
	Size		rest = MemoryContextMemAllocated(s->cxt, false) -
		s->slots * s->entry_size + s->io_records * s->record_size +
		Max(s->maxruns, 8) * sizeof(Run) + 1024;
	Size		room = s->budget > rest ? s->budget - rest : 0;

	return Max(room / (s->entry_size + (2 * FILTER_BITS * 3 / 4 + 7) / 8),
			   s->budget / TABLE_FLOOR / s->entry_size);
}

/*
 * Whether the table has room for another entry: it is filled to three
 * quarters at most, and grows, to twice its size or what fits its room
 * beside it (both are held as it grows), while that is a quarter more.
 */
static bool
table_has_room(ProllySeen *s)
{
	uint64		room;
	uint64		grown;

	if (s->in_table + 1 <= s->slots / 4 * 3)
		return true;
	room = table_room(s);
	grown = room > s->slots ? Min(s->slots * 2, room - s->slots) : 0;
	if (grown < s->slots + s->slots / 4)
		return false;
	table_resize(s, grown);
	return true;
}

ProllySeen *
prolly_seen_create(const char *name, Size value_size, Size budget)
{
	ProllySeen *s = palloc0(sizeof(ProllySeen));

	/*
	 * (blocks of 1kB, so that all but the smallest pieces, filters among
	 * them, are blocks of their own, given back as they are freed: what the
	 * context holds is what the map holds)
	 */
	s->cxt = AllocSetContextCreate(CurrentMemoryContext, "prolly walk seen",
								   0, 1024, 1024);
	MemoryContextCopyAndSetIdentifier(s->cxt, name);
	s->value_size = value_size;
	s->entry_size = MAXALIGN(sizeof(ProllyHash) + value_size);
	s->record_size = sizeof(ProllyHash) + value_size;
	if (budget == 0)
		budget = (Size) maintenance_work_mem * 1024;
	s->io_records = Max(Min(IO_SIZE, budget / IO_SHARE) / s->record_size, 1);
	s->budget = budget;
	s->filter_budget = budget / FILTER_SHARE;
	table_resize(s, TABLE_MIN_SLOTS);
	s->window = MemoryContextAlloc(s->cxt, s->io_records * s->record_size);
	s->window_file = -1;
	s->record = MemoryContextAlloc(s->cxt, s->record_size);
	s->stats.budget = budget;
	s->stats.filter_budget = s->filter_budget;
	return s;
}

/* The filter's probes for key: double hashing of its first sixteen bytes. */
static void
probes(const ProllyHash *key, uint64 nbits, int nprobes, uint64 *out)
{
	uint64		h1;
	uint64		h2;

	memcpy(&h1, key->bytes, sizeof(h1));
	memcpy(&h2, key->bytes + 8, sizeof(h2));
	h2 |= 1;
	for (int i = 0; i < nprobes; i++)
		out[i] = (h1 + (uint64) i * h2) & (nbits - 1);
}

static void
filter_add(Run *r, const ProllyHash *key)
{
	uint64		p[FILTER_MAX_PROBES];

	probes(key, r->nbits, r->nprobes, p);
	for (int j = 0; j < r->nprobes; j++)
		r->filter[p[j] / 8] |= 1 << (p[j] % 8);
}

static bool
filter_may_hold(const Run *r, const ProllyHash *key)
{
	uint64		p[FILTER_MAX_PROBES];

	probes(key, r->nbits, r->nprobes, p);
	for (int j = 0; j < r->nprobes; j++)
		if (!(r->filter[p[j] / 8] & (1 << (p[j] % 8))))
			return false;
	return true;
}

/*
 * The filters folded, those of the most bits a hash first, until they and
 * reserve bytes more fit their share (or are as small as they fold).
 */
static void
filters_fit(ProllySeen *s, Size reserve)
{
	while (s->filter_bytes + reserve > s->filter_budget)
	{
		Run		   *most = NULL;
		uint64		half;

		for (int i = 0; i < s->nruns; i++)
		{
			Run		   *r = &s->runs[i];

			if (r->filter == NULL || r->nbits <= FILTER_MIN_BITS)
				continue;
			if (most == NULL ||
				(double) r->nbits / r->n > (double) most->nbits / most->n)
				most = r;
		}
		if (most == NULL)
			break;
		half = most->nbits / 16;
		for (uint64 b = 0; b < half; b++)
			most->filter[b] |= most->filter[b + half];
		most->filter = repalloc(most->filter, half);
		most->nbits /= 2;
		s->filter_bytes -= half;
		s->stats.filter_folds++;
	}
}

/*
 * A new run's filter, empty, for n hashes: ten bits a hash, or the bits a
 * hash the filters' share leaves for every hash spilled with these, the
 * power of two nearest above (no more than the share); probes the best
 * number for that.  The others are folded first to make room for it.
 */
static void
filter_make(ProllySeen *s, Run *r, uint64 n)
{
	double		per = Min((double) FILTER_BITS,
						  (double) s->filter_budget * 8 /
						  (double) Max(s->spilled, 1));
	uint64		want = (uint64) Max(per * n, (double) FILTER_MIN_BITS);
	uint64		nbits = FILTER_MIN_BITS;

	while (nbits < want)
		nbits <<= 1;
	while (nbits > FILTER_MIN_BITS && nbits / 8 > s->filter_budget)
		nbits >>= 1;
	filters_fit(s, nbits / 8);
	r->nbits = nbits;
	r->nprobes = (int) rint((double) nbits / Max(n, 1) * M_LN2);
	r->nprobes = Max(Min(r->nprobes, FILTER_MAX_PROBES), 1);
	r->filter = MemoryContextAllocZero(s->cxt, nbits / 8);
	s->filter_bytes += nbits / 8;
}

/* Memory now taken, the most taken so far. */
static void
note_memory(ProllySeen *s)
{
	Size		now = MemoryContextMemAllocated(s->cxt, true);

	s->stats.memory_peak = Max(s->stats.memory_peak, now);
	s->stats.filter_bytes = s->filter_bytes;
	s->stats.filter_bytes_peak = Max(s->stats.filter_bytes_peak,
									 s->filter_bytes);
}

static void
file_read(File file, void *buf, Size len, off_t at)
{
	ssize_t		got = FileRead(file, buf, len, at, WAIT_EVENT_BUFFILE_READ);

	if (got < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read from a walk's temporary file: %m")));
	if ((Size) got != len)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("could not read from a walk's temporary file: read only %zd of %zu bytes",
						got, len)));
}

static void
file_write(File file, const void *buf, Size len, off_t at)
{
	ssize_t		put = FileWrite(file, buf, len, at, WAIT_EVENT_BUFFILE_WRITE);

	if ((Size) put != len)
	{
		/* (a short write without an error is out of space) */
		if (put >= 0)
			errno = ENOSPC;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write to a walk's temporary file: %m")));
	}
}

static void
writer_start(ProllySeen *s, RunWriter *w)
{
	w->file = OpenTemporaryFile(false);
	w->off = 0;
	w->len = 0;
	w->buf = MemoryContextAlloc(s->cxt, s->io_records * s->record_size);
}

static void
writer_put(ProllySeen *s, RunWriter *w, const void *record)
{
	if (w->len == s->io_records * s->record_size)
	{
		file_write(w->file, w->buf, w->len, w->off);
		w->off += w->len;
		w->len = 0;
	}
	memcpy(w->buf + w->len, record, s->record_size);
	w->len += s->record_size;
}

/* The writer's file, written to its end. */
static File
writer_end(RunWriter *w)
{
	if (w->len > 0)
		file_write(w->file, w->buf, w->len, w->off);
	pfree(w->buf);
	return w->file;
}

/* A reader of run r from its start, with a buffer of its own. */
static void
reader_start(ProllySeen *s, RunReader *rd, const Run *r)
{
	rd->file = r->file;
	rd->off = 0;
	rd->end = (off_t) (r->n * s->record_size);
	rd->buf = MemoryContextAlloc(s->cxt, s->io_records * s->record_size);
	rd->len = 0;
	rd->pos = 0;
}

/* The reader's next record, copied to out; false at the run's end. */
static bool
reader_next(ProllySeen *s, RunReader *rd, char *out)
{
	if (rd->pos == rd->len)
	{
		if (rd->off >= rd->end)
			return false;
		rd->len = Min(s->io_records * s->record_size,
					  (Size) (rd->end - rd->off));
		file_read(rd->file, rd->buf, rd->len, rd->off);
		rd->off += rd->len;
		rd->pos = 0;
	}
	memcpy(out, rd->buf + rd->pos, s->record_size);
	rd->pos += s->record_size;
	return true;
}

static Run *
run_add(ProllySeen *s)
{
	if (s->nruns == s->maxruns)
	{
		s->maxruns = Max(s->maxruns * 2, 8);
		s->runs = s->runs ?
			repalloc(s->runs, sizeof(Run) * s->maxruns) :
			MemoryContextAlloc(s->cxt, sizeof(Run) * s->maxruns);
	}
	memset(&s->runs[s->nruns], 0, sizeof(Run));
	return &s->runs[s->nruns++];
}

static void
run_close(ProllySeen *s, Run *r)
{
	if (s->window_file == r->file)
		s->window_file = -1;
	FileClose(r->file);
}

static int
entry_cmp(const void *a, const void *b)
{
	return memcmp(a, b, sizeof(ProllyHash));
}

/*
 * While the newest MERGE_FANIN runs are of one level, they merged into one
 * of the next.
 */
static void
merge_newest(ProllySeen *s)
{
	while (s->nruns >= MERGE_FANIN)
	{
		Run		   *in = &s->runs[s->nruns - MERGE_FANIN];
		int			level = in[0].level;
		RunReader	rd[MERGE_FANIN];
		bool		more[MERGE_FANIN];
		char	   *heads;
		RunWriter	w;
		Run			out;
		uint64		n = 0;
		uint64		count = 0;

		for (int i = 1; i < MERGE_FANIN; i++)
			if (in[i].level != level)
				return;

		/* (lookups wait for the merge: the inputs' filters go first) */
		for (int i = 0; i < MERGE_FANIN; i++)
		{
			n += in[i].n;
			pfree(in[i].filter);
			in[i].filter = NULL;
			s->filter_bytes -= in[i].nbits / 8;
		}
		memset(&out, 0, sizeof(out));
		out.n = n;
		out.level = level + 1;
		filter_make(s, &out, n);

		heads = MemoryContextAlloc(s->cxt, MERGE_FANIN * s->record_size);
		for (int i = 0; i < MERGE_FANIN; i++)
		{
			reader_start(s, &rd[i], &in[i]);
			more[i] = reader_next(s, &rd[i], heads + i * s->record_size);
		}
		writer_start(s, &w);
		note_memory(s);
		for (;;)
		{
			int			min = -1;
			char	   *rec;

			for (int i = 0; i < MERGE_FANIN; i++)
				if (more[i] &&
					(min < 0 ||
					 memcmp(heads + i * s->record_size,
							heads + min * s->record_size,
							sizeof(ProllyHash)) < 0))
					min = i;
			if (min < 0)
				break;
			if ((count++ & 1023) == 0)
				CHECK_FOR_INTERRUPTS();
			rec = heads + min * s->record_size;
			if (count == 1)
				out.first = leading(rec);
			out.last = leading(rec);
			writer_put(s, &w, rec);
			filter_add(&out, (ProllyHash *) rec);
			more[min] = reader_next(s, &rd[min], rec);
		}
		Assert(count == n);
		out.file = writer_end(&w);
		pfree(heads);

		for (int i = 0; i < MERGE_FANIN; i++)
		{
			pfree(rd[i].buf);
			run_close(s, &in[i]);
		}
		s->nruns -= MERGE_FANIN;
		*run_add(s) = out;
		s->stats.merges++;
		s->stats.records_merged += n;
		s->stats.temp_bytes_peak = Max(s->stats.temp_bytes_peak,
									   s->stats.temp_bytes +
									   n * s->record_size);
	}
}

/* The table's entries written as a run, the table emptied. */
static void
write_run(ProllySeen *s)
{
	uint64		n = 0;
	uint64		slots = s->slots;
	RunWriter	w;
	Run		   *run;

	/* the entries moved to the table's front, sorted there */
	for (uint64 i = 0; i < s->slots; i++)
	{
		char	   *slot = slot_at(s, i);

		if (((ProllyHash *) slot)->len == 0)
			continue;
		if (i != n)
			memcpy(slot_at(s, n), slot, s->entry_size);
		n++;
	}
	Assert(n == s->in_table && n > 0);
	qsort(s->table, n, s->entry_size, entry_cmp);

	s->spilled += n;
	run = run_add(s);
	run->n = n;
	run->level = 0;
	run->first = leading(slot_at(s, 0));
	run->last = leading(slot_at(s, n - 1));
	filter_make(s, run, n);
	writer_start(s, &w);
	for (uint64 i = 0; i < n; i++)
	{
		if ((i & 1023) == 0)
			CHECK_FOR_INTERRUPTS();
		writer_put(s, &w, slot_at(s, i));
		filter_add(run, (ProllyHash *) slot_at(s, i));
	}
	note_memory(s);
	run->file = writer_end(&w);
	s->stats.runs_written++;
	s->stats.temp_bytes += n * s->record_size;
	s->stats.temp_bytes_peak = Max(s->stats.temp_bytes_peak,
								   s->stats.temp_bytes);

	/* the table freed while runs merge, then made again, empty */
	pfree(s->table);
	s->table = NULL;
	s->slots = 0;
	s->in_table = 0;
	merge_newest(s);
	slots = Max(Min(slots, table_room(s)), TABLE_MIN_SLOTS);
	table_resize(s, slots);

	s->stats.runs = s->nruns;
	s->stats.runs_peak = Max(s->stats.runs_peak, s->nruns);
	note_memory(s);
}

/*
 * Record i of run r, read into the window (with those around it, unless
 * it is there already).
 */
static char *
run_record(ProllySeen *s, const Run *r, uint64 i)
{
	uint64		first;

	s->stats.record_reads++;
	if (s->window_file != r->file || i < s->window_first ||
		i >= s->window_first + s->window_n)
	{
		first = i > s->io_records / 2 ? i - s->io_records / 2 : 0;
		if (first + s->io_records > r->n)
			first = r->n > s->io_records ? r->n - s->io_records : 0;
		s->window_file = -1;
		s->window_n = Min(s->io_records, r->n - first);
		file_read(r->file, s->window, s->window_n * s->record_size,
				  (off_t) (first * s->record_size));
		s->window_file = r->file;
		s->window_first = first;
		s->stats.block_reads++;
	}
	return s->window + (i - s->window_first) * s->record_size;
}

/* Whether key is in run r: if so, its place, and its record in s->record. */
static bool
run_find(ProllySeen *s, Run *r, const ProllyHash *key, uint64 *place)
{
	uint64		lo = 0;
	uint64		hi = r->n;
	uint64		lo_v = r->first;
	uint64		hi_v = r->last;
	uint64		v = leading(key);

	s->stats.filter_checks++;
	if (v < r->first || v > r->last || !filter_may_hold(r, key))
		return false;
	s->stats.run_searches++;
	for (int step = 0; lo < hi; step++)
	{
		uint64		mid;
		char	   *rec;
		int			c;

		/* where v falls between the range's bounds, or its middle */
		if ((step < 4 || step % 2 == 0) && hi_v > lo_v &&
			v >= lo_v && v <= hi_v)
		{
			double		frac = (double) (v - lo_v) / (double) (hi_v - lo_v);

			mid = lo + (uint64) (frac * (double) (hi - lo - 1));
			mid = Min(mid, hi - 1);
		}
		else
			mid = lo + (hi - lo) / 2;

		rec = run_record(s, r, mid);
		c = memcmp(key, rec, sizeof(ProllyHash));
		if (c == 0)
		{
			memcpy(s->record, rec, s->record_size);
			*place = mid;
			return true;
		}
		if (c < 0)
		{
			hi = mid;
			hi_v = leading(rec);
		}
		else
		{
			lo = mid + 1;
			lo_v = leading(rec);
		}
	}
	s->stats.false_positives++;
	return false;
}

/*
 * Where key is: in the table (*entry), or a run's record (*run, at *place),
 * or not.
 */
static bool
locate(ProllySeen *s, const ProllyHash *key, char **entry, Run **run,
	   uint64 *place)
{
	int			checked = 0;

	s->stats.lookups++;
	*entry = table_find(s, key);
	*run = NULL;
	if (*entry)
		return true;
	for (int i = s->nruns - 1; i >= 0; i--)
	{
		checked++;
		if (run_find(s, &s->runs[i], key, place))
		{
			*run = &s->runs[i];
			break;
		}
	}
	s->stats.max_filter_checks = Max(s->stats.max_filter_checks, checked);
	return *run != NULL;
}

bool
prolly_seen_find(ProllySeen *s, const ProllyHash *hash, void *value)
{
	ProllyHash	key = key_of(hash);
	char	   *entry;
	Run		   *run;
	uint64		place;

	if (!locate(s, &key, &entry, &run, &place))
		return false;
	if (value && s->value_size > 0)
		memcpy(value, entry ? entry + sizeof(ProllyHash) :
			   s->record + sizeof(ProllyHash), s->value_size);
	return true;
}

bool
prolly_seen_enter(ProllySeen *s, const ProllyHash *hash, const void *value)
{
	ProllyHash	key = key_of(hash);
	char	   *entry;
	Run		   *run;
	uint64		place;

	Assert(key.len > 0);
	if (locate(s, &key, &entry, &run, &place))
		return false;
	if (!table_has_room(s))
		write_run(s);
	entry = table_slot(s, &key);
	memcpy(entry, &key, sizeof(ProllyHash));
	if (s->value_size > 0)
		memcpy(entry + sizeof(ProllyHash), value, s->value_size);
	s->in_table++;
	return true;
}

void
prolly_seen_set(ProllySeen *s, const ProllyHash *hash, const void *value)
{
	ProllyHash	key = key_of(hash);
	char	   *entry;
	Run		   *run;
	uint64		place;

	if (!locate(s, &key, &entry, &run, &place))
		elog(ERROR, "hash %s isn't in the walk's map", prolly_hash_hex(hash));
	if (s->value_size == 0)
		return;
	if (entry)
	{
		memcpy(entry + sizeof(ProllyHash), value, s->value_size);
		return;
	}
	file_write(run->file, value, s->value_size,
			   (off_t) (place * s->record_size + sizeof(ProllyHash)));
	/* (the window holds the record: run_find just read it there) */
	if (s->window_file == run->file && place >= s->window_first &&
		place < s->window_first + s->window_n)
		memcpy(s->window + (place - s->window_first) * s->record_size +
			   sizeof(ProllyHash), value, s->value_size);
}

uint64
prolly_seen_count(ProllySeen *s)
{
	return s->in_table + s->spilled;
}

int
prolly_seen_runs(ProllySeen *s)
{
	return s->nruns;
}

void
prolly_seen_stats(ProllySeen *s, ProllySeenStats *stats)
{
	note_memory(s);
	s->stats.runs = s->nruns;
	*stats = s->stats;
}

void
prolly_seen_destroy(ProllySeen *s)
{
	for (int i = 0; i < s->nruns; i++)
		FileClose(s->runs[i].file);
	MemoryContextDelete(s->cxt);
	pfree(s);
}
