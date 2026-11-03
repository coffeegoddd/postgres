/*-------------------------------------------------------------------------
 *
 * vc.c
 *	  Root values, commits, merge bases, and branches on a repository's
 *	  references (prolly/engine/vc.h).
 *
 * Schema and commit layouts are in prolly/engine/chunkfmt.h.  A root value
 * is a map from table name to (schema hash, data root hash), both held
 * strongly.
 *
 * Holds: a commit is held by its reference (history is kept whole until
 * the window decides otherwise), and a branch's reference holds its
 * chunk, which holds its head, its working root and any merge head.
 * Everything a call writes is held by the store until the transaction
 * ends, so a reference made later in it may name it.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/backend/prolly/engine/vc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "miscadmin.h"
#include "prolly/engine/vc.h"
#include "utils/timestamp.h"

#define ENTRY_LEN (2 * PHASH_LEN)
#define RV_VREFS 2				/* a table entry holds its schema and data root */

#define BRANCH_REF_PREFIX "vc/branch/"
#define COMMIT_REF_PREFIX "vc/commit/"

/* A tree error, raised. */
static void
vc_must(ProllyCtx *ctx, int rc)
{
	if (rc < 0)
		prolly_report(ctx);
}

/* ---------------------------------------------------------- root values */

static void
entry_encode(uint8 out[ENTRY_LEN], const VcTable *t)
{
	memcpy(out, t->schema.b, PHASH_LEN);
	memcpy(out + PHASH_LEN, t->data.b, PHASH_LEN);
}

static void
entry_decode(const uint8 *v, size_t vl, VcTable *t)
{
	if (vl != ENTRY_LEN)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("a root value's table entry is %zu bytes, not %d",
						vl, ENTRY_LEN)));
	memcpy(t->schema.b, v, PHASH_LEN);
	memcpy(t->data.b, v + PHASH_LEN, PHASH_LEN);
}

PHash
vc_root_empty(ProllyCtx *ctx)
{
	PHash		h;

	vc_must(ctx, prolly_empty(ctx, RV_VREFS, &h));
	return h;
}

bool
vc_table_get(ProllyCtx *ctx, const PHash *rv, const char *name, VcTable *out)
{
	const uint8 *v;
	size_t		vl;
	int			rc = prolly_get(ctx, rv, (const uint8 *) name, strlen(name), &v, &vl);

	if (rc == PROLLY_NOT_FOUND)
		return false;
	vc_must(ctx, rc);
	entry_decode(v, vl, out);
	px_free((void *) v);
	return true;
}

static PHash
put_entry(ProllyCtx *ctx, const PHash *rv, const char *name, const VcTable *t)
{
	uint8		val[ENTRY_LEN];
	ProllyEdit	e = {(const uint8 *) name, strlen(name), val, ENTRY_LEN, false};
	PHash		out;

	entry_encode(val, t);
	vc_must(ctx, prolly_apply(ctx, rv, &e, 1, &out));
	return out;
}

static PHash
put_schema(ProllyCtx *ctx, const void *schema, size_t slen)
{
	Buf			b = BUF_INIT;
	PHash		h;

	buf_append_byte(&b, CHUNK_TAG_SCHEMA);
	buf_append_byte(&b, CHUNK_FORMAT_VERSION);
	buf_append(&b, schema, slen);
	vc_must(ctx, prolly_put_chunk(ctx, b.data, b.len, &h));
	buf_free(&b);
	return h;
}

uint8 *
vc_schema_get(ProllyCtx *ctx, const PHash *schema, size_t *len)
{
	uint32		l;
	uint8	   *d = cs_get(ctx->cs, schema->b, &l);
	uint8	   *out;

	if (d == NULL)
		return NULL;
	if (l < 2 || d[0] != CHUNK_TAG_SCHEMA || d[1] != CHUNK_FORMAT_VERSION)
	{
		pfree(d);
		return NULL;
	}
	*len = l - 2;
	out = px_memdup(d + 2, l - 2);
	pfree(d);
	return out;
}

int
vc_table_create(ProllyCtx *ctx, const PHash *rv, const char *name,
				const void *schema, size_t slen, PHash *out)
{
	VcTable		t;

	if (vc_table_get(ctx, rv, name, &t))
		return VC_ERR;
	t.schema = put_schema(ctx, schema, slen);
	vc_must(ctx, prolly_empty(ctx, 0, &t.data));
	*out = put_entry(ctx, rv, name, &t);
	return VC_OK;
}

int
vc_table_alter(ProllyCtx *ctx, const PHash *rv, const char *name,
			   const void *schema, size_t slen, PHash *out)
{
	VcTable		t;

	if (!vc_table_get(ctx, rv, name, &t))
		return VC_ERR;
	t.schema = put_schema(ctx, schema, slen);
	*out = put_entry(ctx, rv, name, &t);
	return VC_OK;
}

int
vc_table_drop(ProllyCtx *ctx, const PHash *rv, const char *name, PHash *out)
{
	VcTable		t;
	ProllyEdit	e = {(const uint8 *) name, strlen(name), NULL, 0, true};

	if (!vc_table_get(ctx, rv, name, &t))
		return VC_ERR;
	vc_must(ctx, prolly_apply(ctx, rv, &e, 1, out));
	return VC_OK;
}

int
vc_table_edit(ProllyCtx *ctx, const PHash *rv, const char *name,
			  const ProllyEdit *edits, size_t n, PHash *out)
{
	VcTable		t;

	if (!vc_table_get(ctx, rv, name, &t))
		return VC_ERR;
	vc_must(ctx, prolly_apply(ctx, &t.data, edits, n, &t.data));
	*out = put_entry(ctx, rv, name, &t);
	return VC_OK;
}

int
vc_table_set_data(ProllyCtx *ctx, const PHash *rv, const char *name,
				  const PHash *data, PHash *out)
{
	VcTable		t;

	if (!vc_table_get(ctx, rv, name, &t))
		return VC_ERR;
	t.data = *data;
	*out = put_entry(ctx, rv, name, &t);
	return VC_OK;
}

typedef struct DiffTablesArg
{
	VcTableChangeFn fn;
	void	   *arg;
} DiffTablesArg;

static void
diff_tables_fn(void *arg, const ProllyDiff *d)
{
	DiffTablesArg *a = arg;
	VcTableChange c;

	memset(&c, 0, sizeof(c));
	c.name = d->key;
	c.nlen = d->klen;
	if ((c.has_from = d->from != NULL))
		entry_decode(d->from, d->flen, &c.from);
	if ((c.has_to = d->to != NULL))
		entry_decode(d->to, d->tlen, &c.to);
	a->fn(a->arg, &c);
}

void
vc_diff_tables(ProllyCtx *ctx, const PHash *from, const PHash *to,
			   VcTableChangeFn fn, void *arg)
{
	DiffTablesArg a = {fn, arg};

	vc_must(ctx, prolly_diff(ctx, from, to, diff_tables_fn, &a));
}

typedef struct ConflictList
{
	VcConflict *v;
	size_t		n;
	size_t		cap;
} ConflictList;

static void
add_conflict(ConflictList *l, VcConflictKind kind, const uint8 *name,
			 size_t nlen, const uint8 *key, size_t klen)
{
	VcConflict *c;

	px_grow((void **) &l->v, &l->cap, l->n + 1, sizeof(VcConflict));
	c = &l->v[l->n++];
	c->kind = kind;
	c->table = px_malloc(nlen + 1);
	memcpy(c->table, name, nlen);
	c->table[nlen] = '\0';
	c->key = key ? px_memdup(key, klen) : NULL;
	c->klen = key ? klen : 0;
}

/*
 * Merges one table both sides changed.  Returns false (after recording the
 * conflict) when no merged entry can be produced.
 */
static bool
merge_table(ProllyCtx *ctx, const ProllyDiff *o, const ProllyDiff *t,
			ConflictList *cl, VcTable *merged)
{
	VcTable		base,
				ours,
				theirs;
	bool		has_base = t->from != NULL;
	ProllyMergeResult mr;
	PHash		empty;

	if (o->to == NULL || t->to == NULL)
	{
		add_conflict(cl, VC_CONFLICT_TABLE, t->key, t->klen, NULL, 0);
		return false;
	}
	entry_decode(o->to, o->tlen, &ours);
	entry_decode(t->to, t->tlen, &theirs);
	if (has_base)
		entry_decode(t->from, t->flen, &base);

	if (phash_eq(&ours.schema, &theirs.schema))
		merged->schema = ours.schema;
	else if (has_base && phash_eq(&theirs.schema, &base.schema))
		merged->schema = ours.schema;
	else if (has_base && phash_eq(&ours.schema, &base.schema))
		merged->schema = theirs.schema;
	else
	{
		add_conflict(cl, VC_CONFLICT_SCHEMA, t->key, t->klen, NULL, 0);
		return false;
	}

	vc_must(ctx, prolly_empty(ctx, 0, &empty));
	vc_must(ctx, prolly_merge(ctx, has_base ? &base.data : &empty, &ours.data,
							  &theirs.data, &mr));
	for (size_t i = 0; i < mr.nconflicts; i++)
		add_conflict(cl, VC_CONFLICT_ROW, t->key, t->klen,
					 mr.conflicts[i].key, mr.conflicts[i].klen);
	merged->data = mr.root;
	prolly_merge_result_free(&mr);
	return true;
}

void
vc_merge_roots(ProllyCtx *ctx, const PHash *base, const PHash *ours,
			   const PHash *theirs, PHash *out,
			   VcConflict **conflicts, size_t *nconflicts)
{
	ProllyDiffList dours,
				dtheirs;
	ConflictList cl = {0};
	ProllyEdit *edits;
	uint8	   *vals;
	size_t		nedits = 0,
				a = 0;

	vc_must(ctx, prolly_diff_collect(ctx, base, ours, &dours));
	vc_must(ctx, prolly_diff_collect(ctx, base, theirs, &dtheirs));
	edits = px_malloc(dtheirs.n * sizeof(ProllyEdit));
	vals = px_malloc(dtheirs.n * ENTRY_LEN);
	for (size_t b = 0; b < dtheirs.n; b++)
	{
		const ProllyDiff *t = &dtheirs.v[b];
		const ProllyDiff *o = NULL;
		ProllyEdit *e = &edits[nedits];
		VcTable		merged;

		CHECK_FOR_INTERRUPTS();
		while (a < dours.n &&
			   key_cmp(dours.v[a].key, dours.v[a].klen, t->key, t->klen) < 0)
			a++;
		if (a < dours.n &&
			key_cmp(dours.v[a].key, dours.v[a].klen, t->key, t->klen) == 0)
			o = &dours.v[a];

		e->key = t->key;
		e->klen = t->klen;
		if (o == NULL)
		{
			/* only theirs changed this table */
			e->val = t->to;
			e->vlen = t->tlen;
			e->del = t->to == NULL;
			nedits++;
		}
		else if (o->to != NULL && t->to != NULL &&
				 bytes_eq(o->to, o->tlen, t->to, t->tlen))
			continue;
		else if (o->to == NULL && t->to == NULL)
			continue;
		else if (merge_table(ctx, o, t, &cl, &merged))
		{
			entry_encode(vals + nedits * ENTRY_LEN, &merged);
			e->val = vals + nedits * ENTRY_LEN;
			e->vlen = ENTRY_LEN;
			e->del = false;
			nedits++;
		}
	}
	*out = *ours;
	if (nedits)
		vc_must(ctx, prolly_apply(ctx, ours, edits, nedits, out));
	*conflicts = cl.v;
	*nconflicts = cl.n;
	px_free(edits);
	px_free(vals);
	prolly_diff_list_free(&dours);
	prolly_diff_list_free(&dtheirs);
}

void
vc_conflicts_free(VcConflict *c, size_t n)
{
	for (size_t i = 0; i < n; i++)
	{
		px_free(c[i].table);
		px_free(c[i].key);
	}
	px_free(c);
}

/* -------------------------------------------------------------- commits */

bool
vc_commit_load(ProllyCtx *ctx, const PHash *h, VcCommit *out)
{
	uint32		len;
	uint8	   *d = cs_get(ctx->cs, h->b, &len);
	const uint8 *p,
			   *end;
	uint64		np,
				ml;

	if (d == NULL)
		return false;
	if (len < 2 + PHASH_LEN || d[0] != CHUNK_TAG_COMMIT ||
		d[1] != CHUNK_FORMAT_VERSION)
		goto bad;
	p = d + 2;
	end = d + len;
	out->hash = *h;
	memcpy(out->root.b, p, PHASH_LEN);
	p += PHASH_LEN;
	if (!read_uvarint(&p, end, &out->generation) || !read_uvarint(&p, end, &out->bytes) ||
		!read_uvarint(&p, end, &out->time) ||
		!read_uvarint(&p, end, &np) || np > VC_MAX_PARENTS)
		goto bad;
	out->nparents = (uint32) np;
	for (uint32 i = 0; i < out->nparents; i++)
	{
		if ((size_t) (end - p) < PHASH_LEN)
			goto bad;
		memcpy(out->parents[i].b, p, PHASH_LEN);
		p += PHASH_LEN;
		if (!read_uvarint(&p, end, &out->parent_gen[i]) ||
			!read_uvarint(&p, end, &out->parent_bytes[i]) ||
			!read_uvarint(&p, end, &out->parent_time[i]))
			goto bad;
	}
	if (!read_uvarint(&p, end, &ml) || ml != (uint64) (end - p))
		goto bad;
	out->msg = (const char *) p;
	out->msglen = ml;
	out->chunk = d;
	return true;
bad:
	pfree(d);
	return false;
}

void
vc_commit_release(VcCommit *c)
{
	if (c->chunk)
		pfree(c->chunk);
	c->chunk = NULL;
	c->msg = NULL;
}

static void
commit_corrupt(const PHash *h)
{
	char		hex[PHASH_HEX_LEN + 1];

	phash_hex(h, hex);
	ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("chunk %s is not a commit", hex)));
}

/* For commits that must exist: the parents of a commit being written. */
static VcCommit
must_load_commit(ProllyCtx *ctx, const PHash *h)
{
	VcCommit	c;

	if (!vc_commit_load(ctx, h, &c))
	{
		char		hex[PHASH_HEX_LEN + 1];

		if (!cs_has(ctx->cs, h->b))
		{
			phash_hex(h, hex);
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("commit %s is not in the store", hex)));
		}
		commit_corrupt(h);
	}
	return c;
}

typedef struct SizeAcc
{
	ProllyCtx  *ctx;
	uint64		bytes;
} SizeAcc;

static void
size_fn(void *arg, const PHash *h)
{
	SizeAcc    *a = arg;
	uint32		len;
	void	   *d = cs_get(a->ctx->cs, h->b, &len);

	if (d)
	{
		a->bytes += len;
		pfree(d);
	}
}

static void
table_bytes_fn(void *arg, const VcTableChange *c)
{
	SizeAcc    *a = arg;
	PHash		from;

	if (!c->has_to)
		return;
	if (!c->has_from || !phash_eq(&c->from.schema, &c->to.schema))
		size_fn(a, &c->to.schema);
	if (c->has_from)
		from = c->from.data;
	else
		vc_must(a->ctx, prolly_empty(a->ctx, 0, &from));
	vc_must(a->ctx, prolly_chunk_diff(a->ctx, &c->to.data, &from, size_fn, a));
}

/*
 * A commit's change bytes: the chunks its root value has that base (its
 * first parent's, or an empty one) lacks.  Content alone decides it, so it
 * costs what changed and is the same wherever the commit is written.
 */
static uint64
change_bytes(ProllyCtx *ctx, const PHash *root, const PHash *base)
{
	SizeAcc		a = {ctx, 0};
	PHash		empty;

	if (base == NULL)
	{
		empty = vc_root_empty(ctx);
		base = &empty;
	}
	vc_must(ctx, prolly_chunk_diff(ctx, root, base, size_fn, &a));
	vc_diff_tables(ctx, base, root, table_bytes_fn, &a);
	return a.bytes;
}

/* A commit chunk from its fields, with each parent's measures. */
static PHash
commit_put(ProllyCtx *ctx, const PHash *root, uint64 gen, uint64 bytes, uint64 time,
		   uint32 nparents, const PHash *parents, const uint64 *pgen,
		   const uint64 *pbytes, const uint64 *ptime, const char *msg, size_t msglen)
{
	Buf			b = BUF_INIT;
	PHash		h;

	buf_append_byte(&b, CHUNK_TAG_COMMIT);
	buf_append_byte(&b, CHUNK_FORMAT_VERSION);
	buf_append(&b, root->b, PHASH_LEN);
	buf_append_uvarint(&b, gen);
	buf_append_uvarint(&b, bytes);
	buf_append_uvarint(&b, time);
	buf_append_uvarint(&b, nparents);
	for (uint32 i = 0; i < nparents; i++)
	{
		buf_append(&b, parents[i].b, PHASH_LEN);
		buf_append_uvarint(&b, pgen[i]);
		buf_append_uvarint(&b, pbytes[i]);
		buf_append_uvarint(&b, ptime[i]);
	}
	buf_append_uvarint(&b, msglen);
	buf_append(&b, msg, msglen);
	vc_must(ctx, prolly_put_chunk(ctx, b.data, b.len, &h));
	buf_free(&b);
	return h;
}

/*
 * time is raised past every parent's, so along any history generation,
 * change bytes and time all only fall.
 */
PHash
vc_commit_write(ProllyCtx *ctx, const PHash *root, const PHash *parents,
				uint32 nparents, uint64 time, const char *msg)
{
	VcCommit	pc[VC_MAX_PARENTS];
	uint64		gen = 0,
				bytes = 0,
				pgen[VC_MAX_PARENTS],
				pbytes[VC_MAX_PARENTS],
				ptime[VC_MAX_PARENTS];
	PHash		h;

	if (nparents > VC_MAX_PARENTS)
		elog(ERROR, "a commit has at most %d parents", VC_MAX_PARENTS);
	for (uint32 i = 0; i < nparents; i++)
	{
		pc[i] = must_load_commit(ctx, &parents[i]);
		if (pc[i].generation > gen)
			gen = pc[i].generation;
		if (pc[i].bytes > bytes)
			bytes = pc[i].bytes;
		if (pc[i].time >= time)
			time = pc[i].time + 1;
	}
	bytes += change_bytes(ctx, root, nparents ? &pc[0].root : NULL);
	for (uint32 i = 0; i < nparents; i++)
	{
		pgen[i] = pc[i].generation;
		pbytes[i] = pc[i].bytes;
		ptime[i] = pc[i].time;
		vc_commit_release(&pc[i]);
	}
	h = commit_put(ctx, root, gen + 1, bytes, time, nparents, parents, pgen, pbytes,
				   ptime, msg, strlen(msg));
	return h;
}

/* Max-heap of commits ordered by generation. */
typedef struct GenEnt
{
	uint64		gen;
	PHash		h;
} GenEnt;

typedef struct GenHeap
{
	GenEnt	   *v;
	size_t		n;
	size_t		cap;
} GenHeap;

static void
heap_push(GenHeap *hp, uint64 gen, const PHash *h)
{
	size_t		i;

	px_grow((void **) &hp->v, &hp->cap, hp->n + 1, sizeof(GenEnt));
	i = hp->n++;
	while (i > 0 && hp->v[(i - 1) / 2].gen < gen)
	{
		hp->v[i] = hp->v[(i - 1) / 2];
		i = (i - 1) / 2;
	}
	hp->v[i].gen = gen;
	hp->v[i].h = *h;
}

static GenEnt
heap_pop(GenHeap *hp)
{
	GenEnt		top = hp->v[0];
	GenEnt		last = hp->v[--hp->n];
	size_t		i = 0;

	for (;;)
	{
		size_t		c = 2 * i + 1;

		if (c >= hp->n)
			break;
		if (c + 1 < hp->n && hp->v[c + 1].gen > hp->v[c].gen)
			c++;
		if (hp->v[c].gen <= last.gen)
			break;
		hp->v[i] = hp->v[c];
		i = c;
	}
	if (hp->n > 0)
		hp->v[i] = last;
	return top;
}

#define MB_A 1
#define MB_B 2
#define MB_DONE 4

/*
 * Loads a commit that may lie beyond a cut: an absent one is
 * VC_ERR_SEVERED, in err_hash; one not a commit is raised.
 */
static int
load_commit(ProllyCtx *ctx, const PHash *h, VcCommit *out)
{
	if (vc_commit_load(ctx, h, out))
		return VC_OK;
	if (cs_has(ctx->cs, h->b))
		commit_corrupt(h);
	ctx->err_hash = *h;
	ctx->err_has_hash = true;
	ctx->err = VC_ERR_SEVERED;
	return ctx->err;
}

/*
 * Walks both histories newest generation first.  A commit's flags are final
 * when it is popped, because every descendant has a larger generation and
 * was popped before it, so the first commit reached from both sides is a
 * best common ancestor.  Cost is the number of commits newer than it.  The
 * answer itself is never loaded, so it may be a commit beyond a cut.  When
 * gate is set, every commit read must pass it.
 */
typedef int (*CommitGate) (void *arg, const PHash *h);

static int
merge_base_core(ProllyCtx *ctx, const PHash *a, const PHash *b,
				CommitGate gate, void *garg, PHash *out)
{
	PHashMap	flags;
	GenHeap		hp = {0};
	bool		created;
	int			result = 0;
	VcCommit	ca,
				cb;
	int			rc;

	if (phash_eq(a, b))
	{
		*out = *a;
		return 1;
	}
	if (gate && ((rc = gate(garg, a)) != VC_OK || (rc = gate(garg, b)) != VC_OK))
		return rc;
	if ((rc = load_commit(ctx, a, &ca)) != VC_OK)
		return rc;
	if ((rc = load_commit(ctx, b, &cb)) != VC_OK)
	{
		vc_commit_release(&ca);
		return rc;
	}
	phmap_init(&flags);
	*phmap_ref(&flags, a, &created) |= MB_A;
	*phmap_ref(&flags, b, &created) |= MB_B;
	heap_push(&hp, ca.generation, a);
	heap_push(&hp, cb.generation, b);
	vc_commit_release(&ca);
	vc_commit_release(&cb);
	while (hp.n > 0)
	{
		GenEnt		e = heap_pop(&hp);
		uint64	   *f = phmap_find(&flags, &e.h);
		uint64		mine;
		VcCommit	c;

		CHECK_FOR_INTERRUPTS();
		if (*f & MB_DONE)
			continue;
		*f |= MB_DONE;
		mine = *f & (MB_A | MB_B);
		if (mine == (MB_A | MB_B))
		{
			*out = e.h;
			result = 1;
			break;
		}
		if ((gate && (rc = gate(garg, &e.h)) != VC_OK) ||
			(rc = load_commit(ctx, &e.h, &c)) != VC_OK)
		{
			result = rc;
			break;
		}
		for (uint32 i = 0; i < c.nparents; i++)
		{
			uint64	   *pf = phmap_ref(&flags, &c.parents[i], &created);

			if ((*pf & mine) != mine)
			{
				*pf |= mine;
				heap_push(&hp, c.parent_gen[i], &c.parents[i]);
			}
		}
		vc_commit_release(&c);
	}
	px_free(hp.v);
	phmap_free(&flags);
	return result;
}

int
vc_merge_base(ProllyCtx *ctx, const PHash *a, const PHash *b, PHash *out)
{
	return merge_base_core(ctx, a, b, NULL, NULL, out);
}

/* ----------------------------------------------------------- references */

static char *
branch_ref(const char *name)
{
	return psprintf(BRANCH_REF_PREFIX "%s", name);
}

static char *
commit_ref(const PHash *h)
{
	char		hex[PHASH_HEX_LEN + 1];

	phash_hex(h, hex);
	return psprintf(COMMIT_REF_PREFIX "%s", hex);
}

/* Whether a branch may be named so: 1 to NAMEDATALEN - 1 bytes. */
static bool
branch_name_ok(const char *name)
{
	return name[0] != '\0' && strlen(name) <= NAMEDATALEN - 1;
}

/*
 * A branch's chunk, of its head, working root and merge state, and its
 * name: no two branches share one, so moving one branch's reference lets
 * go of a chunk no other's holds (letting go of a root waits for any other
 * transaction changing that chunk's counts).
 */
static PHash
branch_put(ProllyCtx *ctx, const VcBranch *b)
{
	Buf			buf = BUF_INIT;
	PHash		h;

	buf_append_byte(&buf, CHUNK_TAG_BRANCH);
	buf_append_byte(&buf, CHUNK_FORMAT_VERSION);
	buf_append(&buf, b->head.b, PHASH_LEN);
	buf_append(&buf, b->working.b, PHASH_LEN);
	buf_append_byte(&buf, b->merging ? 1 : 0);
	if (b->merging)
		buf_append(&buf, b->merge_head.b, PHASH_LEN);
	buf_append_uvarint(&buf, strlen(b->name));
	buf_append(&buf, b->name, strlen(b->name));
	vc_must(ctx, prolly_put_chunk(ctx, buf.data, buf.len, &h));
	buf_free(&buf);
	return h;
}

static void
branch_decode(ProllyCtx *ctx, const PHash *h, VcBranch *b)
{
	uint32		len;
	uint8	   *d = cs_get(ctx->cs, h->b, &len);
	VcCommit	c;
	char		hex[PHASH_HEX_LEN + 1];
	const uint8 *p,
			   *end;
	uint64		nl = 0;
	bool		ok;

	/* (fixed fields, then the name, which must be the branch's own) */
	ok = d != NULL && len >= 2 + 2 * PHASH_LEN + 1 && d[0] == CHUNK_TAG_BRANCH &&
		d[1] == CHUNK_FORMAT_VERSION && d[2 + 2 * PHASH_LEN] <= 1;
	if (ok)
	{
		p = d + 3 + 2 * PHASH_LEN + (d[2 + 2 * PHASH_LEN] ? PHASH_LEN : 0);
		end = d + len;
		ok = p <= end && read_uvarint(&p, end, &nl) && nl == (uint64) (end - p) &&
			nl == strlen(b->name) && memcmp(p, b->name, nl) == 0;
	}
	if (!ok)
	{
		phash_hex(h, hex);
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("chunk %s, which a branch names, is no branch", hex)));
	}
	memcpy(b->head.b, d + 2, PHASH_LEN);
	memcpy(b->working.b, d + 2 + PHASH_LEN, PHASH_LEN);
	b->merging = d[2 + 2 * PHASH_LEN] == 1;
	if (b->merging)
		memcpy(b->merge_head.b, d + 3 + 2 * PHASH_LEN, PHASH_LEN);
	else
		memset(&b->merge_head, 0, sizeof(b->merge_head));
	pfree(d);
	c = must_load_commit(ctx, &b->head);
	b->head_root = c.root;
	vc_commit_release(&c);
}

/*
 * The branch's reference read, locked for this transaction if for_update;
 * false if there is none.
 */
static bool
branch_read(VcRepo *r, const char *name, bool for_update, VcBranch *out)
{
	char	   *ref;
	PHash		h;
	bool		found;

	if (!branch_name_ok(name))
		return false;
	ref = branch_ref(name);
	found = vc_refs_get(r->refs, ref, for_update, &h);

	pfree(ref);
	if (!found)
		return false;
	out->name = pstrdup(name);
	branch_decode(&r->ctx, &h, out);
	return true;
}

static void
branch_write(VcRepo *r, const VcBranch *b)
{
	char	   *ref = branch_ref(b->name);
	PHash		h = branch_put(&r->ctx, b);

	vc_refs_set(r->refs, ref, &h);
	pfree(ref);
}

/* A new branch's reference made; false if the branch is there already. */
static bool
branch_add(VcRepo *r, const VcBranch *b)
{
	char	   *ref = branch_ref(b->name);
	PHash		h = branch_put(&r->ctx, b);
	bool		added = vc_refs_add(r->refs, ref, &h);

	pfree(ref);
	return added;
}

/* The branch checked out, locked; an error if it is gone. */
static void
lock_head(VcRepo *r, VcBranch *out)
{
	if (!branch_read(r, r->cur, true, out))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("branch \"%s\", checked out, is gone", r->cur)));
}

bool
vc_in_window(VcRepo *r, const PHash *commit)
{
	char	   *ref = commit_ref(commit);
	PHash		h;
	bool		found = vc_refs_get(r->refs, ref, false, &h);

	pfree(ref);
	return found;
}

/* ----------------------------------------------------------------- repo */

static uint64
now_us(VcRepo *r)
{
	if (r->now)
		return r->now(r->now_arg);
	/* (microseconds since 1970, not 2000) */
	return (uint64) (GetCurrentTimestamp() +
					 (POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * USECS_PER_DAY);
}

void
vc_set_clock(VcRepo *r, uint64 (*now) (void *arg), void *arg)
{
	r->now = now;
	r->now_arg = arg;
}

/* Writes a commit and keeps it in the window's history, by its reference. */
static PHash
keep_commit(VcRepo *r, const PHash *root, const PHash *parents, uint32 np,
			const char *msg)
{
	PHash		h = vc_commit_write(&r->ctx, root, parents, np, now_us(r), msg);
	char	   *ref = commit_ref(&h);

	vc_refs_set(r->refs, ref, &h);
	pfree(ref);
	return h;
}

/*
 * Repos started separately must not share history by accident, so each
 * init commit is unique: no two in a backend get the same time.
 */
static uint64
unique_clock(VcRepo *r)
{
	static uint64 last = 0;
	uint64		t = now_us(r);

	if (t <= last)
		t = last + 1;
	last = t;
	return t;
}

void
vc_repo_open(VcRepo *r, ChunkStore *cs, VcRefs *refs, const ChunkPolicy *policy)
{
	vc_repo_open_clock(r, cs, refs, policy, NULL, NULL);
}

void
vc_repo_open_clock(VcRepo *r, ChunkStore *cs, VcRefs *refs,
				   const ChunkPolicy *policy, uint64 (*now) (void *arg),
				   void *now_arg)
{
	VcBranch	main;

	memset(r, 0, sizeof(*r));
	prolly_ctx_init(&r->ctx, cs, policy);
	r->refs = refs;
	r->now = now;
	r->now_arg = now_arg;
	r->cur = pstrdup("main");
	if (!branch_read(r, "main", false, &main))
	{
		PHash		root = vc_root_empty(&r->ctx);
		PHash		commit = vc_commit_write(&r->ctx, &root, NULL, 0, unique_clock(r),
											 "init");
		char	   *ref = commit_ref(&commit);
		bool		added = vc_refs_add(r->refs, ref, &commit);

		main.name = pstrdup("main");
		main.head = commit;
		main.head_root = root;
		main.working = root;
		main.merging = false;
		/*
		 * (of two opening an empty repository at once, one makes "main";
		 * the other's init commit is then let go of with its reference,
		 * unless the two wrote one commit, in one microsecond: then its
		 * reference is the first's, and stays)
		 */
		if (!branch_add(r, &main) && added)
			(void) vc_refs_remove(r->refs, ref);
		pfree(ref);
	}
	vc_branch_free(&main);
}

void
vc_repo_free(VcRepo *r)
{
	pfree(r->cur);
	prolly_ctx_free(&r->ctx);
	memset(r, 0, sizeof(*r));
}

bool
vc_branch_get(VcRepo *r, const char *name, VcBranch *out)
{
	return branch_read(r, name, false, out);
}

void
vc_branch_free(VcBranch *b)
{
	if (b->name)
		pfree(b->name);
	b->name = NULL;
}

typedef struct ListArg
{
	VcRepo	   *r;
	VcBranchFn	fn;
	void	   *arg;
} ListArg;

static void
list_fn(void *arg, const char *name, const PHash *hash)
{
	ListArg    *a = arg;
	VcBranch	b;

	b.name = pstrdup(name + strlen(BRANCH_REF_PREFIX));
	branch_decode(&a->r->ctx, hash, &b);
	a->fn(a->arg, &b);
	vc_branch_free(&b);
}

void
vc_branch_list(VcRepo *r, VcBranchFn fn, void *arg)
{
	ListArg		a = {r, fn, arg};

	vc_refs_list(r->refs, BRANCH_REF_PREFIX, list_fn, &a);
}

void
vc_head(VcRepo *r, VcBranch *out)
{
	if (!branch_read(r, r->cur, false, out))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("branch \"%s\", checked out, is gone", r->cur)));
}

PHash
vc_working(VcRepo *r)
{
	VcBranch	b;
	PHash		w;

	vc_head(r, &b);
	w = b.working;
	vc_branch_free(&b);
	return w;
}

void
vc_set_working(VcRepo *r, const PHash *rv)
{
	VcBranch	b;

	lock_head(r, &b);
	b.working = *rv;
	branch_write(r, &b);
	vc_branch_free(&b);
}

/* With VC_WINDOW_ONLY, a commit outside the window is refused, present or not. */
static int
reach_commit(VcRepo *r, const PHash *h, unsigned flags, VcCommit *c)
{
	if ((flags & VC_WINDOW_ONLY) && !vc_in_window(r, h))
	{
		r->ctx.err_hash = *h;
		r->ctx.err_has_hash = true;
		r->ctx.err = VC_ERR_OUTSIDE_WINDOW;
		return VC_ERR_OUTSIDE_WINDOW;
	}
	return load_commit(&r->ctx, h, c);
}

int
vc_branch_create(VcRepo *r, const char *name, const PHash *commit)
{
	VcBranch	cur,
				b;
	bool		added;

	if (!branch_name_ok(name))
		return VC_ERR;
	if (vc_branch_get(r, name, &cur))
	{
		vc_branch_free(&cur);
		return VC_ERR;
	}
	vc_head(r, &cur);
	b.name = pstrdup(name);
	b.head = cur.head;
	b.head_root = cur.head_root;
	b.merging = false;
	vc_branch_free(&cur);
	if (commit && !phash_eq(commit, &b.head))
	{
		VcCommit	c;
		int			rc;

		/* a commit outside the window must be attached before use */
		if ((rc = reach_commit(r, commit, VC_WINDOW_ONLY, &c)) != VC_OK)
		{
			vc_branch_free(&b);
			return rc;
		}
		b.head = *commit;
		b.head_root = c.root;
		vc_commit_release(&c);
	}
	b.working = b.head_root;
	/* (of two making one branch at once, one makes it, the other finds it) */
	added = branch_add(r, &b);
	vc_branch_free(&b);
	return added ? VC_OK : VC_ERR;
}

int
vc_branch_delete(VcRepo *r, const char *name)
{
	char	   *ref;
	bool		found;

	/* (main is never deleted: a repository's open finds it, or makes it) */
	if (strcmp(name, r->cur) == 0 || strcmp(name, "main") == 0 ||
		!branch_name_ok(name))
		return VC_ERR;
	ref = branch_ref(name);
	found = vc_refs_remove(r->refs, ref);
	pfree(ref);
	return found ? VC_OK : VC_ERR;
}

int
vc_checkout(VcRepo *r, const char *name)
{
	VcBranch	b;

	if (!vc_branch_get(r, name, &b))
		return VC_ERR;
	vc_branch_free(&b);
	pfree(r->cur);
	r->cur = pstrdup(name);
	return VC_OK;
}

/* Commits root on the locked branch b, which this moves. */
static PHash
commit_on(VcRepo *r, VcBranch *b, const PHash *root, const char *msg)
{
	PHash		parents[2] = {b->head, b->merge_head};
	PHash		h = keep_commit(r, root, parents, b->merging ? 2 : 1, msg);

	b->head = h;
	b->head_root = *root;
	b->merging = false;
	branch_write(r, b);
	return h;
}

PHash
vc_commit_root(VcRepo *r, const PHash *root, const char *msg)
{
	VcBranch	b;
	PHash		h;

	lock_head(r, &b);
	h = commit_on(r, &b, root, msg);
	vc_branch_free(&b);
	return h;
}

PHash
vc_commit(VcRepo *r, const char *msg)
{
	VcBranch	b;
	PHash		root,
				h;

	lock_head(r, &b);
	root = b.working;
	h = commit_on(r, &b, &root, msg);
	vc_branch_free(&b);
	return h;
}

int
vc_reset_hard(VcRepo *r, const PHash *commit)
{
	VcBranch	b;
	VcCommit	c;
	int			rc;

	if ((rc = reach_commit(r, commit, VC_WINDOW_ONLY, &c)) != VC_OK)
		return rc;
	lock_head(r, &b);
	b.head = *commit;
	b.head_root = c.root;
	b.working = c.root;
	b.merging = false;
	branch_write(r, &b);
	vc_branch_free(&b);
	vc_commit_release(&c);
	return VC_OK;
}

int
vc_reset_soft(VcRepo *r, const PHash *commit)
{
	VcBranch	b;
	VcCommit	c;
	int			rc;

	if ((rc = reach_commit(r, commit, VC_WINDOW_ONLY, &c)) != VC_OK)
		return rc;
	lock_head(r, &b);
	b.head = *commit;
	b.head_root = c.root;
	b.merging = false;
	branch_write(r, &b);
	vc_branch_free(&b);
	vc_commit_release(&c);
	return VC_OK;
}

int
vc_merge(VcRepo *r, const char *branch, VcMergeResult *res)
{
	VcBranch	b,
				other;
	PHash		ours,
				theirs,
				base,
				merged;
	VcCommit	cbase,
				ctheirs;
	int			rc;

	memset(res, 0, sizeof(*res));
	if (!vc_branch_get(r, branch, &other))
		return VC_ERR;			/* unknown branch */
	theirs = other.head;
	vc_branch_free(&other);
	lock_head(r, &b);
	if (b.merging || !phash_eq(&b.working, &b.head_root))
	{
		vc_branch_free(&b);
		return VC_ERR;			/* a merge pending, or uncommitted changes */
	}
	ours = b.head;
	rc = vc_merge_base(&r->ctx, &ours, &theirs, &base);
	if (rc != 1)
	{
		vc_branch_free(&b);
		return rc < 0 ? rc : VC_ERR;
	}
	if (phash_eq(&base, &theirs))
	{
		res->status = VC_MERGE_UP_TO_DATE;
		res->commit = ours;
		vc_branch_free(&b);
		return VC_OK;
	}
	if ((rc = load_commit(&r->ctx, &theirs, &ctheirs)) != VC_OK)
	{
		vc_branch_free(&b);
		return rc;
	}
	if (phash_eq(&base, &ours))
	{
		b.head = theirs;
		b.head_root = ctheirs.root;
		b.working = ctheirs.root;
		branch_write(r, &b);
		res->status = VC_MERGE_FAST_FORWARD;
		res->commit = theirs;
		vc_commit_release(&ctheirs);
		vc_branch_free(&b);
		return VC_OK;
	}
	/* the base may be beyond a cut even when both heads are not */
	if ((rc = load_commit(&r->ctx, &base, &cbase)) != VC_OK)
	{
		vc_commit_release(&ctheirs);
		vc_branch_free(&b);
		return rc;
	}
	vc_merge_roots(&r->ctx, &cbase.root, &b.head_root, &ctheirs.root, &merged,
				   &res->conflicts, &res->nconflicts);
	vc_commit_release(&cbase);
	vc_commit_release(&ctheirs);
	b.merging = true;
	b.merge_head = theirs;
	b.working = merged;
	if (res->nconflicts > 0)
	{
		branch_write(r, &b);
		res->status = VC_MERGE_CONFLICTS;
		vc_branch_free(&b);
		return VC_OK;
	}
	res->commit = commit_on(r, &b, &merged, "merge");
	res->status = VC_MERGE_COMMITTED;
	vc_branch_free(&b);
	return VC_OK;
}

void
vc_merge_result_free(VcMergeResult *res)
{
	vc_conflicts_free(res->conflicts, res->nconflicts);
	memset(res, 0, sizeof(*res));
}

static int
window_gate(void *arg, const PHash *h)
{
	VcRepo	   *r = arg;

	if (vc_in_window(r, h))
		return VC_OK;
	r->ctx.err_hash = *h;
	r->ctx.err_has_hash = true;
	r->ctx.err = VC_ERR_OUTSIDE_WINDOW;
	return VC_ERR_OUTSIDE_WINDOW;
}

int
vc_merge_base_in(VcRepo *r, const PHash *a, const PHash *b, unsigned flags,
				 PHash *out)
{
	bool		only = (flags & VC_WINDOW_ONLY) != 0;
	int			rc = merge_base_core(&r->ctx, a, b, only ? window_gate : NULL,
									 r, out);

	/*
	 * The walk may name a base past the edge from parent links alone; in
	 * window-only mode that is still outside, with the base in err_hash.
	 */
	if (rc == 1 && only && (rc = window_gate(r, out)) == VC_OK)
		rc = 1;
	return rc;
}

int
vc_diff_commits(VcRepo *r, const PHash *from, const PHash *to, unsigned flags,
				VcTableChangeFn fn, void *arg)
{
	VcCommit	a,
				b;
	int			rc;

	if ((rc = reach_commit(r, from, flags, &a)) != VC_OK)
		return rc;
	if ((rc = reach_commit(r, to, flags, &b)) != VC_OK)
	{
		vc_commit_release(&a);
		return rc;
	}
	vc_diff_tables(&r->ctx, &a.root, &b.root, fn, arg);
	vc_commit_release(&a);
	vc_commit_release(&b);
	return VC_OK;
}

int
vc_log(VcRepo *r, const PHash *start, size_t max, unsigned flags,
	   VcLogFn fn, void *arg)
{
	PHash		h = *start;

	for (size_t i = 0; i < max; i++)
	{
		VcCommit	c;
		int			rc;

		CHECK_FOR_INTERRUPTS();
		if ((rc = reach_commit(r, &h, flags, &c)) != VC_OK)
			return rc;
		fn(arg, &c);
		if (c.nparents == 0)
		{
			vc_commit_release(&c);
			break;
		}
		h = c.parents[0];
		vc_commit_release(&c);
	}
	return VC_OK;
}
