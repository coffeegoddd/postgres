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
#include "utils/builtins.h"
#include "utils/timestamp.h"

#define ENTRY_LEN (2 * PHASH_LEN)
#define RV_VREFS 2				/* a table entry holds its schema and data root */

#define BRANCH_REF_PREFIX "vc/branch/"
#define COMMIT_REF_PREFIX "vc/commit/"
#define REPO_REF "vc/repo"
#define SEGMENT_REF_PREFIX "vc/segment/"

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

/* The repository's own state, which vc/repo names. */
typedef struct RepoState
{
	PHash		loc_root;		/* where each commit outside the window is */
	uint64		next_seg;
	char	   *name;
} RepoState;

static void repo_share(VcRepo *r);
static PHash repo_put(ProllyCtx *ctx, const RepoState *st);

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
 * The branch's reference read, locked for this transaction as lock says;
 * false if there is none.
 */
static bool
branch_read(VcRepo *r, const char *name, VcRefLock lock, VcBranch *out)
{
	char	   *ref;
	PHash		h;
	bool		found;

	if (!branch_name_ok(name))
		return false;
	ref = branch_ref(name);
	found = vc_refs_get(r->refs, ref, lock, &h);

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
	if (!branch_read(r, r->cur, VC_REF_UPDATE, out))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("branch \"%s\", checked out, is gone", r->cur)));
}

bool
vc_in_window(VcRepo *r, const PHash *commit)
{
	char	   *ref = commit_ref(commit);
	PHash		h;
	bool		found = vc_refs_get(r->refs, ref, VC_REF_READ, &h);

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
	if (!branch_read(r, "main", VC_REF_READ, &main))
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
		/* the repository's own state: no commit outside the window yet */
		{
			RepoState	st;
			PHash		h;

			vc_must(&r->ctx, prolly_empty(&r->ctx, 0, &st.loc_root));
			st.next_seg = 0;
			st.name = "";
			h = repo_put(&r->ctx, &st);
			(void) vc_refs_add(r->refs, REPO_REF, &h);
		}

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
	return branch_read(r, name, VC_REF_READ, out);
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
	if (!branch_read(r, r->cur, VC_REF_READ, out))
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

PHash
vc_working_lock(VcRepo *r)
{
	VcBranch	b;
	PHash		w;

	lock_head(r, &b);
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
	repo_share(r);
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

	repo_share(r);
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

	repo_share(r);
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
	repo_share(r);
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

/* ----------------------------------------------------------- repository */


/*
 * Where a commit outside the window is, in the repository's location map:
 * a member of segment n (n + LOC_SEG_BASE), a parent of a commit held that
 * was discarded here (LOC_CUT), or a parent never had here (LOC_BOUNDARY).
 * A commit in the window has its own reference instead.
 */
#define LOC_GONE 0
#define LOC_SEG_BASE 2
#define LOC_CUT (PG_UINT64_MAX - 1)
#define LOC_BOUNDARY PG_UINT64_MAX



static PHash
repo_put(ProllyCtx *ctx, const RepoState *st)
{
	Buf			b = BUF_INIT;
	PHash		h;

	buf_append_byte(&b, CHUNK_TAG_REPO);
	buf_append_byte(&b, CHUNK_FORMAT_VERSION);
	buf_append(&b, st->loc_root.b, PHASH_LEN);
	buf_append_uvarint(&b, st->next_seg);
	buf_append_uvarint(&b, strlen(st->name));
	buf_append(&b, st->name, strlen(st->name));
	vc_must(ctx, prolly_put_chunk(ctx, b.data, b.len, &h));
	buf_free(&b);
	return h;
}

/* The repository's state, its reference locked as lock says. */
static void
repo_read(VcRepo *r, VcRefLock lock, RepoState *st)
{
	PHash		h;
	uint32		len;
	uint8	   *d;
	const uint8 *p,
			   *end;
	uint64		nl;
	char		hex[PHASH_HEX_LEN + 1];

	if (!vc_refs_get(r->refs, REPO_REF, lock, &h))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("the repository has no \"%s\" reference", REPO_REF)));
	d = cs_get(r->ctx.cs, h.b, &len);
	if (d == NULL || len < 2 + PHASH_LEN || d[0] != CHUNK_TAG_REPO ||
		d[1] != CHUNK_FORMAT_VERSION)
		goto bad;
	memcpy(st->loc_root.b, d + 2, PHASH_LEN);
	p = d + 2 + PHASH_LEN;
	end = d + len;
	if (!read_uvarint(&p, end, &st->next_seg) || !read_uvarint(&p, end, &nl) ||
		nl != (uint64) (end - p))
		goto bad;
	st->name = pnstrdup((const char *) p, nl);
	pfree(d);
	return;
bad:
	phash_hex(&h, hex);
	ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("chunk %s, which \"%s\" names, is no repository", hex, REPO_REF)));
}

static void
repo_write(VcRepo *r, const RepoState *st)
{
	PHash		h = repo_put(&r->ctx, st);

	vc_refs_set(r->refs, REPO_REF, &h);
}

/* (for a call pointing a branch at a commit it did not write) */
static void
repo_share(VcRepo *r)
{
	RepoState	st;

	repo_read(r, VC_REF_SHARE, &st);
	pfree(st.name);
}

void
vc_repo_set_name(VcRepo *r, const char *name)
{
	RepoState	st;

	repo_read(r, VC_REF_UPDATE, &st);
	pfree(st.name);
	st.name = pstrdup(name);
	repo_write(r, &st);
	pfree(st.name);
}

/* (a commit's location in the map, LOC_GONE if none) */
static uint64
loc_get(VcRepo *r, const PHash *root, const PHash *h)
{
	const uint8 *val;
	size_t		vl;
	uint64		out = LOC_GONE;
	int			rc = prolly_get(&r->ctx, root, h->b, PHASH_LEN, &val, &vl);

	if (rc == PROLLY_NOT_FOUND)
		return LOC_GONE;
	vc_must(&r->ctx, rc);
	{
		const uint8 *p = val;

		if (!read_uvarint(&p, val + vl, &out) || p != val + vl)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("a commit's location is not a number")));
	}
	px_free((void *) val);
	return out;
}

/* Changes to the location map, applied at once (LOC_GONE removes). */
typedef struct LocEdits
{
	ProllyEdit *e;
	uint8	   *keys;
	uint8	   *vals;
	size_t		n;
	size_t		cap;
} LocEdits;

static void
loc_set(LocEdits *le, const PHash *h, uint64 where)
{
	if (le->n == le->cap)
	{
		le->cap = Max(le->cap * 2, 16);
		le->e = px_realloc(le->e, le->cap * sizeof(ProllyEdit));
		le->keys = px_realloc(le->keys, le->cap * PHASH_LEN);
		le->vals = px_realloc(le->vals, le->cap * 10);
	}
	memcpy(le->keys + le->n * PHASH_LEN, h->b, PHASH_LEN);
	le->n++;
	{
		Buf			b = BUF_INIT;
		size_t		i = le->n - 1;

		buf_append_uvarint(&b, where);
		memcpy(le->vals + i * 10, b.data, b.len);
		le->e[i].vlen = b.len;
		buf_free(&b);
		le->e[i].del = where == LOC_GONE;
	}
}

static PHash
loc_apply(VcRepo *r, const PHash *root, LocEdits *le)
{
	PHash		out = *root;

	for (size_t i = 0; i < le->n; i++)
	{
		le->e[i].key = le->keys + i * PHASH_LEN;
		le->e[i].klen = PHASH_LEN;
		le->e[i].val = le->e[i].del ? NULL : le->vals + i * 10;
		if (le->e[i].del)
			le->e[i].vlen = 0;
	}
	if (le->n)
		vc_must(&r->ctx, prolly_apply(&r->ctx, root, le->e, le->n, &out));
	px_free(le->e);
	px_free(le->keys);
	px_free(le->vals);
	memset(le, 0, sizeof(*le));
	return out;
}

/* ---------------------------------------------------------------- window */

static bool
depth_only(const VcWindow *w)
{
	return w->depth && !w->keep_bytes && !w->keep_seconds && !w->max_commits &&
		!w->max_bytes && !w->max_seconds;
}

/* Whether a floor keeps a commit with these measures, against head H. */
static bool
floor_keeps(const VcWindow *w, const VcCommit *H, uint64 now, uint64 gen,
			uint64 bytes, uint64 time)
{
	if (!w->depth && !w->keep_bytes && !w->keep_seconds)
		return true;			/* no floor: the ceilings decide */
	return (w->depth && gen + w->depth > H->generation) ||
		(w->keep_bytes && bytes + w->keep_bytes > H->bytes) ||
		(w->keep_seconds && time + w->keep_seconds * 1000000 > now);
}

/*
 * Whether a ceiling cuts a commit; span is the change bytes of it and
 * every commit after it up to H.
 */
static bool
ceiling_cuts(const VcWindow *w, const VcCommit *H, uint64 now, uint64 gen,
			 uint64 span, uint64 time)
{
	return (w->max_commits && gen + w->max_commits <= H->generation) ||
		(w->max_bytes && span > w->max_bytes) ||
		(w->max_seconds && time + w->max_seconds * 1000000 <= now);
}

typedef struct HashStack
{
	PHash	   *v;
	size_t		n;
	size_t		cap;
} HashStack;

static void
stack_push(HashStack *s, const PHash *h)
{
	px_grow((void **) &s->v, &s->cap, s->n + 1, sizeof(PHash));
	s->v[s->n++] = *h;
}

/*
 * The window of one head under floors and ceilings.  Generation, change
 * bytes and time only fall along history, so once a commit is out, all
 * its ancestors are out on that path: the walk stops there.  A parent's
 * measures are in its child, so a parent no floor keeps is never read; one
 * a byte ceiling may cut is read for its own change bytes.
 */
static int
window_add_policy(VcRepo *r, PHashMap *win, const PHash *head, const VcWindow *w,
				  PHashMap *below)
{
	VcCommit	H,
				c;
	PHashMap	seen;
	HashStack	st = {0};
	uint64		now = now_us(r);
	int			rc = load_commit(&r->ctx, head, &H);
	bool		created;

	if (rc != VC_OK)
		return rc;
	phmap_init(&seen);
	stack_push(&st, head);
	while (st.n > 0)
	{
		PHash		h = st.v[--st.n];
		uint64		pb = 0;

		CHECK_FOR_INTERRUPTS();
		phmap_ref(&seen, &h, &created);
		if (!created)
			continue;
		/* a wider window can reach commits already cut; they stay cut */
		if (!vc_commit_load(&r->ctx, &h, &c))
			continue;
		for (uint32 i = 0; i < c.nparents; i++)
			if (c.parent_bytes[i] > pb)
				pb = c.parent_bytes[i];
		if (!phash_eq(&h, head) &&
			ceiling_cuts(w, &H, now, c.generation, H.bytes - pb, c.time))
		{
			if (below)
				phmap_ref(below, &h, &created);
			vc_commit_release(&c);
			continue;
		}
		phmap_ref(win, &h, &created);
		for (uint32 i = 0; i < c.nparents; i++)
		{
			if (!floor_keeps(w, &H, now, c.parent_gen[i], c.parent_bytes[i], c.parent_time[i]) ||
				ceiling_cuts(w, &H, now, c.parent_gen[i], H.bytes - c.parent_bytes[i],
							 c.parent_time[i]))
			{
				if (below)
					phmap_ref(below, &c.parents[i], &created);
				continue;
			}
			stack_push(&st, &c.parents[i]);
		}
		vc_commit_release(&c);
	}
	vc_commit_release(&H);
	px_free(st.v);
	phmap_free(&seen);
	return VC_OK;
}

/*
 * Adds to win every commit in head's window.  With only a depth, those
 * with generation > gen(head) - depth on some path from it: win maps each
 * commit to 1 + the lowest generation bound it was expanded under, so a
 * commit reached again under a lower bound is expanded again, and heads
 * sharing history share the walk.  Parent generations live in the child,
 * so nothing outside the window is read.  below collects the parents the
 * window stops at.
 */
static int
window_add(VcRepo *r, PHashMap *win, const PHash *head, const VcWindow *w, PHashMap *below)
{
	uint64		depth = w->depth;
	VcCommit	c;
	HashStack	st = {0};
	uint64		bound;
	int			rc;

	if (!depth_only(w))
		return window_add_policy(r, win, head, w, below);
	if ((rc = load_commit(&r->ctx, head, &c)) != VC_OK)
		return rc;
	bound = c.generation > depth ? c.generation - depth : 0;
	vc_commit_release(&c);
	stack_push(&st, head);
	while (st.n > 0)
	{
		PHash		h = st.v[--st.n];
		bool		created;
		uint64	   *v = phmap_ref(win, &h, &created);

		CHECK_FOR_INTERRUPTS();
		if (!created && *v <= bound + 1)
			continue;
		*v = bound + 1;
		/* a wider window can reach commits already cut; they stay cut */
		if (!vc_commit_load(&r->ctx, &h, &c))
			continue;
		for (uint32 i = 0; i < c.nparents; i++)
			if (c.parent_gen[i] > bound)
				stack_push(&st, &c.parents[i]);
			else if (below)
				phmap_ref(below, &c.parents[i], &created);
		vc_commit_release(&c);
	}
	px_free(st.v);
	return VC_OK;
}

static bool
has_low_marks(const VcWindow *w)
{
	return w->low_commits || w->low_bytes || w->low_seconds;
}

static uint64
tighter(uint64 limit, uint64 mark)
{
	if (mark == 0)
		return limit;
	if (limit == 0)
		return mark;
	return mark < limit ? mark : limit;
}

/* The window a head at its high mark is cut back to: the low marks as ceilings too. */
static VcWindow
low_window(const VcWindow *w)
{
	VcWindow	low = *w;

	low.max_commits = tighter(w->max_commits, w->low_commits);
	low.max_bytes = tighter(w->max_bytes, w->low_bytes);
	low.max_seconds = tighter(w->max_seconds, w->low_seconds);
	low.low_commits = low.low_bytes = low.low_seconds = 0;
	return low;
}

/*
 * Whether head's window has reached its high mark: it would let go of a
 * commit still held (one it stops at, cut by a ceiling or past the floors).
 */
static int
at_high_mark(VcRepo *r, const PHash *head, const VcWindow *w, bool *over)
{
	PHashMap	win,
				below;
	int			rc;

	phmap_init(&win);
	phmap_init(&below);
	rc = window_add(r, &win, head, w, &below);
	*over = false;
	for (size_t i = 0; rc == VC_OK && i < below.cap && !*over; i++)
		if (below.used[i] && !phmap_find(&win, &below.keys[i]) &&
			vc_in_window(r, &below.keys[i]))
			*over = true;
	phmap_free(&win);
	phmap_free(&below);
	return rc;
}

/* Adds head's window to win: its high mark's, or its low mark's once it reached the high one. */
static int
window_add_marked(VcRepo *r, PHashMap *win, const PHash *head, const VcWindow *w,
				  PHashMap *below)
{
	VcWindow	high = *w,
				low;
	bool		over;
	int			rc;

	high.low_commits = high.low_bytes = high.low_seconds = 0;
	if (!has_low_marks(w))
		return window_add(r, win, head, &high, below);
	if ((rc = at_high_mark(r, head, &high, &over)) != VC_OK)
		return rc;
	if (!over)
		return window_add(r, win, head, &high, below);
	low = low_window(w);
	return window_add(r, win, head, &low, below);
}

/* (each branch's head and merge head, for the window) */
typedef struct HeadsArg
{
	HashStack	heads;
} HeadsArg;

static void
heads_fn(void *arg, const VcBranch *b)
{
	HeadsArg   *a = arg;

	stack_push(&a->heads, &b->head);
	if (b->merging)
		stack_push(&a->heads, &b->merge_head);
}

static bool
hex_hash(const char *hex, PHash *out)
{
	if (strlen(hex) != PHASH_HEX_LEN)
		return false;
	return hex_decode(hex, PHASH_HEX_LEN, (char *) out->b) == PHASH_LEN;
}

/* (every commit with a reference: the window's history) */
static void
held_fn(void *arg, const char *name, const PHash *hash)
{
	HashStack  *held = arg;
	PHash		h;

	if (!hex_hash(name + strlen(COMMIT_REF_PREFIX), &h) || !phash_eq(&h, hash))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("reference \"%s\" names another commit", name)));
	stack_push(held, &h);
}

static char *
segment_ref(uint32 id)
{
	return psprintf(SEGMENT_REF_PREFIX "%u", id);
}

/* A segment's chunk, of its members, edge and (exported) archive. */
static PHash
segment_put(ProllyCtx *ctx, const VcSegment *seg)
{
	Buf			b = BUF_INIT;
	PHash		h;

	buf_append_byte(&b, CHUNK_TAG_SEGMENT);
	buf_append_byte(&b, CHUNK_FORMAT_VERSION);
	buf_append_byte(&b, seg->exported ? 1 : 0);
	buf_append_uvarint(&b, seg->n);
	for (size_t i = 0; i < seg->n; i++)
		buf_append(&b, seg->commits[i].b, PHASH_LEN);
	buf_append_uvarint(&b, seg->nedge);
	for (size_t i = 0; i < seg->nedge; i++)
		buf_append(&b, seg->edge[i].b, PHASH_LEN);
	if (seg->exported)
	{
		buf_append_uvarint(&b, strlen(seg->archive_name));
		buf_append(&b, seg->archive_name, strlen(seg->archive_name));
		buf_append(&b, seg->manifest.b, PHASH_LEN);
	}
	vc_must(ctx, prolly_put_chunk(ctx, b.data, b.len, &h));
	buf_free(&b);
	return h;
}

static void
segment_write(VcRepo *r, const VcSegment *seg)
{
	char	   *ref = segment_ref(seg->id);
	PHash		h = segment_put(&r->ctx, seg);

	vc_refs_set(r->refs, ref, &h);
	pfree(ref);
}

static bool
read_hashes(const uint8 **p, const uint8 *end, PHash **out, size_t *n)
{
	uint64		count;

	if (!read_uvarint(p, end, &count) || count > (uint64) (end - *p) / PHASH_LEN)
		return false;
	*out = palloc_array(PHash, Max(count, 1));
	for (uint64 i = 0; i < count; i++)
	{
		memcpy((*out)[i].b, *p, PHASH_LEN);
		*p += PHASH_LEN;
	}
	*n = (size_t) count;
	return true;
}

bool
vc_segment_get(VcRepo *r, uint32 id, VcSegment *out)
{
	char	   *ref = segment_ref(id);
	PHash		h;
	bool		found = vc_refs_get(r->refs, ref, VC_REF_READ, &h);
	uint32		len;
	uint8	   *d;
	const uint8 *p,
			   *end;
	char		hex[PHASH_HEX_LEN + 1];

	pfree(ref);
	if (!found)
		return false;
	memset(out, 0, sizeof(*out));
	out->id = id;
	d = cs_get(r->ctx.cs, h.b, &len);
	if (d == NULL || len < 3 || d[0] != CHUNK_TAG_SEGMENT ||
		d[1] != CHUNK_FORMAT_VERSION || d[2] > 1)
		goto bad;
	out->exported = d[2] == 1;
	p = d + 3;
	end = d + len;
	if (!read_hashes(&p, end, &out->commits, &out->n) ||
		!read_hashes(&p, end, &out->edge, &out->nedge))
		goto bad;
	if (out->exported)
	{
		uint64		nl;

		if (!read_uvarint(&p, end, &nl) || nl > (uint64) (end - p))
			goto bad;
		out->archive_name = pnstrdup((const char *) p, nl);
		p += nl;
		if (end - p != PHASH_LEN)
			goto bad;
		memcpy(out->manifest.b, p, PHASH_LEN);
		p += PHASH_LEN;
	}
	if (p != end)
		goto bad;
	pfree(d);
	return true;
bad:
	phash_hex(&h, hex);
	ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("chunk %s, which segment %u names, is no segment", hex, id)));
}

void
vc_segment_free(VcSegment *seg)
{
	if (seg->commits)
		pfree(seg->commits);
	if (seg->edge)
		pfree(seg->edge);
	if (seg->archive_name)
		pfree(seg->archive_name);
	memset(seg, 0, sizeof(*seg));
}

int
vc_sever(VcRepo *r, const VcWindow *w, uint32 *seg_out)
{
	RepoState	st;
	PHashMap	win,
				below;
	HeadsArg	ha = {{0}};
	HashStack	held = {0};
	VcSegment	seg = {0};
	LocEdits	le = {0};
	size_t		cap = 0,
				ecap = 0;
	int			rc = VC_OK;

	*seg_out = VC_NO_SEGMENT;
	if (!w->depth && !w->keep_bytes && !w->keep_seconds && !w->max_commits &&
		!w->max_bytes && !w->max_seconds)
		return VC_ERR;			/* no window at all, or low marks alone */

	/*
	 * (locked first: a call moving a branch to a commit it did not write
	 * holds this for share, so none is in flight while the heads are read)
	 */
	repo_read(r, VC_REF_UPDATE, &st);
	phmap_init(&win);
	phmap_init(&below);
	vc_branch_list(r, heads_fn, &ha);
	for (size_t i = 0; i < ha.heads.n && rc == VC_OK; i++)
		rc = window_add_marked(r, &win, &ha.heads.v[i], w, &below);
	for (size_t i = 0; i < w->npinned && rc == VC_OK; i++)
		rc = window_add(r, &win, &w->pinned[i], &(VcWindow) {.depth = 1}, &below);
	px_free(ha.heads.v);
	if (rc != VC_OK)
	{
		phmap_free(&win);
		phmap_free(&below);
		pfree(st.name);
		return rc;
	}

	/* everything the window's history holds that the window no longer covers */
	vc_refs_list(r->refs, COMMIT_REF_PREFIX, held_fn, &held);
	for (size_t i = 0; i < held.n; i++)
	{
		if (phmap_find(&win, &held.v[i]))
			continue;
		px_grow((void **) &seg.commits, &cap, seg.n + 1, sizeof(PHash));
		seg.commits[seg.n++] = held.v[i];
		/* a member the window's commits name as a parent is its edge */
		if (phmap_find(&below, &held.v[i]))
		{
			px_grow((void **) &seg.edge, &ecap, seg.nedge + 1, sizeof(PHash));
			seg.edge[seg.nedge++] = held.v[i];
		}
	}
	px_free(held.v);
	phmap_free(&win);
	phmap_free(&below);
	if (seg.n == 0)
	{
		pfree(st.name);
		return VC_OK;
	}

	/*
	 * Detached: the segment's reference holds them before theirs go, so no
	 * chunk is read, copied or freed
	 */
	seg.id = (uint32) st.next_seg;
	segment_write(r, &seg);
	for (size_t i = 0; i < seg.n; i++)
	{
		char	   *ref = commit_ref(&seg.commits[i]);

		(void) vc_refs_remove(r->refs, ref);
		pfree(ref);
		loc_set(&le, &seg.commits[i], LOC_SEG_BASE + seg.id);
	}
	st.loc_root = loc_apply(r, &st.loc_root, &le);
	st.next_seg++;
	repo_write(r, &st);
	*seg_out = seg.id;
	px_free(seg.commits);
	px_free(seg.edge);
	pfree(st.name);
	return VC_OK;
}

/* Commits of one segment that are not members: its boundary edges. */
static void
segment_bounds(ProllyCtx *ctx, const VcSegment *seg, PHash **out, size_t *nout)
{
	size_t		cap = 0;
	PHashMap	mem,
				seen;
	bool		created;

	phmap_init(&mem);
	phmap_init(&seen);
	for (size_t i = 0; i < seg->n; i++)
		phmap_ref(&mem, &seg->commits[i], &created);
	*out = NULL;
	*nout = 0;
	for (size_t i = 0; i < seg->n; i++)
	{
		VcCommit	c;

		if (!vc_commit_load(ctx, &seg->commits[i], &c))
			continue;
		for (uint32 j = 0; j < c.nparents; j++)
		{
			if (phmap_find(&mem, &c.parents[j]))
				continue;
			phmap_ref(&seen, &c.parents[j], &created);
			if (!created)
				continue;
			px_grow((void **) out, &cap, *nout + 1, sizeof(PHash));
			(*out)[(*nout)++] = c.parents[j];
		}
		vc_commit_release(&c);
	}
	phmap_free(&mem);
	phmap_free(&seen);
}

/*
 * Exports a segment: copies each member with its whole tree to the
 * archive, then writes a manifest there that names the members, their
 * generations, the segment's boundary and the source repository, and holds
 * them.  Only then does the segment let go of them here, which truncates
 * local history to the window.
 */
int
vc_segment_export(VcRepo *r, uint32 id, const VcSource *archive)
{
	RepoState	st;
	VcSegment	seg;
	PHash	   *bounds;
	size_t		nb;
	Buf			b = BUF_INIT;
	PHash		manifest;

	if (archive->cs == r->ctx.cs)
		return VC_ERR;
	repo_read(r, VC_REF_UPDATE, &st);
	if (!vc_segment_get(r, id, &seg) || seg.exported)
	{
		pfree(st.name);
		return VC_ERR;
	}
	(void) cs_copy_closure(r->ctx.cs, archive->cs, seg.commits[0].b, (int) seg.n,
						   "export", "the repository's store");
	segment_bounds(&r->ctx, &seg, &bounds, &nb);
	buf_append_byte(&b, CHUNK_TAG_MANIFEST);
	buf_append_byte(&b, CHUNK_FORMAT_VERSION);
	buf_append_uvarint(&b, strlen(st.name));
	buf_append(&b, st.name, strlen(st.name));
	buf_append_uvarint(&b, seg.n);
	for (size_t i = 0; i < seg.n; i++)
	{
		VcCommit	c;

		if (!vc_commit_load(&r->ctx, &seg.commits[i], &c))
			commit_corrupt(&seg.commits[i]);
		buf_append(&b, seg.commits[i].b, PHASH_LEN);
		buf_append_uvarint(&b, c.generation);
		vc_commit_release(&c);
	}
	buf_append_uvarint(&b, nb);
	for (size_t i = 0; i < nb; i++)
		buf_append(&b, bounds[i].b, PHASH_LEN);
	px_free(bounds);
	buf_append_uvarint(&b, 0);	/* (no keyframes) */
	(void) cs_put(archive->cs, b.data, (uint32) b.len, manifest.b);
	buf_free(&b);
	cs_ref_root(archive->cs, manifest.b);

	seg.exported = true;
	seg.archive_name = pstrdup(archive->name);
	seg.manifest = manifest;
	segment_write(r, &seg);
	vc_segment_free(&seg);
	pfree(st.name);
	return VC_OK;
}

/*
 * Discards a segment.  A local one is let go of here; an exported one is
 * let go of in its archive, which frees exactly what no other manifest
 * there holds.
 */
int
vc_segment_discard(VcRepo *r, uint32 id, ChunkStore *archive)
{
	RepoState	st;
	VcSegment	seg;
	LocEdits	le = {0};
	ProllyCtx	actx;
	ProllyCtx  *from = &r->ctx;
	char	   *ref;

	repo_read(r, VC_REF_UPDATE, &st);
	if (!vc_segment_get(r, id, &seg) || (seg.exported && archive == NULL))
	{
		pfree(st.name);
		return VC_ERR;
	}
	if (seg.exported)
	{
		prolly_ctx_init(&actx, archive, &r->ctx.policy);
		from = &actx;
	}

	/*
	 * Forgotten, but for what held commits name of them: the segment's
	 * edge.  Whatever its members named as cut is a level deeper now.
	 */
	for (size_t i = 0; i < seg.n; i++)
	{
		VcCommit	c;

		if (loc_get(r, &st.loc_root, &seg.commits[i]) == LOC_SEG_BASE + id)
			loc_set(&le, &seg.commits[i], LOC_GONE);
		if (vc_commit_load(from, &seg.commits[i], &c))
		{
			for (uint32 j = 0; j < c.nparents; j++)
				if (loc_get(r, &st.loc_root, &c.parents[j]) == LOC_CUT)
					loc_set(&le, &c.parents[j], LOC_GONE);
			vc_commit_release(&c);
		}
	}
	for (size_t i = 0; i < seg.nedge; i++)
		loc_set(&le, &seg.edge[i], LOC_CUT);
	if (from == &actx)
		prolly_ctx_free(&actx);
	st.loc_root = loc_apply(r, &st.loc_root, &le);
	repo_write(r, &st);
	if (seg.exported)
		cs_unref_root(archive, seg.manifest.b);
	ref = segment_ref(id);
	(void) vc_refs_remove(r->refs, ref);
	pfree(ref);
	vc_segment_free(&seg);
	pfree(st.name);
	return VC_OK;
}

int
vc_segment_attach(VcRepo *r, uint32 id, ChunkStore *archive)
{
	RepoState	st;
	VcSegment	seg;
	LocEdits	le = {0};
	char	   *ref;

	repo_read(r, VC_REF_UPDATE, &st);
	if (!vc_segment_get(r, id, &seg) || (seg.exported && archive == NULL))
	{
		pfree(st.name);
		return VC_ERR;
	}
	/* an exported one's chunks copied back first, each checked */
	if (seg.exported)
		(void) cs_copy_closure(archive, r->ctx.cs, seg.commits[0].b, (int) seg.n,
							   "attach", "the archive");
	/* the holds move back to the window's history */
	for (size_t i = 0; i < seg.n; i++)
	{
		ref = commit_ref(&seg.commits[i]);
		vc_refs_set(r->refs, ref, &seg.commits[i]);
		pfree(ref);
		if (loc_get(r, &st.loc_root, &seg.commits[i]) == LOC_SEG_BASE + id)
			loc_set(&le, &seg.commits[i], LOC_GONE);
	}
	st.loc_root = loc_apply(r, &st.loc_root, &le);
	repo_write(r, &st);
	if (seg.exported)
		cs_unref_root(archive, seg.manifest.b);
	ref = segment_ref(id);
	(void) vc_refs_remove(r->refs, ref);
	pfree(ref);
	vc_segment_free(&seg);
	pfree(st.name);
	return VC_OK;
}

VcLocation
vc_locate(VcRepo *r, const PHash *h, uint32 *seg)
{
	RepoState	st;
	uint64		v;
	VcSegment	s;
	VcLocation	l;

	if (vc_in_window(r, h))
		return VC_LOC_WINDOW;
	repo_read(r, VC_REF_READ, &st);
	v = loc_get(r, &st.loc_root, h);
	pfree(st.name);
	if (v == LOC_GONE)
		return VC_LOC_UNKNOWN;
	if (v == LOC_CUT)
		return VC_LOC_DISCARDED;
	if (v == LOC_BOUNDARY)
		return VC_LOC_BOUNDARY;
	if (seg)
		*seg = (uint32) (v - LOC_SEG_BASE);
	if (!vc_segment_get(r, (uint32) (v - LOC_SEG_BASE), &s))
		return VC_LOC_UNKNOWN;
	l = s.exported ? VC_LOC_EXPORTED : VC_LOC_SEGMENT;
	vc_segment_free(&s);
	return l;
}

typedef struct LocsArg
{
	VcRepo	   *r;
	VcLocFn		fn;
	void	   *arg;
} LocsArg;

static void
window_loc_fn(void *arg, const char *name, const PHash *hash)
{
	LocsArg    *a = arg;

	a->fn(a->arg, hash, VC_LOC_WINDOW, VC_NO_SEGMENT);
}

void
vc_locations(VcRepo *r, VcLocFn fn, void *arg)
{
	LocsArg		a = {r, fn, arg};
	RepoState	st;
	ProllyIter *it;
	const uint8 *k,
			   *v;
	size_t		kl,
				vl;
	int			rc;

	vc_refs_list(r->refs, COMMIT_REF_PREFIX, window_loc_fn, &a);
	repo_read(r, VC_REF_READ, &st);
	vc_must(&r->ctx, prolly_iter_open(&r->ctx, &st.loc_root, NULL, 0, &it));
	while ((rc = prolly_iter_next(it, &k, &kl, &v, &vl)) == 1)
	{
		PHash		h;
		uint32		seg = VC_NO_SEGMENT;
		VcLocation	l;

		if (kl != PHASH_LEN)
			continue;
		memcpy(h.b, k, PHASH_LEN);
		l = vc_locate(r, &h, &seg);
		fn(arg, &h, l, l == VC_LOC_SEGMENT || l == VC_LOC_EXPORTED ? seg : VC_NO_SEGMENT);
	}
	prolly_iter_close(it);
	if (rc < 0)
		prolly_report(&r->ctx);
	pfree(st.name);
}

/* --------------------------------------------------------------- archives */

typedef struct ManifestAcc
{
	ChunkStore *cs;
	PHash	   *v;
	size_t		n;
	size_t		cap;
} ManifestAcc;

static void
manifest_visit(void *arg, const uint8 *hash, uint32 len, const ChunkCounts *counts)
{
	ManifestAcc *a = arg;
	uint32		l;
	uint8	   *d;

	if (counts->roots == 0)
		return;
	d = cs_get(a->cs, hash, &l);
	if (d && l >= 2 && d[0] == CHUNK_TAG_MANIFEST && d[1] == CHUNK_FORMAT_VERSION)
	{
		px_grow((void **) &a->v, &a->cap, a->n + 1, sizeof(PHash));
		memcpy(a->v[a->n++].b, hash, PHASH_LEN);
	}
	if (d)
		pfree(d);
}

void
vc_archive_list(ChunkStore *archive, PHash **manifests, size_t *n)
{
	ManifestAcc a = {archive, NULL, 0, 0};

	cs_visit(archive, manifest_visit, &a);
	*manifests = a.v;
	*n = a.n;
}

bool
vc_manifest_load(ChunkStore *archive, const PHash *h, VcManifest *m)
{
	uint32		len;
	uint8	   *d = cs_get(archive, h->b, &len);
	const uint8 *p,
			   *end;
	uint64		nl,
				n;

	memset(m, 0, sizeof(*m));
	if (d == NULL || len < 2 || d[0] != CHUNK_TAG_MANIFEST || d[1] != CHUNK_FORMAT_VERSION)
		goto bad;
	p = d + 2;
	end = d + len;
	if (!read_uvarint(&p, end, &nl) || nl > (uint64) (end - p))
		goto bad;
	m->repo = pnstrdup((const char *) p, nl);
	p += nl;
	if (!read_uvarint(&p, end, &n) || n > (uint64) (end - p) / (PHASH_LEN + 1))
		goto bad;
	m->members = palloc_array(PHash, Max(n, 1));
	m->gens = palloc_array(uint64, Max(n, 1));
	m->n = n;
	for (uint64 i = 0; i < n; i++)
	{
		if (end - p < PHASH_LEN)
			goto bad;
		memcpy(m->members[i].b, p, PHASH_LEN);
		p += PHASH_LEN;
		if (!read_uvarint(&p, end, &m->gens[i]))
			goto bad;
	}
	if (!read_hashes(&p, end, &m->bounds, &m->nb) || !read_uvarint(&p, end, &n) || n != 0 ||
		p != end)
		goto bad;
	pfree(d);
	return true;
bad:
	if (d)
		pfree(d);
	vc_manifest_free(m);
	return false;
}

void
vc_manifest_free(VcManifest *m)
{
	if (m->repo)
		pfree(m->repo);
	if (m->members)
		pfree(m->members);
	if (m->gens)
		pfree(m->gens);
	if (m->bounds)
		pfree(m->bounds);
	memset(m, 0, sizeof(*m));
}
