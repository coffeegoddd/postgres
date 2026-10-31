/*
 * pgstore.c
 *	  The history store inside Postgres, behind the ChunkStore interface (see
 *	  pgstore.h): chunk metadata in memory, chunks in a schema's page log
 *	  (contrib/prolly_chunkpages) on the server.
 *
 * Schema s holds:
 *	pages	the log (access method prolly_chunks), written by cp.cp_append
 *	idx		hash -> place in the log and length
 *	roots	hash -> durable root count
 *	state	the repo state root
 * A flush is one transaction: new chunks copied in (binary COPY to a
 * temporary table, then appended to the log and indexed), chunks let go
 * removed from the index, the roots and state written whole.  The log's
 * space for chunks let go comes back by compaction (cp.compact), not here.
 *
 * Chunks read or written stay cached across a flush up to a byte budget,
 * the least recently used let go first, and only at a flush (which is when
 * bytes returned by get may become invalid, as with dirstore).
 */
#include "../trace.h"
#include <libpq-fe.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "pgstore.h"

#define SLOT_EMPTY 0
#define SLOT_USED 1
#define SLOT_DELETED 2

typedef struct PEntry
{
	PHash		h;
	uint8_t    *data;			/* cached bytes, or NULL: on the server */
	size_t		len;
	int64_t		loc;			/* place in the log; -1 until flushed */
	uint64_t	used;			/* when last read or written (the store's tick) */
	ChunkMeta	meta;
	uint8_t		state;
	bool		pending;		/* not yet on the server */
} PEntry;

typedef struct PgStore
{
	ChunkStore	base;
	PGconn	   *c;
	char	   *schema;			/* quoted */
	bool		ephemeral;
	bool		readonly;		/* a standby: the store is only read */
	int			packs;			/* 0: a row a chunk; 1: a pack a flush, cp_append;
								 * 2: a pack a flush, cp_append_bulk */
	bool		failed;			/* not opened: nothing to flush */
	PEntry	   *tab;
	size_t		cap,
				used,
				deleted;
	uint64_t	bytes;
	size_t		npending;
	PHashMap	roots;
	bool		roots_dirty;
	PHash	   *dels;			/* chunks let go since the last flush */
	int64_t    *del_locs;		/* (their places in the log) */
	size_t		ndel,
				delcap,
				dellocap;
	PHash		state;
	bool		has_state,
				state_dirty;
	uint64_t	server_reads;
	double		server_read_secs;	/* spent in them */
	uint64_t	server_read_calls;	/* round trips for them */
	uint64_t	tick;
	uint64_t	cache_budget;	/* bytes of flushed chunks kept across a flush */
} PgStore;

/* ------------------------------------------------------------- the table */

static size_t
slot_of(const PHash *h, size_t cap)
{
	uint64_t	x;

	memcpy(&x, h->b, sizeof(x));
	return (size_t) (x & (cap - 1));
}

static PEntry *
find(PgStore *ps, const PHash *h)
{
	for (size_t i = slot_of(h, ps->cap);; i = (i + 1) & (ps->cap - 1))
	{
		PEntry	   *e = &ps->tab[i];

		if (e->state == SLOT_EMPTY)
			return NULL;
		if (e->state == SLOT_USED && phash_eq(&e->h, h))
			return e;
	}
}

static void
rehash(PgStore *ps, size_t cap)
{
	PEntry	   *old = ps->tab;
	size_t		oldcap = ps->cap;

	ps->tab = calloc(cap, sizeof(PEntry));
	if (ps->tab == NULL)
		abort();
	ps->cap = cap;
	ps->deleted = 0;
	for (size_t i = 0; i < oldcap; i++)
	{
		if (old[i].state != SLOT_USED)
			continue;
		for (size_t j = slot_of(&old[i].h, cap);; j = (j + 1) & (cap - 1))
			if (ps->tab[j].state == SLOT_EMPTY)
			{
				ps->tab[j] = old[i];
				break;
			}
	}
	free(old);
}

/* A new entry for h (which must be absent), with zeroed metadata. */
static PEntry *
insert(PgStore *ps, const PHash *h)
{
	PEntry	   *slot = NULL;

	if ((ps->used + ps->deleted + 1) * 10 > ps->cap * 7)
		rehash(ps, (ps->used + 1) * 10 > ps->cap * 5 ? ps->cap * 2 : ps->cap);
	for (size_t i = slot_of(h, ps->cap);; i = (i + 1) & (ps->cap - 1))
		if (ps->tab[i].state != SLOT_USED)
		{
			slot = &ps->tab[i];
			break;
		}
	if (slot->state == SLOT_DELETED)
		ps->deleted--;
	memset(slot, 0, sizeof(*slot));
	slot->state = SLOT_USED;
	slot->h = *h;
	slot->loc = -1;
	ps->used++;
	return slot;
}

/* --------------------------------------------------------------- the server */

static bool
ok(PGresult *res, char *err, size_t errlen, PGconn *c)
{
	ExecStatusType s = PQresultStatus(res);

	if (s == PGRES_COMMAND_OK || s == PGRES_TUPLES_OK || s == PGRES_COPY_IN)
		return true;
	if (err)
		snprintf(err, errlen, "%s", PQerrorMessage(c));
	return false;
}

static bool
run(PgStore *ps, const char *sql, char *err, size_t errlen)
{
	PGresult   *res = PQexec(ps->c, sql);
	bool		good = ok(res, err, errlen, ps->c);

	PQclear(res);
	return good;
}

static void
must(PgStore *ps, const char *sql)
{
	char		err[512];

	if (!run(ps, sql, err, sizeof(err)))
	{
		fprintf(stderr, "pgstore: %s: %s\n", sql, err);
		abort();
	}
}

/* sql with the schema in place of each %s (at most four). */
static char *
in_schema(PgStore *ps, const char *fmt)
{
	size_t		n = strlen(fmt) + 4 * strlen(ps->schema) + 1;
	char	   *out = malloc(n);

	snprintf(out, n, fmt, ps->schema, ps->schema, ps->schema, ps->schema);
	return out;
}

static void
must_in(PgStore *ps, const char *fmt)
{
	char	   *sql = in_schema(ps, fmt);

	must(ps, sql);
	free(sql);
}

static void
put_be16(Buf *b, uint16_t v)
{
	uint8_t		x[2] = {(uint8_t) (v >> 8), (uint8_t) v};

	buf_append(b, x, 2);
}

static void
put_be32(Buf *b, uint32_t v)
{
	uint8_t		x[4] = {(uint8_t) (v >> 24), (uint8_t) (v >> 16), (uint8_t) (v >> 8), (uint8_t) v};

	buf_append(b, x, 4);
}

static int64_t
get_be64(const char *p)
{
	uint64_t	v = 0;

	for (int i = 0; i < 8; i++)
		v = (v << 8) | (uint8_t) p[i];
	return (int64_t) v;
}

static uint32_t
get_be32(const char *p)
{
	return ((uint32_t) (uint8_t) p[0] << 24) | ((uint32_t) (uint8_t) p[1] << 16) |
		((uint32_t) (uint8_t) p[2] << 8) | (uint8_t) p[3];
}

/* A hash as a bytea literal: \x and hex. */
static void
hash_literal(const PHash *h, char *out)
{
	out[0] = '\\';
	out[1] = 'x';
	phash_hex(h, out + 2);
}

/* ------------------------------------------------------------ the store */

static const uint8_t *
pg_get(ChunkStore *cs, const PHash *h, size_t *len)
{
	PgStore    *ps = (PgStore *) cs;
	PEntry	   *e = find(ps, h);

	if (e == NULL)
		return NULL;
	e->used = ++ps->tick;
	if (e->data == NULL)
	{
		char	   *sql = in_schema(ps, "SELECT cp.cp_read('%s.pages', $1::int8, $2::int4)");
		char		loc[24],
					l[16];
		const char *vals[2] = {loc, l};
		PGresult   *res;

		snprintf(loc, sizeof(loc), "%lld", (long long) e->loc);
		snprintf(l, sizeof(l), "%zu", e->len);
		struct timespec t0,
					t1;

		clock_gettime(CLOCK_MONOTONIC, &t0);
		res = PQexecParams(ps->c, sql, 2, NULL, vals, NULL, NULL, 1);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		ps->server_read_secs += (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
		free(sql);
		if (PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) != 1 ||
			(size_t) PQgetlength(res, 0, 0) != e->len)
		{
			fprintf(stderr, "pgstore: reading a chunk: %s\n", PQerrorMessage(ps->c));
			abort();
		}
		e->data = px_memdup(PQgetvalue(res, 0, 0), e->len);
		PQclear(res);
		ps->server_reads++;
		ps->server_read_calls++;
		/* what the server sent must be the chunk asked for */
		{
			PHash		got;

			phash_compute(e->data, e->len, &got);
			if (!phash_eq(&got, h))
			{
				fprintf(stderr, "pgstore: the server's bytes for a chunk do not match its hash\n");
				abort();
			}
		}
	}
	*len = e->len;
	return e->data;
}

#define GET_MANY_BATCH 2000

/*
 * Several chunks at once: those not cached read from the server in
 * statements of GET_MANY_BATCH chunks each (one round trip a statement),
 * each checked against its hash, and kept cached as pg_get keeps them.
 */
static bool
pg_get_many(ChunkStore *cs, const PHash *hs, size_t n, const uint8_t **out,
			size_t *lens)
{
	PgStore    *ps = (PgStore *) cs;
	PEntry	  **want = px_malloc((n ? n : 1) * sizeof(PEntry *));
	size_t		nwant = 0;
	char	   *sql = in_schema(ps, "SELECT cp.cp_read('%s.pages', u.l, u.n) "
								"FROM unnest($1::int8[], $2::int4[]) WITH ORDINALITY AS u(l, n, o) "
								"ORDER BY u.o");

	for (size_t i = 0; i < n; i++)
	{
		PEntry	   *e = find(ps, &hs[i]);

		if (e && e->data == NULL)
		{
			bool		dup = false;

			/* (a hash asked twice is read once) */
			for (size_t k = nwant > 64 ? nwant - 64 : 0; k < nwant; k++)
				dup |= want[k] == e;
			if (!dup)
				want[nwant++] = e;
		}
	}
	for (size_t lo = 0; lo < nwant; lo += GET_MANY_BATCH)
	{
		size_t		hi = lo + GET_MANY_BATCH < nwant ? lo + GET_MANY_BATCH : nwant;
		Buf			locs = {0},
					ls = {0};
		char		num[32];
		const char *vals[2];
		PGresult   *res;
		struct timespec t0,
					t1;

		buf_append(&locs, "{", 1);
		buf_append(&ls, "{", 1);
		for (size_t i = lo; i < hi; i++)
		{
			snprintf(num, sizeof(num), "%s%lld", i > lo ? "," : "", (long long) want[i]->loc);
			buf_append(&locs, num, strlen(num));
			snprintf(num, sizeof(num), "%s%zu", i > lo ? "," : "", want[i]->len);
			buf_append(&ls, num, strlen(num));
		}
		buf_append(&locs, "}", 2);
		buf_append(&ls, "}", 2);
		vals[0] = (const char *) locs.data;
		vals[1] = (const char *) ls.data;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		res = PQexecParams(ps->c, sql, 2, NULL, vals, NULL, NULL, 1);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		ps->server_read_secs += (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
		free(locs.data);
		free(ls.data);
		if (PQresultStatus(res) != PGRES_TUPLES_OK || (size_t) PQntuples(res) != hi - lo)
		{
			fprintf(stderr, "pgstore: reading chunks: %s\n", PQerrorMessage(ps->c));
			abort();
		}
		for (size_t i = lo; i < hi; i++)
		{
			PEntry	   *e = want[i];
			PHash		got;

			if ((size_t) PQgetlength(res, (int) (i - lo), 0) != e->len)
				abort();
			e->data = px_memdup(PQgetvalue(res, (int) (i - lo), 0), e->len);
			e->used = ++ps->tick;
			phash_compute(e->data, e->len, &got);
			if (!phash_eq(&got, &e->h))
			{
				fprintf(stderr, "pgstore: the server's bytes for a chunk do not match its hash\n");
				abort();
			}
		}
		PQclear(res);
		ps->server_reads += hi - lo;
		ps->server_read_calls++;
	}
	free(sql);
	free(want);
	for (size_t i = 0; i < n; i++)
	{
		PEntry	   *e = find(ps, &hs[i]);

		out[i] = e ? e->data : NULL;
		lens[i] = e ? e->len : 0;
		if (e)
			e->used = ++ps->tick;
	}
	return true;
}

static bool
pg_put(ChunkStore *cs, const PHash *h, const uint8_t *data, size_t len)
{
	PgStore    *ps = (PgStore *) cs;
	PEntry	   *e;

	if (find(ps, h))
		return false;
	e = insert(ps, h);
	e->data = px_memdup(data, len);
	e->len = len;
	e->used = ++ps->tick;
	e->pending = true;
	ps->npending++;
	ps->bytes += len;
	return true;
}

static bool
pg_has(ChunkStore *cs, const PHash *h)
{
	return find((PgStore *) cs, h) != NULL;
}

static ChunkMeta *
pg_meta(ChunkStore *cs, const PHash *h)
{
	PEntry	   *e = find((PgStore *) cs, h);

	return e ? &e->meta : NULL;
}

static void
pg_del(ChunkStore *cs, const PHash *h)
{
	PgStore    *ps = (PgStore *) cs;
	PEntry	   *e = find(ps, h);

	if (e == NULL)
		return;
	if (e->pending)
		ps->npending--;
	else
	{
		px_grow((void **) &ps->dels, &ps->delcap, ps->ndel + 1, sizeof(PHash));
		px_grow((void **) &ps->del_locs, &ps->dellocap, ps->ndel + 1, sizeof(int64_t));
		ps->del_locs[ps->ndel] = e->loc;
		ps->dels[ps->ndel++] = *h;
	}
	free(e->data);
	e->data = NULL;
	ps->bytes -= e->len;
	e->state = SLOT_DELETED;
	ps->used--;
	ps->deleted++;
}

static void
pg_foreach(ChunkStore *cs, ChunkVisitFn fn, void *arg)
{
	PgStore    *ps = (PgStore *) cs;

	for (size_t i = 0; i < ps->cap; i++)
		if (ps->tab[i].state == SLOT_USED)
			fn(arg, &ps->tab[i].h);
}

static size_t
pg_count(ChunkStore *cs)
{
	return ((PgStore *) cs)->used;
}

static uint64_t
pg_bytes(ChunkStore *cs)
{
	return ((PgStore *) cs)->bytes;
}

static void
pg_note_root(ChunkStore *cs, const PHash *h, uint64_t roots)
{
	PgStore    *ps = (PgStore *) cs;
	bool		created;

	ps->roots_dirty = true;
	if (roots == 0)
		phmap_remove(&ps->roots, h);
	else
		*phmap_ref(&ps->roots, h, &created) = roots;
}

static void pg_destroy(ChunkStore *cs);

static void
quiet(void *arg, const char *message)
{
}

static const ChunkStoreOps pg_ops = {
	pg_get, pg_put, pg_has, pg_meta, pg_del, pg_foreach,
	pg_count, pg_bytes, pg_destroy, pg_note_root, pg_get_many
};

/* -------------------------------------------------------------- flushing */

/* The chunks not yet on the server, through a binary COPY. */
#define PACK_ENT (PHASH_LEN + 8 + 4)

static void
put_be64_at(uint8_t *p, uint64_t v)
{
	for (int i = 0; i < 8; i++)
		p[i] = (uint8_t) (v >> (56 - 8 * i));
}

static void
put_be32_at(uint8_t *p, uint32_t v)
{
	for (int i = 0; i < 4; i++)
		p[i] = (uint8_t) (v >> (24 - 8 * i));
}

#define PACK_MAX	(64 << 20)	/* bytes of chunks a pack, at most */

/*
 * The chunks not yet on the server as packs of at most PACK_MAX bytes: each
 * their bytes, then an index of (hash, offset, length), appended to the log
 * in one call, and one row in packs naming it.  No row, nor index entry, a
 * chunk.
 */
static void
send_pack_of(PgStore *ps, PEntry **ents, size_t n, uint64_t data_len)
{
	uint64_t	off = 0;
	uint8_t    *pack = px_malloc(data_len + n * PACK_ENT);
	const char *vals[1];
	int			lens[1],
				fmts[1] = {1};
	PGresult   *res;
	char	   *sql;
	int64_t		start;
	char		row[256];

	for (size_t k = 0; k < n; k++)
	{
		uint8_t    *q = pack + data_len + k * PACK_ENT;

		memcpy(pack + off, ents[k]->data, ents[k]->len);
		memcpy(q, ents[k]->h.b, PHASH_LEN);
		put_be64_at(q + PHASH_LEN, off);
		put_be32_at(q + PHASH_LEN + 8, (uint32_t) ents[k]->len);
		off += ents[k]->len;
	}
	vals[0] = (const char *) pack;
	lens[0] = (int) (data_len + n * PACK_ENT);
	sql = in_schema(ps, ps->packs == 2 ?
					"SELECT cp.cp_append_bulk('%s.pages', $1::bytea)" :
					"SELECT cp.cp_append('%s.pages', $1::bytea)");
	res = PQexecParams(ps->c, sql, 1, NULL, vals, lens, fmts, 0);
	free(sql);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		fprintf(stderr, "pgstore: append a pack: %s\n", PQerrorMessage(ps->c));
		abort();
	}
	start = strtoll(PQgetvalue(res, 0, 0), NULL, 10);
	PQclear(res);
	snprintf(row, sizeof(row), "INSERT INTO %%s.packs VALUES (%lld, %zu, %llu)",
			 (long long) start, n, (unsigned long long) data_len);
	must_in(ps, row);
	off = 0;
	for (size_t k = 0; k < n; k++)
	{
		ents[k]->loc = start + (int64_t) off;
		off += ents[k]->len;
	}
	free(pack);
}

static void
send_pack(PgStore *ps)
{
	PEntry	  **ents = NULL;
	size_t		n = 0,
				cap = 0;
	uint64_t	data_len = 0;

	for (size_t i = 0; i < ps->cap; i++)
	{
		PEntry	   *e = &ps->tab[i];

		if (e->state != SLOT_USED || !e->pending)
			continue;
		if (n > 0 && data_len + e->len > PACK_MAX)
		{
			send_pack_of(ps, ents, n, data_len);
			n = 0;
			data_len = 0;
		}
		px_grow((void **) &ents, &cap, n + 1, sizeof(PEntry *));
		ents[n++] = e;
		data_len += e->len;
	}
	if (n > 0)
		send_pack_of(ps, ents, n, data_len);
	free(ents);
}

static void
send_new(PgStore *ps)
{
	Buf			b = {0};
	PGresult   *res;
	char	   *sql;
	static const char header[] = "PGCOPY\n\377\r\n";

	if (ps->packs)
	{
		send_pack(ps);
		return;
	}

	must(ps, "TRUNCATE prolly_pending");
	res = PQexec(ps->c, "COPY prolly_pending (hash, data) FROM STDIN (FORMAT binary)");
	if (PQresultStatus(res) != PGRES_COPY_IN)
	{
		fprintf(stderr, "pgstore: copy: %s\n", PQerrorMessage(ps->c));
		abort();
	}
	PQclear(res);
	buf_append(&b, header, 11);
	put_be32(&b, 0);
	put_be32(&b, 0);
	for (size_t i = 0; i < ps->cap; i++)
	{
		PEntry	   *e = &ps->tab[i];

		if (e->state != SLOT_USED || !e->pending)
			continue;
		put_be16(&b, 2);
		put_be32(&b, PHASH_LEN);
		buf_append(&b, e->h.b, PHASH_LEN);
		put_be32(&b, (uint32_t) e->len);
		buf_append(&b, e->data, e->len);
		if (b.len > (1 << 20))
		{
			if (PQputCopyData(ps->c, (const char *) b.data, (int) b.len) != 1)
				abort();
			b.len = 0;
		}
	}
	put_be16(&b, 0xffff);
	if (PQputCopyData(ps->c, (const char *) b.data, (int) b.len) != 1 ||
		PQputCopyEnd(ps->c, NULL) != 1)
		abort();
	res = PQgetResult(ps->c);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		fprintf(stderr, "pgstore: copy: %s\n", PQerrorMessage(ps->c));
		abort();
	}
	PQclear(res);
	free(b.data);

	/* appended to the log in one pass; each chunk's place comes back */
	sql = in_schema(ps, "INSERT INTO %s.idx (hash, loc, len) "
					"SELECT hash, cp.cp_append('%s.pages', data), length(data) FROM prolly_pending "
					"ON CONFLICT (hash) DO NOTHING RETURNING hash, loc");
	res = PQexecParams(ps->c, sql, 0, NULL, NULL, NULL, NULL, 1);
	free(sql);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		fprintf(stderr, "pgstore: append: %s\n", PQerrorMessage(ps->c));
		abort();
	}
	for (int i = 0; i < PQntuples(res); i++)
	{
		PHash		h;
		PEntry	   *e;

		memcpy(h.b, PQgetvalue(res, i, 0), PHASH_LEN);
		e = find(ps, &h);
		if (e)
			e->loc = get_be64(PQgetvalue(res, i, 1));
	}
	PQclear(res);
}

static int
by_use(const void *a, const void *b)
{
	uint64_t	x = (*(PEntry *const *) a)->used,
				y = (*(PEntry *const *) b)->used;

	return (x > y) - (x < y);
}

/* A text[] of bytea literals for n hashes. */
static void
hash_array(const PHash *hs, size_t n, Buf *out)
{
	char		lit[PHASH_HEX_LEN + 3];

	out->len = 0;
	buf_append(out, "{", 1);
	for (size_t i = 0; i < n; i++)
	{
		hash_literal(&hs[i], lit);
		if (i)
			buf_append(out, ",", 1);
		buf_append(out, "\"\\", 2);		/* the backslash, escaped for the array */
		buf_append(out, lit, strlen(lit));
		buf_append(out, "\"", 1);
	}
	buf_append(out, "}", 2);
}

int
pgstore_flush(ChunkStore *cs)
{
	PgStore    *ps = (PgStore *) cs;
	Buf			arr = {0};
	char	   *sql;

	if (ps->readonly)
		return -1;
	if (ps->npending == 0 && ps->ndel == 0 && !ps->roots_dirty && !ps->state_dirty)
		return 0;
	TraceSpan	tfl = trace_begin("pgstore: flush");
	size_t		npend = ps->npending;

	must(ps, "BEGIN");
	if (ps->npending > 0)
	{
		TraceSpan	tsn = trace_begin("pgstore: send chunks");

		send_new(ps);
		trace_end(tsn, "%zu chunks", npend);
	}
	if (ps->ndel > 0)
	{
		const char *vals[1];
		PGresult   *res;

		if (ps->packs)
		{
			/* (a chunk's copy let go, by its place: it may be put again) */
			char		num[24];

			arr.len = 0;
			buf_append(&arr, "{", 1);
			for (size_t i = 0; i < ps->ndel; i++)
			{
				snprintf(num, sizeof(num), "%s%lld", i ? "," : "", (long long) ps->del_locs[i]);
				buf_append(&arr, num, strlen(num));
			}
			buf_append(&arr, "}", 2);
		}
		else
			hash_array(ps->dels, ps->ndel, &arr);
		vals[0] = (const char *) arr.data;
		sql = in_schema(ps, ps->packs ?
						"INSERT INTO %s.gone SELECT unnest($1::int8[]) ON CONFLICT DO NOTHING" :
						"DELETE FROM %s.idx WHERE hash = ANY ($1::bytea[])");
		res = PQexecParams(ps->c, sql, 1, NULL, vals, NULL, NULL, 0);
		free(sql);
		if (!ok(res, NULL, 0, ps->c))
		{
			fprintf(stderr, "pgstore: delete: %s\n", PQerrorMessage(ps->c));
			abort();
		}
		PQclear(res);
		ps->ndel = 0;
	}
	TraceSpan	trt = trace_begin("pgstore: roots");

	if (ps->roots_dirty)
	{
		Buf			counts = {0};
		PHash	   *hs = NULL;
		size_t		n = 0,
					cap = 0;
		const char *vals[2];
		PGresult   *res;
		char		num[24];

		must_in(ps, "DELETE FROM %s.roots");
		buf_append(&counts, "{", 1);
		for (size_t i = 0; i < ps->roots.cap; i++)
			if (ps->roots.used[i])
			{
				px_grow((void **) &hs, &cap, n + 1, sizeof(PHash));
				hs[n] = ps->roots.keys[i];
				snprintf(num, sizeof(num), "%s%llu", n ? "," : "",
						 (unsigned long long) ps->roots.vals[i]);
				buf_append(&counts, num, strlen(num));
				n++;
			}
		buf_append(&counts, "}", 2);
		if (n > 0)
		{
			hash_array(hs, n, &arr);
			vals[0] = (const char *) arr.data;
			vals[1] = (const char *) counts.data;
			sql = in_schema(ps, "INSERT INTO %s.roots SELECT * FROM unnest($1::bytea[], $2::int8[])");
			res = PQexecParams(ps->c, sql, 2, NULL, vals, NULL, NULL, 0);
			free(sql);
			if (!ok(res, NULL, 0, ps->c))
			{
				fprintf(stderr, "pgstore: roots: %s\n", PQerrorMessage(ps->c));
				abort();
			}
			PQclear(res);
		}
		free(hs);
		free(counts.data);
		ps->roots_dirty = false;
	}
	if (ps->state_dirty)
	{
		char		lit[PHASH_HEX_LEN + 3];
		const char *vals[1] = {lit};
		PGresult   *res;

		hash_literal(&ps->state, lit);
		sql = in_schema(ps, "INSERT INTO %s.state VALUES (1, $1::bytea) "
						"ON CONFLICT (k) DO UPDATE SET root = EXCLUDED.root");
		res = PQexecParams(ps->c, sql, 1, NULL, vals, NULL, NULL, 0);
		free(sql);
		if (!ok(res, NULL, 0, ps->c))
			abort();
		PQclear(res);
		ps->state_dirty = false;
	}
	trace_end(trt, "and state");
	{
		TraceSpan	tc = trace_begin("pgstore: commit");

		must(ps, "COMMIT");
		trace_end(tc);
	}

	/* on the server now; the cache keeps the most recently used */
	TraceSpan	ttrim = trace_begin("pgstore: cache trim");

	{
		PEntry	  **cached = NULL;
		size_t		n = 0,
					cap = 0;
		uint64_t	total = 0;

		for (size_t i = 0; i < ps->cap; i++)
		{
			PEntry	   *e = &ps->tab[i];

			if (e->state != SLOT_USED)
				continue;
			e->pending = false;
			if (e->data)
			{
				px_grow((void **) &cached, &cap, n + 1, sizeof(PEntry *));
				cached[n++] = e;
				total += e->len;
			}
		}
		if (total > ps->cache_budget)
		{
			qsort(cached, n, sizeof(PEntry *), by_use);
			for (size_t i = 0; i < n && total > ps->cache_budget; i++)
			{
				total -= cached[i]->len;
				free(cached[i]->data);
				cached[i]->data = NULL;
			}
		}
		free(cached);
		trace_end(ttrim, "%zu entries, %zu cached", ps->cap, n);
	}
	ps->npending = 0;
	free(arr.data);
	trace_end(tfl);
	return 0;
}

void
pgstore_set_state(ChunkStore *cs, const PHash *state)
{
	PgStore    *ps = (PgStore *) cs;

	ps->state = *state;
	ps->has_state = true;
	ps->state_dirty = true;
}

bool
pgstore_get_state(ChunkStore *cs, PHash *state)
{
	PgStore    *ps = (PgStore *) cs;

	if (ps->has_state)
		*state = ps->state;
	return ps->has_state;
}

uint64_t
pgstore_disk_bytes(ChunkStore *cs)
{
	PgStore    *ps = (PgStore *) cs;
	char	   *sql = in_schema(ps, "SELECT pg_relation_size('%s.pages') + pg_total_relation_size('%s.idx')");
	PGresult   *res = PQexec(ps->c, sql);
	uint64_t	n = 0;

	free(sql);
	if (PQresultStatus(res) == PGRES_TUPLES_OK)
		n = strtoull(PQgetvalue(res, 0, 0), NULL, 10);
	PQclear(res);
	return n;
}

void
pgstore_set_cache(ChunkStore *cs, uint64_t bytes)
{
	((PgStore *) cs)->cache_budget = bytes;
}

uint64_t
pgstore_server_reads(ChunkStore *cs)
{
	return ((PgStore *) cs)->server_reads;
}

double
pgstore_server_read_secs(ChunkStore *cs)
{
	return ((PgStore *) cs)->server_read_secs;
}

uint64_t
pgstore_server_read_calls(ChunkStore *cs)
{
	return ((PgStore *) cs)->server_read_calls;
}

static void
pg_destroy(ChunkStore *cs)
{
	PgStore    *ps = (PgStore *) cs;

	if (PQstatus(ps->c) == CONNECTION_OK && !ps->failed)
	{
		if (ps->ephemeral)
			must_in(ps, "DROP SCHEMA %s CASCADE");
		else if (!ps->readonly)
			pgstore_flush(cs);
	}
	for (size_t i = 0; i < ps->cap; i++)
		if (ps->tab[i].state == SLOT_USED)
			free(ps->tab[i].data);
	free(ps->tab);
	free(ps->dels);
	free(ps->del_locs);
	phmap_free(&ps->roots);
	PQfinish(ps->c);
	free(ps->schema);
	free(ps);
}

/* ------------------------------------------------------------- opening */

typedef struct Mark
{
	PgStore    *ps;
	PHashMap   *seen;
	PHash	   *stack;
	size_t		n,
				cap;
	bool		ok;
} Mark;

static void
mark_fn(void *arg, const PHash *h, bool strong)
{
	Mark	   *m = arg;
	PEntry	   *e = find(m->ps, h);
	bool		created;

	if (!strong)
		return;
	if (e == NULL)
	{
		m->ok = false;
		return;
	}
	e->meta.refs++;
	phmap_ref(m->seen, h, &created);
	if (created)
	{
		px_grow((void **) &m->stack, &m->cap, m->n + 1, sizeof(PHash));
		m->stack[m->n++] = *h;
	}
}

/*
 * The chunks the roots reach are live, with reference counts equal to the
 * strong edges among them; the rest are let go at the next flush.
 */
static bool
rebuild_holds(PgStore *ps, char *err, size_t errlen)
{
	PHashMap	seen;
	Mark		m = {ps, &seen, NULL, 0, 0, true};
	bool		created;

	phmap_init(&seen);
	for (size_t i = 0; i < ps->roots.cap; i++)
	{
		PEntry	   *e;

		if (!ps->roots.used[i])
			continue;
		e = find(ps, &ps->roots.keys[i]);
		if (e == NULL)
		{
			snprintf(err, errlen, "a root is not in the store");
			phmap_free(&seen);
			return false;
		}
		e->meta.roots = ps->roots.vals[i];
		phmap_ref(&seen, &e->h, &created);
		if (created)
		{
			px_grow((void **) &m.stack, &m.cap, m.n + 1, sizeof(PHash));
			m.stack[m.n++] = e->h;
		}
	}
	while (m.n > 0 && m.ok)
	{
		PHash		h = m.stack[--m.n];
		size_t		len = 0;
		const uint8_t *d = pg_get(&ps->base, &h, &len);

		if (!chunk_refs(d, len, mark_fn, &m))
			m.ok = false;
	}
	free(m.stack);
	if (!m.ok)
	{
		snprintf(err, errlen, "a chunk the roots reach is missing or does not parse");
		phmap_free(&seen);
		return false;
	}
	for (size_t i = 0; i < ps->cap; i++)
	{
		PEntry	   *e = &ps->tab[i];

		if (e->state == SLOT_USED && !phmap_find(&seen, &e->h))
			pg_del(&ps->base, &e->h);
	}
	phmap_free(&seen);
	return true;
}

ChunkStore *
pgstore_open(const char *conninfo, const char *schema, bool fresh, bool ephemeral, char *err,
			 size_t errlen)
{
	PgStore    *ps = px_malloc(sizeof(PgStore));
	char	   *sql;
	PGresult   *res;

	memset(ps, 0, sizeof(*ps));
	ps->base.ops = &pg_ops;
	ps->cap = 1024;
	ps->tab = calloc(ps->cap, sizeof(PEntry));
	ps->ephemeral = ephemeral;
	ps->cache_budget = 256 << 20;
	if (getenv("PGSTORE_CACHE_MB"))
		ps->cache_budget = (uint64_t) strtoull(getenv("PGSTORE_CACHE_MB"), NULL, 10) << 20;
	phmap_init(&ps->roots);
	ps->c = PQconnectdb(conninfo);
	if (PQstatus(ps->c) != CONNECTION_OK)
	{
		snprintf(err, errlen, "%s", PQerrorMessage(ps->c));
		PQfinish(ps->c);
		free(ps->tab);
		free(ps);
		return NULL;
	}
	PQsetNoticeProcessor(ps->c, quiet, NULL);
	res = PQexecParams(ps->c, "SELECT quote_ident($1)", 1, NULL, &schema, NULL, NULL, 0);
	ps->schema = strdup(PQgetvalue(res, 0, 0));
	PQclear(res);
	if (fresh)
	{
		must(ps, "CREATE EXTENSION IF NOT EXISTS prolly_chunkpages");
		must_in(ps, "DROP SCHEMA IF EXISTS %s CASCADE");
		must_in(ps, "CREATE SCHEMA %s");
		must_in(ps, "CREATE TABLE %s.pages () USING prolly_chunks");
		must_in(ps, "CREATE TABLE %s.idx (hash bytea PRIMARY KEY, loc int8 NOT NULL, len int4 NOT NULL)");
		must_in(ps, "CREATE TABLE %s.roots (hash bytea PRIMARY KEY, n int8 NOT NULL)");
		must_in(ps, "CREATE TABLE %s.state (k int PRIMARY KEY, root bytea NOT NULL)");
		must_in(ps, "CREATE TABLE %s.packs (loc int8 PRIMARY KEY, nchunks int8 NOT NULL, data_len int8 NOT NULL)");
		must_in(ps, "CREATE TABLE %s.gone (loc int8 PRIMARY KEY)");
	}
	{
		const char *m = getenv("PGSTORE_PACKS");

		ps->packs = m ? atoi(m) : 0;
	}
	/* a standby is read only: there the store is only read */
	res = PQexec(ps->c, "SELECT pg_is_in_recovery()");
	ps->readonly = PQresultStatus(res) == PGRES_TUPLES_OK && PQgetvalue(res, 0, 0)[0] == 't';
	PQclear(res);
	if (!ps->readonly)
		must(ps, "CREATE TEMP TABLE IF NOT EXISTS prolly_pending (hash bytea, data bytea)");

	/* every chunk the store holds, by place and length */
	sql = in_schema(ps, "SELECT hash, loc, len FROM %s.idx");
	res = PQexecParams(ps->c, sql, 0, NULL, NULL, NULL, NULL, 1);
	free(sql);
	if (!ok(res, err, errlen, ps->c))
	{
		PQclear(res);
		ps->failed = true;
		pg_destroy(&ps->base);
		return NULL;
	}
	for (int i = 0; i < PQntuples(res); i++)
	{
		PHash		h;
		PEntry	   *e;

		memcpy(h.b, PQgetvalue(res, i, 0), PHASH_LEN);
		e = insert(ps, &h);
		e->loc = get_be64(PQgetvalue(res, i, 1));
		e->len = get_be32(PQgetvalue(res, i, 2));
		ps->bytes += e->len;
	}
	PQclear(res);

	/* the chunks of packs, from each pack's index; less the copies let go */
	{
		int64_t    *gone = NULL;
		size_t		ngone = 0,
					gonecap = 0;

		sql = in_schema(ps, "SELECT loc FROM %s.gone ORDER BY loc");
		res = PQexecParams(ps->c, sql, 0, NULL, NULL, NULL, NULL, 1);
		free(sql);
		if (PQresultStatus(res) == PGRES_TUPLES_OK)
			for (int i = 0; i < PQntuples(res); i++)
			{
				px_grow((void **) &gone, &gonecap, ngone + 1, sizeof(int64_t));
				gone[ngone++] = (int64_t) get_be64(PQgetvalue(res, i, 0));
			}
		PQclear(res);
		sql = in_schema(ps, "SELECT loc, nchunks, data_len, "
						"cp.cp_read('%s.pages', loc + data_len, (nchunks * 32)::int4) FROM %s.packs");
		res = PQexecParams(ps->c, sql, 0, NULL, NULL, NULL, NULL, 1);
		free(sql);
		if (PQresultStatus(res) == PGRES_TUPLES_OK)
		{
			for (int i = 0; i < PQntuples(res); i++)
			{
				int64_t		loc = (int64_t) get_be64(PQgetvalue(res, i, 0));
				int64_t		nch = (int64_t) get_be64(PQgetvalue(res, i, 1));
				const char *idx = PQgetvalue(res, i, 3);

				for (int64_t k = 0; k < nch; k++)
				{
					const char *q = idx + k * PACK_ENT;
					int64_t		at = loc + (int64_t) get_be64(q + PHASH_LEN);
					size_t		lo = 0,
								hi = ngone;
					PHash		h;
					PEntry	   *e;

					while (lo < hi)
					{
						size_t		mid = (lo + hi) / 2;

						if (gone[mid] < at)
							lo = mid + 1;
						else
							hi = mid;
					}
					if (lo < ngone && gone[lo] == at)
						continue;
					memcpy(h.b, q, PHASH_LEN);
					e = insert(ps, &h);
					e->loc = at;
					e->len = get_be32(q + PHASH_LEN + 8);
					ps->bytes += e->len;
				}
			}
		}
		PQclear(res);
		free(gone);
	}

	sql = in_schema(ps, "SELECT hash, n FROM %s.roots");
	res = PQexecParams(ps->c, sql, 0, NULL, NULL, NULL, NULL, 1);
	free(sql);
	for (int i = 0; i < PQntuples(res); i++)
	{
		PHash		h;
		bool		created;

		memcpy(h.b, PQgetvalue(res, i, 0), PHASH_LEN);
		*phmap_ref(&ps->roots, &h, &created) = (uint64_t) get_be64(PQgetvalue(res, i, 1));
	}
	PQclear(res);

	sql = in_schema(ps, "SELECT root FROM %s.state WHERE k = 1");
	res = PQexecParams(ps->c, sql, 0, NULL, NULL, NULL, NULL, 1);
	free(sql);
	if (PQntuples(res) == 1)
	{
		memcpy(ps->state.b, PQgetvalue(res, 0, 0), PHASH_LEN);
		ps->has_state = true;
	}
	PQclear(res);

	if (!rebuild_holds(ps, err, errlen))
	{
		ps->failed = true;
		pg_destroy(&ps->base);
		return NULL;
	}
	return &ps->base;
}
