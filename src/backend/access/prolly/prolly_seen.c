/*-------------------------------------------------------------------------
 *
 * prolly_seen.c
 *	  What a walk has seen: a map of hashes held to a budget of memory,
 *	  spilling to a temporary file past it (prolly_seen.h).
 *
 * Entries are kept in a hash table until they would take more than the
 * budget; then they are sorted and written to the map's temporary file as
 * a run (each record the hash, zero-padded as a key, then the value), the
 * table emptied, and a Bloom filter of the run kept in memory.  A lookup
 * misses a run its filter rules out without reading it; one it doesn't,
 * it reads a record at a time, halving the run each read.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/prolly_seen.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_seen.h"
#include "miscadmin.h"
#include "storage/buffile.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"

#define FILTER_BITS		10		/* a hash's bits in a run's filter */
#define FILTER_PROBES	7

typedef struct Run
{
	off_t		start;			/* its first record's offset in the file */
	uint64		n;
	uint64		nbits;
	uint8	   *filter;
} Run;

struct ProllySeen
{
	MemoryContext cxt;
	Size		value_size;
	Size		entry_size;		/* in memory: key, then value */
	Size		record_size;	/* in the file: the same */
	uint64		max_entries;	/* in memory, before a run is written */
	HTAB	   *table;
	uint64		in_table;
	BufFile    *file;
	off_t		file_len;
	Run		   *runs;
	int			nruns;
	char	   *record;			/* a record read */
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

static HTAB *
make_table(ProllySeen *s, const char *name)
{
	HASHCTL		ctl;

	ctl.keysize = sizeof(ProllyHash);
	ctl.entrysize = s->entry_size;
	ctl.hcxt = s->cxt;
	return hash_create(name, 256, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
}

ProllySeen *
prolly_seen_create(const char *name, Size value_size, Size budget)
{
	ProllySeen *s = palloc0(sizeof(ProllySeen));

	s->cxt = AllocSetContextCreate(CurrentMemoryContext, "prolly walk seen",
								   ALLOCSET_DEFAULT_SIZES);
	MemoryContextCopyAndSetIdentifier(s->cxt, name);
	s->value_size = value_size;
	s->entry_size = MAXALIGN(sizeof(ProllyHash) + value_size);
	s->record_size = sizeof(ProllyHash) + value_size;
	if (budget == 0)
		budget = (Size) maintenance_work_mem * 1024;
	/* (a hash table's entry costs about twice its size) */
	s->max_entries = Max(budget / (2 * s->entry_size), 64);
	s->table = make_table(s, name);
	s->record = MemoryContextAlloc(s->cxt, s->record_size);
	return s;
}

/* The filter's probes for key: double hashing of its first sixteen bytes. */
static void
probes(const ProllyHash *key, uint64 nbits, uint64 *out)
{
	uint64		h1;
	uint64		h2;

	memcpy(&h1, key->bytes, sizeof(h1));
	memcpy(&h2, key->bytes + 8, sizeof(h2));
	h2 |= 1;
	for (int i = 0; i < FILTER_PROBES; i++)
		out[i] = (h1 + (uint64) i * h2) % nbits;
}

static int
record_cmp(const void *a, const void *b)
{
	return memcmp(a, b, sizeof(ProllyHash));
}

/* The table's entries written as a run, the table emptied. */
static void
write_run(ProllySeen *s)
{
	HASH_SEQ_STATUS status;
	char	   *entry;
	char	   *records;
	uint64		n = 0;
	Run		   *run;
	MemoryContext old = MemoryContextSwitchTo(s->cxt);

	records = MemoryContextAllocHuge(s->cxt, s->in_table * s->record_size);
	hash_seq_init(&status, s->table);
	while ((entry = hash_seq_search(&status)) != NULL)
		memcpy(records + n++ * s->record_size, entry, s->record_size);
	qsort(records, n, s->record_size, record_cmp);

	if (s->file == NULL)
		s->file = BufFileCreateTemp(false);
	if (BufFileSeek(s->file, 0, s->file_len, SEEK_SET) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not seek in a walk's temporary file: %m")));
	BufFileWrite(s->file, records, n * s->record_size);

	s->runs = s->runs ? repalloc(s->runs, sizeof(Run) * (s->nruns + 1)) :
		palloc(sizeof(Run));
	run = &s->runs[s->nruns++];
	run->start = s->file_len;
	run->n = n;
	run->nbits = Max(n * FILTER_BITS, 64);
	run->filter = MemoryContextAllocZero(s->cxt, (run->nbits + 7) / 8);
	for (uint64 i = 0; i < n; i++)
	{
		uint64		p[FILTER_PROBES];

		probes((ProllyHash *) (records + i * s->record_size), run->nbits, p);
		for (int j = 0; j < FILTER_PROBES; j++)
			run->filter[p[j] / 8] |= 1 << (p[j] % 8);
	}
	s->file_len += n * s->record_size;
	pfree(records);

	hash_destroy(s->table);
	s->table = make_table(s, "prolly walk seen");
	s->in_table = 0;
	MemoryContextSwitchTo(old);
}

/* The record of key in run r: its offset in the file, or -1. */
static off_t
run_find(ProllySeen *s, const Run *r, const ProllyHash *key)
{
	uint64		p[FILTER_PROBES];
	uint64		lo = 0;
	uint64		hi = r->n;

	probes(key, r->nbits, p);
	for (int j = 0; j < FILTER_PROBES; j++)
		if (!(r->filter[p[j] / 8] & (1 << (p[j] % 8))))
			return -1;
	while (lo < hi)
	{
		uint64		mid = lo + (hi - lo) / 2;
		off_t		at = r->start + (off_t) (mid * s->record_size);
		int			c;

		if (BufFileSeek(s->file, 0, at, SEEK_SET) != 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not seek in a walk's temporary file: %m")));
		BufFileReadExact(s->file, s->record, s->record_size);
		c = memcmp(key, s->record, sizeof(ProllyHash));
		if (c == 0)
			return at;
		if (c < 0)
			hi = mid;
		else
			lo = mid + 1;
	}
	return -1;
}

/* Where key is: in the table (*entry), or a run's record (offset), or not. */
static bool
locate(ProllySeen *s, const ProllyHash *key, char **entry, off_t *at)
{
	*entry = hash_search(s->table, key, HASH_FIND, NULL);
	*at = -1;
	if (*entry)
		return true;
	for (int i = s->nruns - 1; i >= 0; i--)
	{
		*at = run_find(s, &s->runs[i], key);
		if (*at >= 0)
			return true;
	}
	return false;
}

bool
prolly_seen_find(ProllySeen *s, const ProllyHash *hash, void *value)
{
	ProllyHash	key = key_of(hash);
	char	   *entry;
	off_t		at;

	if (!locate(s, &key, &entry, &at))
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
	off_t		at;

	if (locate(s, &key, &entry, &at))
		return false;
	entry = hash_search(s->table, &key, HASH_ENTER, NULL);
	if (s->value_size > 0)
		memcpy(entry + sizeof(ProllyHash), value, s->value_size);
	if (++s->in_table >= s->max_entries)
		write_run(s);
	return true;
}

void
prolly_seen_set(ProllySeen *s, const ProllyHash *hash, const void *value)
{
	ProllyHash	key = key_of(hash);
	char	   *entry;
	off_t		at;

	if (!locate(s, &key, &entry, &at))
		elog(ERROR, "hash %s isn't in the walk's map", prolly_hash_hex(hash));
	if (s->value_size == 0)
		return;
	if (entry)
	{
		memcpy(entry + sizeof(ProllyHash), value, s->value_size);
		return;
	}
	if (BufFileSeek(s->file, 0, at + (off_t) sizeof(ProllyHash), SEEK_SET) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not seek in a walk's temporary file: %m")));
	BufFileWrite(s->file, value, s->value_size);
}

uint64
prolly_seen_count(ProllySeen *s)
{
	uint64		n = s->in_table;

	for (int i = 0; i < s->nruns; i++)
		n += s->runs[i].n;
	return n;
}

int
prolly_seen_runs(ProllySeen *s)
{
	return s->nruns;
}

void
prolly_seen_destroy(ProllySeen *s)
{
	if (s->file)
		BufFileClose(s->file);
	MemoryContextDelete(s->cxt);
	pfree(s);
}
