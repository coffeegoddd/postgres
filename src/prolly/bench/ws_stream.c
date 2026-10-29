/*
 * ws_stream.c
 *	  The working set from the replication stream.
 */
#include "ws.h"

/* ----------------------------------------------- the streamed working set */

/*
 * The working set as the real lifecycle sees it: rows from a pgoutput
 * stream, DDL from the event trigger's messages in that stream (see
 * prolly_triggers.sql), and at each Dolt commit a barrier.  The commit takes
 * the DDL lock, exports a snapshot and writes the barrier, then lets DDL go;
 * the stream is applied up to the barrier, and the definitions, sequence
 * positions and unlogged tables (which the WAL does not carry) are read
 * at the snapshot.  The root value must equal a fresh capture's.
 */
Stream		stream;

static void st_flush(STable *t);

static STable *
st_find(uint32_t oid)
{
	for (size_t i = 0; i < stream.n; i++)
		if (stream.t[i].oid == oid)
			return &stream.t[i];
	return NULL;
}

static uint64_t
bytes_hash(const uint8_t *k, size_t n)
{
	uint64_t	h = 1469598103934665603ULL;

	for (size_t i = 0; i < n; i++)
		h = (h ^ k[i]) * 1099511628211ULL;
	return h;
}

static PendEnt *
pend_find(STable *t, const uint8_t *k, size_t kl, size_t **slotp)
{
	if (t->scap == 0)
		return NULL;
	for (size_t i = bytes_hash(k, kl) & (t->scap - 1);; i = (i + 1) & (t->scap - 1))
	{
		PendEnt    *e;

		if (t->slot[i] == 0)
		{
			if (slotp)
				*slotp = &t->slot[i];
			return NULL;
		}
		e = &t->pe[t->slot[i] - 1];
		if (bytes_eq(t->arena.data + e->koff, e->klen, k, kl))
		{
			if (slotp)
				*slotp = &t->slot[i];
			return e;
		}
	}
}

static void
pend_rehash(STable *t)
{
	size_t		cap = t->scap ? t->scap * 2 : 1024;

	free(t->slot);
	t->slot = calloc(cap, sizeof(size_t));
	if (t->slot == NULL)
		abort();
	t->scap = cap;
	for (size_t j = 0; j < t->npe; j++)
	{
		size_t	   *sp;
		PendEnt    *e = &t->pe[j];

		pend_find(t, t->arena.data + e->koff, e->klen, &sp);
		*sp = j + 1;
	}
}


static void
pend_put(STable *t, const void *k, size_t kl, const void *v, size_t vl, bool del)
{
	PendEnt    *e;
	size_t	   *sp;

	if (t->npe >= PEND_MAX)
		st_flush(t);
	if ((t->npe + 1) * 2 > t->scap)
		pend_rehash(t);
	px_grow((void **) &t->pe, &t->pecap, t->npe + 1, sizeof(PendEnt));
	e = &t->pe[t->npe];
	e->koff = t->arena.len;
	e->klen = kl;
	buf_append(&t->arena, k, kl);
	e->voff = t->arena.len;
	e->vlen = vl;
	buf_append(&t->arena, v, vl);
	e->del = del;
	pend_find(t, k, kl, &sp);	/* a key edited again overwrites its slot */
	*sp = ++t->npe;
}

static void
pend_clear(STable *t)
{
	t->npe = 0;
	t->arena.len = 0;
	if (t->slot)
		memset(t->slot, 0, t->scap * sizeof(size_t));
}

static void
st_flush(STable *t)
{
	ProllyEdit *ed;

	if (t->npe == 0)
		return;
	ed = px_malloc(t->npe * sizeof(ProllyEdit));
	for (size_t i = 0; i < t->npe; i++)
	{
		ed[i].key = t->arena.data + t->pe[i].koff;
		ed[i].klen = t->pe[i].klen;
		ed[i].val = t->arena.data + t->pe[i].voff;
		ed[i].vlen = t->pe[i].vlen;
		ed[i].del = t->pe[i].del;
	}
	t->data = t_apply(stream.ctx, &t->data, ed, t->npe);
	free(ed);
	pend_clear(t);
}

static void
st_flush_all(void)
{
	for (size_t i = 0; i < stream.n; i++)
		st_flush(&stream.t[i]);
}

/* A key's current value (pending edits first), copied into out. */
static bool
st_get(STable *t, const uint8_t *k, size_t kl, Buf *out)
{
	PendEnt    *e = pend_find(t, k, kl, NULL);
	const uint8_t *v;
	size_t		vl;

	out->len = 0;
	if (e)
	{
		if (e->del)
			return false;
		buf_append(out, t->arena.data + e->voff, e->vlen);
		return true;
	}
	if (!t_get(stream.ctx, &t->data, k, kl, &v, &vl))
		return false;
	buf_append(out, v, vl);
	return true;
}

/*
 * A tuple's stored fields in COPY order.  A value the stream left out
 * (unchanged TOAST) comes from prev, a tuple of the same row, or else from
 * cur, the row's current value.
 */
static void
st_fields(STable *t, const PgoTuple *tup, const PgoTuple *prev, const Buf *cur, bool have_cur,
		  WsField *f)
{
	const WsLayout *L = &t->L;

	for (int col = 0; col < tup->ncols; col++)
	{
		int			ix = L->stream[col];
		char		k = tup->kind[col];
		WsField    *x = &f[ix];

		x->null = false;
		if (k == PGO_UNCHANGED && prev && prev->kind[col] == PGO_TEXT)
		{
			x->p = (const uint8_t *) prev->val[col];
			x->len = (size_t) prev->len[col];
			stream.toast_filled++;
		}
		else if (k == PGO_UNCHANGED && broken("toast"))
			x->null = true;
		else if (k == PGO_UNCHANGED)
		{
			if (!have_cur || ix < L->nkey ||
				!ws_cell_get(cur->data, cur->len, L->copy[ix], &x->p, &x->len))
				die("an unchanged value in %s with no current row to take it from", t->name);
			stream.toast_filled++;
		}
		else if (k == PGO_NULL)
			x->null = true;
		else
		{
			x->p = (const uint8_t *) tup->val[col];
			x->len = (size_t) tup->len[col];
		}
	}
}

/* A tuple's key (its primary key columns are never left out). */
static void
st_key(STable *t, const PgoTuple *tup, Buf *key)
{
	static Buf	val;
	WsField		f[WS_MAX_COLS];

	for (int i = 0; i < t->L.nstored; i++)
		f[i].null = true;
	for (int col = 0; col < tup->ncols; col++)
	{
		int			ix = t->L.stream[col];

		if (ix >= t->L.nkey)
			continue;
		if (tup->kind[col] != PGO_TEXT)
			die("a key value of %s missing from the stream", t->name);
		f[ix].p = (const uint8_t *) tup->val[col];
		f[ix].len = (size_t) tup->len[col];
		f[ix].null = false;
	}
	ws_encode(&t->L, f, key, &val);
}

/* Adds delta to a keyless row's count. */
static void
st_count(STable *t, const Buf *row, long delta)
{
	Buf			cur = {0};
	long		n = 0;
	char		cnt[24];
	int			cl;

	if (st_get(t, row->data, row->len, &cur))
	{
		CHECK(cur.len < sizeof(cnt));
		memcpy(cnt, cur.data, cur.len);
		cnt[cur.len] = '\0';
		n = atol(cnt);
	}
	n += delta;
	if (n < 0)
		die("a keyless row of %s deleted more often than it was there", t->name);
	cl = snprintf(cnt, sizeof(cnt), "%ld", n);
	pend_put(t, row->data, row->len, cnt, (size_t) cl, n == 0);
	free(cur.data);
}

static void
stream_change(void *arg, char op, uint32_t relid, char oldkind, const PgoTuple *old,
			  const PgoTuple *new_)
{
	STable	   *t = st_find(relid);
	static Buf	key,
				okey,
				val,
				cur;
	WsField		f[WS_MAX_COLS];
	bool		have_cur = false;

	/* Tables the schema does not hold, or not yet (CREATE TABLE AS). */
	if (t == NULL || t->persist != 'p' || stream.skipping)
		return;
	if ((new_ && new_->ncols != t->L.nstored) || (old && old->ncols != t->L.nstored))
		die("a change to %s does not match its layout", t->name);
	stream.changes++;
	if (t->L.nkey == 0)
	{
		if (op != 'I' && oldkind != 'O')
			die("a change to keyless %s without its old row", t->name);
		if (op != 'I')
		{
			st_fields(t, old, NULL, NULL, false, f);
			ws_encode(&t->L, f, &okey, &val);
			st_count(t, &okey, -1);
		}
		if (op != 'D')
		{
			st_fields(t, new_, old, NULL, false, f);
			ws_encode(&t->L, f, &key, &val);
			st_count(t, &key, +1);
		}
		return;
	}
	if (op == 'D')
	{
		st_key(t, old, &okey);
		pend_put(t, okey.data, okey.len, NULL, 0, true);
		return;
	}
	st_key(t, new_, &key);
	okey.len = 0;
	if (op == 'U' && old)
		st_key(t, old, &okey);
	else
		buf_append(&okey, key.data, key.len);
	if (op == 'U')
		have_cur = st_get(t, okey.data, okey.len, &cur);
	st_fields(t, new_, oldkind == 'O' ? old : NULL, &cur, have_cur, f);
	{
		static Buf	k2;

		ws_encode(&t->L, f, &k2, &val);
	}
	if (!bytes_eq(okey.data, okey.len, key.data, key.len))
		pend_put(t, okey.data, okey.len, NULL, 0, true);
	pend_put(t, key.data, key.len, val.data, val.len, false);
}

static void
stream_truncate(void *arg, int n, const uint32_t *relids)
{
	if (stream.skipping)
		return;
	for (int i = 0; i < n; i++)
	{
		STable	   *t = st_find(relids[i]);

		if (t)
		{
			pend_clear(t);
			t->data = t_empty(stream.ctx);
		}
	}
}

void
st_free(STable *t)
{
	free(t->name);
	free(t->layout);
	ws_layout_free(&t->L);
	free(t->arena.data);
	free(t->pe);
	free(t->slot);
}

static void
stream_layout(const char *d, size_t len)
{
	const char *end = d + len;

	st_flush_all();
	for (size_t i = 0; i < stream.n; i++)
		stream.t[i].live = false;
	while (d < end)
	{
		char	   *p;
		uint32_t	oid = (uint32_t) strtoul(d, &p, 10);
		char		persist = p[1];
		size_t		nl = strtoul(p + 3, &p, 10),
					ll;
		const char *name = p + 1;
		STable	   *t;

		ll = strtoul(name + nl + 1, &p, 10);
		t = st_find(oid);
		if (t == NULL)
		{
			px_grow((void **) &stream.t, &stream.cap, stream.n + 1, sizeof(STable));
			t = &stream.t[stream.n++];
			memset(t, 0, sizeof(*t));
			t->oid = oid;
			t->data = t_empty(stream.ctx);
		}
		free(t->name);
		free(t->layout);
		ws_layout_free(&t->L);
		t->name = px_memdup(name, nl + 1);
		t->name[nl] = '\0';
		t->layout = px_memdup(p + 1, ll + 1);
		t->layout[ll] = '\0';
		t->persist = persist;
		t->live = true;
		layout_must_parse(t->layout, &t->L);
		d = p + 1 + ll;
	}
	for (size_t i = 0; i < stream.n;)
		if (!stream.t[i].live)
		{
			st_free(&stream.t[i]);
			stream.t[i] = stream.t[--stream.n];
		}
		else
			i++;
}

static void
stream_rows(const char *d, size_t len)
{
	char	   *p;
	uint32_t	oid = (uint32_t) strtoul(d, &p, 10);
	long		n = strtol(p + 1, &p, 10);
	const char *line = p + 1,
			   *end = d + len;
	STable	   *t = st_find(oid);
	Row		   *rows = NULL;
	size_t		nrows = 0,
				cap = 0;

	if (t == NULL)
		die("%s", "rows for a table no layout named");
	pend_clear(t);
	for (long i = 0; i < n; i++)
	{
		const char *nl = memchr(line, '\n', (size_t) (end - line));
		size_t		ll = nl ? (size_t) (nl - line) : (size_t) (end - line);

		row_add(&rows, &nrows, &cap, &t->L, line, ll);
		line += ll + 1;
	}
	t->data = rows_build(stream.ctx, rows, nrows, &t->L);
	stream.rows_reemitted += (uint64_t) n;
}

static void
stream_message(void *arg, bool tx, const char *prefix, const char *d, size_t len,
			   uint64_t lsn)
{
	if (tx && stream.skipping)
		return;
	if (strcmp(prefix, "prolly.layout") == 0)
	{
		stream.ddl_msgs++;
		stream_layout(d, len);
	}
	else if (strcmp(prefix, "prolly.rows") == 0 && !broken("noreemit"))
	{
		stream.rows_msgs++;
		st_flush_all();
		stream_rows(d, len);
	}
	else if (strcmp(prefix, "prolly.barrier") == 0 && !tx &&
			 bytes_eq((const uint8_t *) d, len, (const uint8_t *) stream.token,
					  strlen(stream.token)))
	{
		stream.barrier_lsn = lsn;
		stream.stop = true;
	}
}

/*
 * After a resume the slot may send transactions again that the last
 * recorded commit already holds (Postgres's own crash recovery can take a
 * slot's position back to its last checkpoint): they are passed over.
 */
static void
stream_begin(void *arg, uint64_t final_lsn)
{
	stream.skipping = final_lsn <= stream.skip_lsn;
}

static const PgoCallbacks stream_cb = {stream_change, stream_truncate, NULL, stream_begin,
stream_message};

void
drop_slot(void)
{
	PGconn	   *c = pg_open("postgres");

	for (int i = 0; i < 100; i++)
	{
		PGresult   *res = pg_exec(c, "SELECT active FROM pg_replication_slots "
								  "WHERE slot_name = '" STREAM_SLOT "'");
		bool		gone = PQntuples(res) == 0,
					active = !gone && PQgetvalue(res, 0, 0)[0] == 't';

		PQclear(res);
		if (gone)
			break;
		if (!active)
		{
			pg_cmd(c, "SELECT pg_drop_replication_slot('" STREAM_SLOT "')");
			break;
		}
		usleep(100000);
	}
	PQfinish(c);
}

/*
 * Makes the publication and slot in db (which has prolly_triggers.sql) and
 * starts the stream.
 */
void
stream_setup(ProllyCtx *ctx, const char *db)
{
	PGconn	   *c = pg_open(db);
	char		ci[1024];

	/* stored generated columns are row values, so the stream sends them */
	pg_cmd(c, broken("nogenpub") ? "CREATE PUBLICATION " STREAM_PUB " FOR ALL TABLES"
		   : "CREATE PUBLICATION " STREAM_PUB " FOR ALL TABLES "
		   "WITH (publish_generated_columns = stored)");
	pg_cmd(c, "SELECT pg_create_logical_replication_slot('" STREAM_SLOT "', 'pgoutput')");
	PQfinish(c);
	memset(&stream, 0, sizeof(stream));
	stream.ctx = ctx;
	/* Values arrive as text under the same settings the capture uses. */
	snprintf(ci, sizeof(ci),
			 "%s dbname=%s options='-c DateStyle=ISO -c IntervalStyle=postgres "
			 "-c TimeZone=UTC -c extra_float_digits=1 -c bytea_output=hex'",
			 o.conninfo, db);
	stream.pgo = pgo_start(ci, STREAM_SLOT, STREAM_PUB);
}

PHash
stream_capture(ProllyCtx *ctx, const char *db)
{
	PGconn	   *c = pg_open(db);
	PGresult   *res;
	const char *params[1];
	PHash		rv;
	double		t0,
				t1;

	/* The barrier: no DDL in flight, a snapshot, the barrier, DDL resumes. */
	snprintf(stream.token, sizeof(stream.token), "%d", ++stream.seq);
	pg_cmd(c, "SELECT pg_advisory_lock(" DDL_LOCK ")");
	pg_cmd(c, "BEGIN ISOLATION LEVEL REPEATABLE READ");
	res = pg_exec(c, "SELECT pg_export_snapshot()");
	snprintf(dump_snapshot, sizeof(dump_snapshot), "%s", PQgetvalue(res, 0, 0));
	PQclear(res);
	params[0] = stream.token;
	/* Flushed: the stream sends only flushed WAL. */
	res = PQexecParams(c, broken("noflush")
					   ? "SELECT pg_logical_emit_message(false, 'prolly.barrier', $1)"
					   : "SELECT pg_logical_emit_message(false, 'prolly.barrier', $1, true)",
					   1, NULL, params, NULL, NULL, 0);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
		die("barrier: %s", PQerrorMessage(c));

	/*
	 * The stream sends WAL only once flushed: a barrier that is not would
	 * wait for whatever flushes next.
	 */
	{
		const char *lsn[1] = {PQgetvalue(res, 0, 0)};
		PGresult   *r2 = PQexecParams(c, "SELECT pg_current_wal_flush_lsn() >= $1::pg_lsn",
									  1, NULL, lsn, NULL, NULL, 0);

		stream.unflushed = PQresultStatus(r2) != PGRES_TUPLES_OK || PQgetvalue(r2, 0, 0)[0] != 't';
		PQclear(r2);
	}
	PQclear(res);
	pg_cmd(c, "SELECT pg_advisory_unlock(" DDL_LOCK ")");

	stream.stop = false;
	t0 = now_sec();
	pgo_run(stream.pgo, &stream_cb, &stream, &stream.stop);
	st_flush_all();
	t1 = now_sec();
	rv = capture_defs(ctx, vc_root_empty(ctx), c, db);
	stream.wait_s = t1 - t0;
	stream.defs_s = now_sec() - t1;
	for (size_t i = 0; i < stream.n; i++)
	{
		STable	   *t = &stream.t[i];
		PHash		data = t->persist == 'p' ? t->data : capture_rows(ctx, c, &t->L);

		rv = put_table(ctx, rv, t->name, t->layout, strlen(t->layout), &data);
	}
	pg_cmd(c, "COMMIT");
	dump_snapshot[0] = '\0';
	PQfinish(c);
	return rv;
}

/*
 * The slot may let go of the WAL up to the last barrier: the caller has
 * made the commit durable (journal synced, root recorded).
 */
void
stream_ack(void)
{
	pgo_feedback(stream.pgo, stream.barrier_lsn);
}

/* The decoder's tables, "oid \t persistence \t name" per line: to resume from. */
char *
stream_tables(void)
{
	Buf			b = {0};

	for (size_t i = 0; i < stream.n; i++)
	{
		char		line[700];
		int			n = snprintf(line, sizeof(line), "%u\t%c\t%s\n", stream.t[i].oid,
								 stream.t[i].persist, stream.t[i].name);

		buf_append(&b, line, (size_t) n);
	}
	buf_append(&b, "", 1);
	return (char *) b.data;
}

/*
 * Picks the stream up again after a crash: the decoder's tables as the
 * last recorded commit has them (rows from its root value), barriers
 * counted from seq, and what that commit holds passed over.
 */
void
stream_resume(ProllyCtx *ctx, const char *db, const PHash *rv, const char *tables,
			  uint64_t after_lsn, int seq)
{
	char		ci[1024];
	char	   *copy = strdup(tables);

	memset(&stream, 0, sizeof(stream));
	stream.ctx = ctx;
	stream.seq = seq;
	stream.skip_lsn = after_lsn;
	for (char *line = strtok(copy, "\n"); line; line = strtok(NULL, "\n"))
	{
		char	   *p;
		uint32_t	oid = (uint32_t) strtoul(line, &p, 10);
		char		persist = p[1];
		const char *name = p + 3;
		STable	   *t;
		VcTable		vt;
		size_t		len;

		px_grow((void **) &stream.t, &stream.cap, stream.n + 1, sizeof(STable));
		t = &stream.t[stream.n++];
		memset(t, 0, sizeof(*t));
		t->oid = oid;
		t->persist = persist;
		t->name = strdup(name);
		CHECK(vc_table_get(ctx, rv, name, &vt));
		t->layout = schema_text(ctx, &vt.schema, &len);
		layout_must_parse(t->layout, &t->L);
		t->data = vt.data;
		t->live = true;
	}
	free(copy);
	snprintf(ci, sizeof(ci),
			 "%s dbname=%s options='-c DateStyle=ISO -c IntervalStyle=postgres "
			 "-c TimeZone=UTC -c extra_float_digits=1 -c bytea_output=hex'",
			 o.conninfo, db);
	stream.pgo = pgo_start(ci, STREAM_SLOT, STREAM_PUB);
}

static void
stream_restore(ProllyCtx *ctx, const PHash *rv, const char *db)
{
	ws_restore(ctx, rv, db, NULL);
}

const WorkingSetOps stream_ops = {"streamed", stream_capture, stream_restore};
