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

#include "access/xact.h"
#include "miscadmin.h"
#include "prolly/engine/vc.h"
#include "utils/builtins.h"
#include "utils/injection_point.h"
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
	/* (the nodes it reads again kept parsed: an operation's work_mem of them) */
	ctx->memo_bytes = (size_t) work_mem * 1024;
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
	uint64		forgot_gen;		/* the highest generation a discard forgot */
	uint64		history;		/* VcHistoryMode */
	char	   *name;
	/* where boundaries' content can be found, as remotes said: their hints */
	char	  **hints;
	uint32		nhints;
} RepoState;

static void
repo_state_free(RepoState *st)
{
	pfree(st->name);
	for (uint32 i = 0; i < st->nhints; i++)
		pfree(st->hints[i]);
	if (st->hints)
		pfree(st->hints);
}

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
			st.forgot_gen = 0;
			st.history = VC_HISTORY_KEEP;
			st.name = "";
			st.hints = NULL;
			st.nhints = 0;
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

	/* (the branch first, then the repository: every call's order) */
	lock_head(r, &b);
	repo_share(r);
	if ((rc = reach_commit(r, commit, VC_WINDOW_ONLY, &c)) != VC_OK)
	{
		vc_branch_free(&b);
		return rc;
	}
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

	/* (the branch first, then the repository: every call's order) */
	lock_head(r, &b);
	repo_share(r);
	if ((rc = reach_commit(r, commit, VC_WINDOW_ONLY, &c)) != VC_OK)
	{
		vc_branch_free(&b);
		return rc;
	}
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
	/* (the branch first, then the repository: every call's order) */
	lock_head(r, &b);
	repo_share(r);
	if (!vc_branch_get(r, branch, &other))
	{
		vc_branch_free(&b);
		return VC_ERR;			/* unknown branch */
	}
	theirs = other.head;
	vc_branch_free(&other);
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
 * a member of segment n, LOC_SEG_BASE + 2n, plus one if it is exported (so
 * that where a commit is costs the map alone), or a parent never had here,
 * a boundary: LOC_BOUND_BASE + the number of its hint (1 for the state's
 * first; 0, no hint).  A commit in the window has its own reference
 * instead, and a parent of a commit held that was discarded here is found
 * from what is held when asked, not kept.
 */
#define LOC_GONE 0
#define LOC_SEG_BASE 2
#define LOC_BOUND_BASE (UINT64CONST(1) << 62)
#define LOC_BOUND(hint) (LOC_BOUND_BASE + (uint64) (hint))
#define LOC_IS_BOUND(v) ((v) >= LOC_BOUND_BASE)
#define LOC_BOUND_HINT(v) ((uint32) ((v) - LOC_BOUND_BASE))
#define LOC_SEG(id, exported) (LOC_SEG_BASE + 2 * (uint64) (id) + ((exported) ? 1 : 0))
#define LOC_IS_SEG(v) ((v) >= LOC_SEG_BASE && (v) < LOC_BOUND_BASE)
#define LOC_SEG_ID(v) ((uint32) (((v) - LOC_SEG_BASE) / 2))
#define LOC_SEG_EXPORTED(v) ((((v) - LOC_SEG_BASE) & 1) == 1)



static PHash
repo_put(ProllyCtx *ctx, const RepoState *st)
{
	Buf			b = BUF_INIT;
	PHash		h;

	buf_append_byte(&b, CHUNK_TAG_REPO);
	buf_append_byte(&b, CHUNK_FORMAT_VERSION);
	buf_append(&b, st->loc_root.b, PHASH_LEN);
	buf_append_uvarint(&b, st->next_seg);
	buf_append_uvarint(&b, st->forgot_gen);
	buf_append_uvarint(&b, st->history);
	buf_append_uvarint(&b, strlen(st->name));
	buf_append(&b, st->name, strlen(st->name));
	buf_append_uvarint(&b, st->nhints);
	for (uint32 i = 0; i < st->nhints; i++)
	{
		buf_append_uvarint(&b, strlen(st->hints[i]));
		buf_append(&b, st->hints[i], strlen(st->hints[i]));
	}
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
	uint64		nl,
				nh;
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
	if (!read_uvarint(&p, end, &st->next_seg) || !read_uvarint(&p, end, &st->forgot_gen) ||
		!read_uvarint(&p, end, &st->history) || st->history > VC_HISTORY_REWRITE ||
		!read_uvarint(&p, end, &nl) || nl > (uint64) (end - p))
		goto bad;
	st->name = pnstrdup((const char *) p, nl);
	p += nl;
	st->hints = NULL;
	st->nhints = 0;
	if (!read_uvarint(&p, end, &nh) || nh > (uint64) (end - p))
		goto bad_state;
	st->hints = palloc_array(char *, Max(nh, 1));
	for (; st->nhints < nh; st->nhints++)
	{
		if (!read_uvarint(&p, end, &nl) || nl > (uint64) (end - p))
			goto bad_state;
		st->hints[st->nhints] = pnstrdup((const char *) p, nl);
		p += nl;
	}
	if (p != end)
		goto bad_state;
	pfree(d);
	return;
bad_state:
	repo_state_free(st);
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
	repo_state_free(&st);
}

void
vc_repo_set_name(VcRepo *r, const char *name)
{
	RepoState	st;

	repo_read(r, VC_REF_UPDATE, &st);
	pfree(st.name);
	st.name = pstrdup(name);
	repo_write(r, &st);
	repo_state_free(&st);
}

void
vc_set_history_mode(VcRepo *r, VcHistoryMode mode)
{
	RepoState	st;

	repo_read(r, VC_REF_UPDATE, &st);
	if (st.history != (uint64) mode)
	{
		st.history = mode;
		repo_write(r, &st);
	}
	repo_state_free(&st);
}

VcHistoryMode
vc_history_mode(VcRepo *r)
{
	RepoState	st;
	VcHistoryMode mode;

	repo_read(r, VC_REF_READ, &st);
	mode = (VcHistoryMode) st.history;
	repo_state_free(&st);
	return mode;
}

void
vc_set_rewrite_hook(VcRepo *r, VcRewriteFn fn, void *arg)
{
	r->rewrite_fn = fn;
	r->rewrite_arg = arg;
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
 * Whether a commit a window's walk reaches is held: in the sever's listing
 * of the commits with references, or (one committed since the listing,
 * below a head that moved on more than a commit) with a reference now.
 * One without is cut already, though its chunks may not yet be freed: a
 * walk stops there, and it stays cut.
 */
static bool
walk_held(VcRepo *r, PHashMap *held, const PHash *h)
{
	return phmap_find(held, h) || vc_in_window(r, h);
}

/*
 * The window of one head under floors and ceilings.  Generation, change
 * bytes and time only fall along history, so once a commit is out, all
 * its ancestors are out on that path: the walk stops there.  A parent's
 * measures are in its child, so a parent no floor keeps is never read; one
 * a byte ceiling may cut is read for its own change bytes.  The walk stops
 * too at a commit already cut (walk_held).
 */
static int
window_add_policy(VcRepo *r, PHashMap *win, const PHash *head, const VcWindow *w,
				  PHashMap *held, PHashMap *below)
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
		if (!phash_eq(&h, head) && !walk_held(r, held, &h))
			continue;
		c = must_load_commit(&r->ctx, &h);
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
 * window stops at; it stops too at a commit already cut (walk_held).
 */
static int
window_add(VcRepo *r, PHashMap *win, const PHash *head, const VcWindow *w,
		   PHashMap *held, PHashMap *below)
{
	uint64		depth = w->depth;
	VcCommit	c;
	HashStack	st = {0};
	uint64		bound;
	int			rc;

	if (!depth_only(w))
		return window_add_policy(r, win, head, w, held, below);
	if ((rc = load_commit(&r->ctx, head, &c)) != VC_OK)
		return rc;
	bound = c.generation > depth ? c.generation - depth : 0;
	vc_commit_release(&c);
	stack_push(&st, head);
	while (st.n > 0)
	{
		PHash		h = st.v[--st.n];
		bool		created;
		uint64	   *v;

		CHECK_FOR_INTERRUPTS();
		/* a wider window can reach commits already cut; they stay cut */
		if (!phash_eq(&h, head) && !walk_held(r, held, &h))
			continue;
		v = phmap_ref(win, &h, &created);
		if (!created && *v <= bound + 1)
			continue;
		*v = bound + 1;
		c = must_load_commit(&r->ctx, &h);
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
at_high_mark(VcRepo *r, const PHash *head, const VcWindow *w, PHashMap *held,
			 bool *over)
{
	PHashMap	win,
				below;
	int			rc;

	phmap_init(&win);
	phmap_init(&below);
	rc = window_add(r, &win, head, w, held, &below);
	*over = false;
	for (size_t i = 0; rc == VC_OK && i < below.cap && !*over; i++)
		if (below.used[i] && !phmap_find(&win, &below.keys[i]) &&
			walk_held(r, held, &below.keys[i]))
			*over = true;
	phmap_free(&win);
	phmap_free(&below);
	return rc;
}

/* Adds head's window to win: its high mark's, or its low mark's once it reached the high one. */
static int
window_add_marked(VcRepo *r, PHashMap *win, const PHash *head, const VcWindow *w,
				  PHashMap *held, PHashMap *below)
{
	VcWindow	high = *w,
				low;
	bool		over;
	int			rc;

	high.low_commits = high.low_bytes = high.low_seconds = 0;
	if (!has_low_marks(w))
		return window_add(r, win, head, &high, held, below);
	if ((rc = at_high_mark(r, head, &high, held, &over)) != VC_OK)
		return rc;
	if (!over)
		return window_add(r, win, head, &high, held, below);
	low = low_window(w);
	return window_add(r, win, head, &low, held, below);
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

/* A segment's chunk, of its members and (exported) its archive and bounds. */
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
	if (seg->exported)
	{
		buf_append_uvarint(&b, strlen(seg->archive_name));
		buf_append(&b, seg->archive_name, strlen(seg->archive_name));
		buf_append(&b, seg->manifest.b, PHASH_LEN);
		buf_append_uvarint(&b, seg->nbounds);
		for (size_t i = 0; i < seg->nbounds; i++)
			buf_append(&b, seg->bounds[i].b, PHASH_LEN);
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

static bool
segment_decode(ChunkStore *cs, uint32 id, const PHash *h, VcSegment *out)
{
	uint32		len;
	uint8	   *d;
	const uint8 *p,
			   *end;
	char		hex[PHASH_HEX_LEN + 1];

	memset(out, 0, sizeof(*out));
	out->id = id;
	d = cs_get(cs, h->b, &len);
	if (d == NULL || len < 3 || d[0] != CHUNK_TAG_SEGMENT ||
		d[1] != CHUNK_FORMAT_VERSION || d[2] > 1)
		goto bad;
	out->exported = d[2] == 1;
	p = d + 3;
	end = d + len;
	if (!read_hashes(&p, end, &out->commits, &out->n))
		goto bad;
	if (out->exported)
	{
		uint64		nl;

		if (!read_uvarint(&p, end, &nl) || nl > (uint64) (end - p))
			goto bad;
		out->archive_name = pnstrdup((const char *) p, nl);
		p += nl;
		if (end - p < PHASH_LEN)
			goto bad;
		memcpy(out->manifest.b, p, PHASH_LEN);
		p += PHASH_LEN;
		if (!read_hashes(&p, end, &out->bounds, &out->nbounds))
			goto bad;
	}
	if (p != end)
		goto bad;
	pfree(d);
	return true;
bad:
	phash_hex(h, hex);
	ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("chunk %s, which segment %u names, is no segment", hex, id)));
}

bool
vc_segment_get(VcRepo *r, uint32 id, VcSegment *out)
{
	char	   *ref = segment_ref(id);
	PHash		h;
	bool		found = vc_refs_get(r->refs, ref, VC_REF_READ, &h);

	pfree(ref);
	if (!found)
		return false;
	return segment_decode(r->ctx.cs, id, &h, out);
}

void
vc_segment_free(VcSegment *seg)
{
	if (seg->commits)
		pfree(seg->commits);
	if (seg->archive_name)
		pfree(seg->archive_name);
	if (seg->bounds)
		pfree(seg->bounds);
	memset(seg, 0, sizeof(*seg));
}

/* (the window's calls read the branches after their lock, so not in a snapshot taken before it) */
static void
window_call_isolation(const char *call)
{
	if (IsolationUsesXactSnapshot())
		ereport(ERROR,
				(errcode(ERRCODE_ACTIVE_SQL_TRANSACTION),
				 errmsg("%s needs a READ COMMITTED transaction", call),
				 errdetail("It reads the branches after it locks the repository, which a snapshot taken before the lock would not show.")));
}

/* Branches locked for update by a rewriting sever, by name. */
typedef struct Locked
{
	char	  **names;
	size_t		n;
	size_t		cap;
} Locked;

static void
locked_name_fn(void *arg, const char *name, const PHash *hash)
{
	Locked	   *l = arg;

	px_grow((void **) &l->names, &l->cap, l->n + 1, sizeof(char *));
	l->names[l->n++] = pstrdup(name + strlen(BRANCH_REF_PREFIX));
}

/*
 * Every branch locked for update, in name order (a listing's), those in
 * done already skipped: no commit is then in flight on any, and none
 * starts until the transaction ends.
 */
static void
lock_branches(VcRepo *r, Locked *done)
{
	Locked		all = {0};

	vc_refs_list(r->refs, BRANCH_REF_PREFIX, locked_name_fn, &all);
	for (size_t i = 0; i < all.n; i++)
	{
		VcBranch	b;
		bool		had = false;

		for (size_t j = 0; j < done->n && !had; j++)
			had = strcmp(done->names[j], all.names[i]) == 0;
		if (had)
		{
			pfree(all.names[i]);
			continue;
		}
		/* (one deleted while waited for is not locked: not named so) */
		if (!branch_read(r, all.names[i], VC_REF_UPDATE, &b))
		{
			pfree(all.names[i]);
			continue;
		}
		vc_branch_free(&b);
		px_grow((void **) &done->names, &done->cap, done->n + 1, sizeof(char *));
		done->names[done->n++] = all.names[i];
	}
	if (all.names)
		px_free(all.names);
}

/* Whether every branch there is was locked by lock_branches into l. */
static bool
all_locked(VcRepo *r, Locked *l)
{
	Locked		all = {0};
	bool		ok = true;

	vc_refs_list(r->refs, BRANCH_REF_PREFIX, locked_name_fn, &all);
	for (size_t i = 0; i < all.n; i++)
	{
		bool		had = false;

		for (size_t j = 0; j < l->n && !had; j++)
			had = strcmp(l->names[j], all.names[i]) == 0;
		ok = ok && had;
		pfree(all.names[i]);
	}
	if (all.names)
		px_free(all.names);
	return ok;
}

static void
locked_free(Locked *l)
{
	for (size_t i = 0; i < l->n; i++)
		pfree(l->names[i]);
	if (l->names)
		px_free(l->names);
}

/* A kept commit of a rewriting sever, and what it became. */
typedef struct Kept
{
	VcCommit	c;
	bool		moved;			/* written again */
	PHash		now;
	uint64		gen,
				bytes;
} Kept;

static int
kept_cmp(const void *a, const void *b)
{
	const Kept *x = a,
			   *y = b;

	return (x->c.generation > y->c.generation) - (x->c.generation < y->c.generation);
}

typedef struct MovedArg
{
	VcRepo	   *r;
	PHashMap   *idx;
	Kept	   *k;
} MovedArg;

static const PHash *
moved_to(MovedArg *a, const PHash *h)
{
	uint64	   *at = phmap_find(a->idx, h);

	return at && a->k[*at].moved ? &a->k[*at].now : NULL;
}

static void
moved_branch_fn(void *arg, const VcBranch *b)
{
	MovedArg   *a = arg;
	const PHash *head = moved_to(a, &b->head);
	const PHash *mh = b->merging ? moved_to(a, &b->merge_head) : NULL;
	VcBranch	nb;

	if (head == NULL && mh == NULL)
		return;
	nb = *b;
	if (head)
		nb.head = *head;		/* (the same root: head_root stays) */
	if (mh)
		nb.merge_head = *mh;
	branch_write(a->r, &nb);
}

/*
 * REWRITE mode: the cut commits are let go at once, and every commit the
 * window keeps with cut history beneath it is written again, oldest first
 * (one outside the window not cut yet, as max_cut leaves some, is left as
 * it is, for a sever after to cut, not written again meanwhile): cut parents
 * dropped, rewritten ones replaced.  Its root value, time and message stay;
 * its generation and change bytes are recounted from its new parents, its
 * own change bytes kept, so measures from a head stay as they were.  A kept
 * commit with nothing cut beneath it keeps its hash.  Heads and merge heads
 * move to the new hashes (every branch locked); the hook hears each one.
 */
static void
rewrite_history(VcRepo *r, const PHash *cut, size_t ncut, PHashMap *win)
{
	PHashMap	cutset,
				idx;
	HashStack	held = {0};
	Kept	   *k;
	size_t		nk = 0;
	bool		created;
	MovedArg	ma;

	phmap_init(&cutset);
	phmap_init(&idx);
	for (size_t i = 0; i < ncut; i++)
		phmap_ref(&cutset, &cut[i], &created);
	vc_refs_list(r->refs, COMMIT_REF_PREFIX, held_fn, &held);
	k = palloc0_array(Kept, Max(held.n, 1));
	for (size_t i = 0; i < held.n; i++)
	{
		CHECK_FOR_INTERRUPTS();
		if (phmap_find(win, &held.v[i]))
			k[nk++].c = must_load_commit(&r->ctx, &held.v[i]);
	}
	if (held.v)
		px_free(held.v);
	qsort(k, nk, sizeof(Kept), kept_cmp);
	for (size_t i = 0; i < nk; i++)
		*phmap_ref(&idx, &k[i].c.hash, &created) = i;

	for (size_t i = 0; i < nk; i++)
	{
		VcCommit   *c = &k[i].c;
		PHash		par[VC_MAX_PARENTS];
		uint64		pg[VC_MAX_PARENTS],
					pb[VC_MAX_PARENTS],
					pt[VC_MAX_PARENTS],
					own,
					gen = 0,
					bytes = 0,
					oldmax = 0;
		uint32		np = 0;
		bool		changed = false;

		CHECK_FOR_INTERRUPTS();
		for (uint32 j = 0; j < c->nparents; j++)
		{
			uint64	   *at = phmap_find(&idx, &c->parents[j]);

			if (c->parent_bytes[j] > oldmax)
				oldmax = c->parent_bytes[j];
			if (phmap_find(&cutset, &c->parents[j]))
			{
				changed = true;
				continue;
			}
			par[np] = c->parents[j];
			pg[np] = c->parent_gen[j];
			pb[np] = c->parent_bytes[j];
			pt[np] = c->parent_time[j];
			if (at && k[*at].moved)
			{
				par[np] = k[*at].now;
				pg[np] = k[*at].gen;
				pb[np] = k[*at].bytes;
				changed = true;
			}
			np++;
		}
		if (!changed)
			continue;
		own = c->bytes - oldmax;
		for (uint32 j = 0; j < np; j++)
		{
			if (pg[j] > gen)
				gen = pg[j];
			if (pb[j] > bytes)
				bytes = pb[j];
		}
		k[i].gen = gen + 1;
		k[i].bytes = bytes + own;
		k[i].now = commit_put(&r->ctx, &c->root, k[i].gen, k[i].bytes, c->time, np, par,
							  pg, pb, pt, c->msg, c->msglen);
		k[i].moved = true;
	}

	/* the new commits held before the old ones go: they share their trees */
	for (size_t i = 0; i < nk; i++)
		if (k[i].moved)
		{
			char	   *ref = commit_ref(&k[i].now);

			(void) vc_refs_add(r->refs, ref, &k[i].now);
			pfree(ref);
		}
	ma.r = r;
	ma.idx = &idx;
	ma.k = k;
	vc_branch_list(r, moved_branch_fn, &ma);
	for (size_t i = 0; i < nk; i++)
	{
		if (k[i].moved)
		{
			char	   *ref = commit_ref(&k[i].c.hash);

			(void) vc_refs_remove(r->refs, ref);
			pfree(ref);
			if (r->rewrite_fn)
				r->rewrite_fn(r->rewrite_arg, &k[i].c.hash, &k[i].now);
		}
		vc_commit_release(&k[i].c);
	}
	/* and what was cut goes for good */
	for (size_t i = 0; i < ncut; i++)
	{
		char	   *ref = commit_ref(&cut[i]);

		(void) vc_refs_remove(r->refs, ref);
		pfree(ref);
	}
	pfree(k);
	phmap_free(&cutset);
	phmap_free(&idx);
}

int
vc_sever(VcRepo *r, const VcWindow *w, uint32 *seg_out)
{
	RepoState	st;
	PHashMap	win,
				heldset;
	HeadsArg	ha = {{0}};
	HashStack	held = {0};
	bool		created;
	VcSegment	seg = {0};
	LocEdits	le = {0};
	size_t		cap = 0;
	int			rc = VC_OK;
	uint64		max_cut = w->max_cut && w->max_cut < VC_SEVER_MAX ? w->max_cut : VC_SEVER_MAX;
	Locked		locked = {0};

	*seg_out = VC_NO_SEGMENT;
	if (!w->depth && !w->keep_bytes && !w->keep_seconds && !w->max_commits &&
		!w->max_bytes && !w->max_seconds)
		return VC_ERR;			/* no window at all, or low marks alone */
	window_call_isolation("a sever");

	/*
	 * A rewriting sever moves heads: every branch locked first (as every
	 * call locks branches before the repository's state), so no commit is
	 * in flight on any.  A branch made, or the mode set, between that and
	 * the state's lock would be locked after the state, against that
	 * order: the sever is refused instead, to be run again.
	 */
	{
		RepoState	pre;

		repo_read(r, VC_REF_READ, &pre);
		if (pre.history == VC_HISTORY_REWRITE)
			lock_branches(r, &locked);
		repo_state_free(&pre);
	}

	/*
	 * (locked before the heads are read: a call moving a branch to a
	 * commit it did not write holds this for share, so none is in flight)
	 */
	repo_read(r, VC_REF_UPDATE, &st);
	if (st.history == VC_HISTORY_REWRITE && !all_locked(r, &locked))
		ereport(ERROR,
				(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				 errmsg("could not sever: the branches or the history mode changed while it waited"),
				 errhint("Sever again.")));
	else if (st.history == VC_HISTORY_KEEP && st.next_seg >= VC_NO_SEGMENT)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("the repository has made its last segment")));

	/*
	 * The commits with references, listed before the heads are read: a
	 * commit's reference and its branch's move become visible together,
	 * as its transaction commits, so one committing after the listing is
	 * not in it (nothing of it cut), and one committing before is in it
	 * and its branch's move seen by the heads' read, which come after:
	 * the head, or (if the branch has moved on since) behind it, where
	 * the window decides as for any.  (Read the other way round, a commit
	 * committing between would be listed but its head unseen: cut.)
	 */
	vc_refs_list(r->refs, COMMIT_REF_PREFIX, held_fn, &held);
	INJECTION_POINT("vc-sever-listed", NULL);
	phmap_init(&heldset);
	for (size_t i = 0; i < held.n; i++)
		phmap_ref(&heldset, &held.v[i], &created);
	phmap_init(&win);
	vc_branch_list(r, heads_fn, &ha);
	for (size_t i = 0; i < ha.heads.n && rc == VC_OK; i++)
		rc = window_add_marked(r, &win, &ha.heads.v[i], w, &heldset, NULL);
	for (size_t i = 0; i < w->npinned && rc == VC_OK; i++)
	{
		/* (a commit cut already is no commit to keep) */
		if (!walk_held(r, &heldset, &w->pinned[i]))
		{
			r->ctx.err_hash = w->pinned[i];
			r->ctx.err_has_hash = true;
			r->ctx.err = rc = VC_ERR_SEVERED;
			break;
		}
		rc = window_add(r, &win, &w->pinned[i], &(VcWindow) {.depth = 1}, &heldset, NULL);
	}
	px_free(ha.heads.v);
	phmap_free(&heldset);
	if (rc != VC_OK)
	{
		px_free(held.v);
		phmap_free(&win);
		locked_free(&locked);
		repo_state_free(&st);
		return rc;
	}

	/*
	 * everything the window's history holds that the window no longer
	 * covers, as many as one sever cuts
	 */
	for (size_t i = 0; i < held.n && seg.n < max_cut; i++)
	{
		if (phmap_find(&win, &held.v[i]))
			continue;
		px_grow((void **) &seg.commits, &cap, seg.n + 1, sizeof(PHash));
		seg.commits[seg.n++] = held.v[i];
	}
	px_free(held.v);
	if (seg.n > 0 && st.history == VC_HISTORY_REWRITE)
	{
		/* (no segment: the history in no state, nothing records it) */
		rewrite_history(r, seg.commits, seg.n, &win);
		px_free(seg.commits);
		phmap_free(&win);
		locked_free(&locked);
		repo_state_free(&st);
		return VC_OK;
	}
	phmap_free(&win);
	if (seg.n == 0)
	{
		locked_free(&locked);
		repo_state_free(&st);
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
		loc_set(&le, &seg.commits[i], LOC_SEG(seg.id, false));
	}
	st.loc_root = loc_apply(r, &st.loc_root, &le);
	st.next_seg++;
	repo_write(r, &st);
	*seg_out = seg.id;
	px_free(seg.commits);
	repo_state_free(&st);
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

		CHECK_FOR_INTERRUPTS();
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
 * An archive a segment may go to, or come back from: another store, as
 * transactional as the repository's (its roots change with the segment's
 * row), and for an exported one, the one it went to
 */
static bool
archive_ok(VcRepo *r, const VcSource *archive, const VcSegment *seg)
{
	if (archive == NULL || archive->cs == r->ctx.cs ||
		archive->cs->routine->transactional != r->ctx.cs->routine->transactional)
		return false;
	return seg == NULL || !seg->exported || strcmp(seg->archive_name, archive->name) == 0;
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
	VcSegment	seg = {0};
	LocEdits	le = {0};
	Buf			b = BUF_INIT;
	PHash		manifest;

	window_call_isolation("an export");
	if (!archive_ok(r, archive, NULL))
		return VC_ERR;
	repo_read(r, VC_REF_UPDATE, &st);
	if (!vc_segment_get(r, id, &seg) || seg.exported)
	{
		vc_segment_free(&seg);
		repo_state_free(&st);
		return VC_ERR;
	}
	(void) cs_copy_closure(r->ctx.cs, archive->cs, seg.commits[0].b, (int) seg.n,
						   "export", "the repository's store");
	segment_bounds(&r->ctx, &seg, &seg.bounds, &seg.nbounds);
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
		loc_set(&le, &seg.commits[i], LOC_SEG(id, true));
	}
	buf_append_uvarint(&b, seg.nbounds);
	for (size_t i = 0; i < seg.nbounds; i++)
		buf_append(&b, seg.bounds[i].b, PHASH_LEN);
	buf_append_uvarint(&b, 0);	/* (no keyframes) */
	(void) cs_put(archive->cs, b.data, (uint32) b.len, manifest.b);
	buf_free(&b);
	cs_ref_root(archive->cs, manifest.b);

	seg.exported = true;
	seg.archive_name = pstrdup(archive->name);
	seg.manifest = manifest;
	segment_write(r, &seg);
	st.loc_root = loc_apply(r, &st.loc_root, &le);
	repo_write(r, &st);
	vc_segment_free(&seg);
	repo_state_free(&st);
	return VC_OK;
}

/*
 * Discards a segment.  A local one is let go of here; an exported one is
 * let go of in its archive, which frees exactly what no other manifest
 * there holds.  Its members are forgotten: those that what is held still
 * names are found as discarded when asked (vc_locate).
 */
/*
 * The highest generation among a segment's members: from their commits
 * (a local segment's), or from its manifest (an exported one's, whose
 * commits are in the archive alone).
 */
static uint64
segment_top_gen(VcRepo *r, const VcSegment *seg, const VcSource *archive)
{
	uint64		top = 0;

	if (seg->exported)
	{
		VcManifest	m;

		if (!vc_manifest_load(archive->cs, &seg->manifest, &m))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("segment %u's manifest is not in archive \"%s\"",
							seg->id, archive->name)));
		for (size_t i = 0; i < m.n; i++)
			top = Max(top, m.gens[i]);
		vc_manifest_free(&m);
		return top;
	}
	for (size_t i = 0; i < seg->n; i++)
	{
		VcCommit	c = must_load_commit(&r->ctx, &seg->commits[i]);

		top = Max(top, c.generation);
		vc_commit_release(&c);
	}
	return top;
}

int
vc_segment_discard(VcRepo *r, uint32 id, const VcSource *archive)
{
	RepoState	st;
	VcSegment	seg = {0};
	LocEdits	le = {0};
	char	   *ref;

	window_call_isolation("a discard");
	repo_read(r, VC_REF_UPDATE, &st);
	if (!vc_segment_get(r, id, &seg) || (seg.exported && !archive_ok(r, archive, &seg)))
	{
		vc_segment_free(&seg);
		repo_state_free(&st);
		return VC_ERR;
	}
	for (size_t i = 0; i < seg.n; i++)
		if (LOC_IS_SEG(loc_get(r, &st.loc_root, &seg.commits[i])))
			loc_set(&le, &seg.commits[i], LOC_GONE);
	st.loc_root = loc_apply(r, &st.loc_root, &le);
	st.forgot_gen = Max(st.forgot_gen, segment_top_gen(r, &seg, archive));
	repo_write(r, &st);
	if (seg.exported)
		cs_unref_root(archive->cs, seg.manifest.b);
	ref = segment_ref(id);
	(void) vc_refs_remove(r->refs, ref);
	pfree(ref);
	vc_segment_free(&seg);
	repo_state_free(&st);
	return VC_OK;
}

int
vc_segment_attach(VcRepo *r, uint32 id, const VcSource *archive)
{
	RepoState	st;
	VcSegment	seg = {0};
	LocEdits	le = {0};
	char	   *ref;

	window_call_isolation("an attach");
	repo_read(r, VC_REF_UPDATE, &st);
	if (!vc_segment_get(r, id, &seg) || (seg.exported && !archive_ok(r, archive, &seg)))
	{
		vc_segment_free(&seg);
		repo_state_free(&st);
		return VC_ERR;
	}
	/* an exported one's chunks copied back first, each checked */
	if (seg.exported)
		(void) cs_copy_closure(archive->cs, r->ctx.cs, seg.commits[0].b, (int) seg.n,
							   "attach", "the archive");
	/* the holds move back to the window's history */
	for (size_t i = 0; i < seg.n; i++)
	{
		ref = commit_ref(&seg.commits[i]);
		vc_refs_set(r->refs, ref, &seg.commits[i]);
		pfree(ref);
		if (LOC_IS_SEG(loc_get(r, &st.loc_root, &seg.commits[i])))
			loc_set(&le, &seg.commits[i], LOC_GONE);
	}
	st.loc_root = loc_apply(r, &st.loc_root, &le);
	repo_write(r, &st);
	if (seg.exported)
		cs_unref_root(archive->cs, seg.manifest.b);
	ref = segment_ref(id);
	(void) vc_refs_remove(r->refs, ref);
	pfree(ref);
	vc_segment_free(&seg);
	repo_state_free(&st);
	return VC_OK;
}

/*
 * The parents of what the repository holds, that it does not otherwise
 * know: commits discarded here, at the edge of what is kept.  The window's
 * commits and the local segments' members are read for theirs; an
 * exported segment's are its bounds.  O(the window and local segments).
 */
typedef struct CutArg
{
	VcRepo	   *r;
	PHashMap   *parents;
} CutArg;

static void
cut_parents_of(CutArg *a, const PHash *h)
{
	VcCommit	c;
	bool		created;

	CHECK_FOR_INTERRUPTS();
	if (!vc_commit_load(&a->r->ctx, h, &c))
		return;
	for (uint32 j = 0; j < c.nparents; j++)
		phmap_ref(a->parents, &c.parents[j], &created);
	vc_commit_release(&c);
}

static void
cut_window_fn(void *arg, const char *name, const PHash *hash)
{
	cut_parents_of(arg, hash);
}

static void
cut_segment_fn(void *arg, const char *name, const PHash *hash)
{
	CutArg	   *a = arg;
	VcSegment	seg;
	bool		created;

	(void) segment_decode(a->r->ctx.cs, (uint32) strtoul(name + strlen(SEGMENT_REF_PREFIX), NULL, 10),
						  hash, &seg);
	if (seg.exported)
		for (size_t i = 0; i < seg.nbounds; i++)
			phmap_ref(a->parents, &seg.bounds[i], &created);
	else
		for (size_t i = 0; i < seg.n; i++)
			cut_parents_of(a, &seg.commits[i]);
	vc_segment_free(&seg);
}

static void
cut_parents(VcRepo *r, PHashMap *parents)
{
	CutArg		a = {r, parents};

	phmap_init(parents);
	vc_refs_list(r->refs, COMMIT_REF_PREFIX, cut_window_fn, &a);
	vc_refs_list(r->refs, SEGMENT_REF_PREFIX, cut_segment_fn, &a);
}

/* (a commit's location, given the map's value for it and the cut parents) */
static VcLocation
locate_in(VcRepo *r, const PHash *h, uint64 v, PHashMap *cut, uint32 *seg)
{
	if (LOC_IS_BOUND(v))
		return VC_LOC_BOUNDARY;
	if (LOC_IS_SEG(v))
	{
		if (seg)
			*seg = LOC_SEG_ID(v);
		return LOC_SEG_EXPORTED(v) ? VC_LOC_EXPORTED : VC_LOC_SEGMENT;
	}
	if (vc_in_window(r, h))
		return VC_LOC_WINDOW;
	if (cut && phmap_find(cut, h))
		return VC_LOC_DISCARDED;
	return VC_LOC_UNKNOWN;
}

VcLocation
vc_locate(VcRepo *r, const PHash *h, uint32 *seg)
{
	RepoState	st;
	uint64		v;
	VcLocation	l;
	PHashMap	cut;

	if (vc_in_window(r, h))
		return VC_LOC_WINDOW;
	repo_read(r, VC_REF_READ, &st);
	v = loc_get(r, &st.loc_root, h);
	repo_state_free(&st);
	if (v != LOC_GONE)
		return locate_in(r, h, v, NULL, seg);
	/* (not held: perhaps a parent of what is) */
	cut_parents(r, &cut);
	l = locate_in(r, h, v, &cut, seg);
	phmap_free(&cut);
	return l;
}

typedef struct LocsArg
{
	VcRepo	   *r;
	VcLocFn		fn;
	void	   *arg;
	PHashMap   *known;
} LocsArg;

static void
window_loc_fn(void *arg, const char *name, const PHash *hash)
{
	LocsArg    *a = arg;
	bool		created;

	phmap_ref(a->known, hash, &created);
	a->fn(a->arg, hash, VC_LOC_WINDOW, VC_NO_SEGMENT);
}

void
vc_locations(VcRepo *r, VcLocFn fn, void *arg)
{
	PHashMap	known,
				cut;
	LocsArg		a = {r, fn, arg, &known};
	RepoState	st;
	ProllyIter *it;
	const uint8 *k,
			   *v;
	size_t		kl,
				vl;
	int			rc;

	phmap_init(&known);
	vc_refs_list(r->refs, COMMIT_REF_PREFIX, window_loc_fn, &a);
	repo_read(r, VC_REF_READ, &st);
	vc_must(&r->ctx, prolly_iter_open(&r->ctx, &st.loc_root, NULL, 0, &it));
	while ((rc = prolly_iter_next(it, &k, &kl, &v, &vl)) == 1)
	{
		PHash		h;
		uint64		where;
		const uint8 *p = v;
		uint32		seg = VC_NO_SEGMENT;
		VcLocation	l;
		bool		created;

		if (kl != PHASH_LEN || !read_uvarint(&p, v + vl, &where))
			continue;
		memcpy(h.b, k, PHASH_LEN);
		phmap_ref(&known, &h, &created);
		l = locate_in(r, &h, where, NULL, &seg);
		fn(arg, &h, l, l == VC_LOC_SEGMENT || l == VC_LOC_EXPORTED ? seg : VC_NO_SEGMENT);
	}
	prolly_iter_close(it);
	if (rc < 0)
		prolly_report(&r->ctx);
	repo_state_free(&st);
	/* and the parents discarded here that what is held names */
	cut_parents(r, &cut);
	for (size_t i = 0; i < cut.cap; i++)
		if (cut.used[i] && !phmap_find(&known, &cut.keys[i]))
			fn(arg, &cut.keys[i], VC_LOC_DISCARDED, VC_NO_SEGMENT);
	phmap_free(&cut);
	phmap_free(&known);
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

/* --------------------------------------------------------------- remotes */

/*
 * The repository's hint for a boundary, its number in st (0 if none).  The
 * list holds each name once, and a name is a remote's or an archive's, so
 * it is as long as the places history came from, not as the boundaries.
 */
static uint32
hint_number(RepoState *st, const char *where)
{
	if (where == NULL || where[0] == '\0')
		return 0;
	for (uint32 i = 0; i < st->nhints; i++)
		if (strcmp(st->hints[i], where) == 0)
			return i + 1;
	st->hints = st->nhints == 0 ? palloc_array(char *, 1) :
		repalloc_array(st->hints, char *, st->nhints + 1);
	st->hints[st->nhints++] = pstrdup(where);
	return st->nhints;
}

/* (vc_hint, its state read already) */
static char *
hint_in(VcRepo *r, const RepoState *st, const PHash *commit)
{
	uint64		v = loc_get(r, &st->loc_root, commit);
	char	   *out = NULL;

	if (LOC_IS_SEG(v) && LOC_SEG_EXPORTED(v))
	{
		VcSegment	seg;

		if (vc_segment_get(r, LOC_SEG_ID(v), &seg))
		{
			out = pstrdup(seg.archive_name);
			vc_segment_free(&seg);
		}
	}
	else if (LOC_IS_BOUND(v) && LOC_BOUND_HINT(v) > 0 &&
			 LOC_BOUND_HINT(v) <= st->nhints)
		out = pstrdup(st->hints[LOC_BOUND_HINT(v) - 1]);
	return out;
}

char *
vc_hint(VcRepo *r, const PHash *commit)
{
	RepoState	st;
	char	   *out;

	repo_read(r, VC_REF_READ, &st);
	out = hint_in(r, &st, commit);
	repo_state_free(&st);
	return out;
}

/*
 * What a transfer's walk asks of each side, read once: the receiver's
 * state and the parents of what it holds that it discarded.
 */
typedef struct Side
{
	VcRepo	   *r;
	RepoState	st;
	bool		has_cut;		/* cut found (on the first commit it may know) */
	PHashMap	cut;
} Side;

static void
side_open(Side *s, VcRepo *r, VcRefLock lock)
{
	s->r = r;
	repo_read(r, lock, &s->st);
	s->has_cut = false;
}

static void
side_close(Side *s)
{
	repo_state_free(&s->st);
	if (s->has_cut)
		phmap_free(&s->cut);
}

/*
 * Known to the side: anywhere in its history, a boundary, or a parent of
 * what it holds that it discarded (the same hash is the same history).
 * Commits it discarded that nothing it holds names are forgotten, so not
 * known: a transfer sends them again if a commit sent builds on them.
 *
 * gen is the commit's generation (PG_UINT64_MAX if not known): one above
 * every generation a discard forgot was never discarded, so the parents
 * of what is held, found by reading every commit held, are found only for
 * a commit that may be one.
 */
static bool
side_knows(Side *s, const PHash *h, uint64 gen)
{
	if (vc_in_window(s->r, h) || loc_get(s->r, &s->st.loc_root, h) != LOC_GONE)
		return true;
	if (s->st.forgot_gen == 0 || (gen != PG_UINT64_MAX && gen > s->st.forgot_gen))
		return false;			/* (none forgotten, or too new to be) */
	if (!s->has_cut)
	{
		cut_parents(s->r, &s->cut);
		s->has_cut = true;
	}
	return phmap_find(&s->cut, h) != NULL;
}

/* Whether the side rewrites its history: no remotes then (its hashes do not last). */
static bool
rewrites(Side *s)
{
	return s->st.history == VC_HISTORY_REWRITE;
}

/* Content the side holds for a commit: in its window or a local segment. */
static bool
side_has(Side *s, const PHash *h)
{
	uint64		v;

	if (vc_in_window(s->r, h))
		return true;
	v = loc_get(s->r, &s->st.loc_root, h);
	return LOC_IS_SEG(v) && !LOC_SEG_EXPORTED(v);
}

static int
gen_cmp(const void *pa, const void *pb)
{
	const GenEnt *a = pa;
	const GenEnt *b = pb;

	return (a->gen > b->gen) - (a->gen < b->gen);
}

/* A planned transfer: commits to send, oldest first, and new boundaries. */
typedef struct Plan
{
	GenEnt	   *send;
	size_t		nsend;
	size_t		sendcap;
	PHash	   *bound;
	char	  **bhint;
	size_t		nbound;
	size_t		boundcap;
	bool		connected;		/* reached a commit the receiver knows */
	bool		cut;			/* reached a commit the sender has cut */
	PHash		first_cut;
} Plan;

static void
plan_free(Plan *p)
{
	if (p->send)
		px_free(p->send);
	if (p->bound)
		px_free(p->bound);
	if (p->bhint)
		px_free(p->bhint);
	memset(p, 0, sizeof(*p));
}

static void
plan_bound(Plan *p, const PHash *h, char *hint)
{
	px_grow((void **) &p->bound, &p->boundcap, p->nbound + 1, sizeof(PHash));
	p->bhint = px_realloc(p->bhint, p->boundcap * sizeof(char *));
	p->bound[p->nbound] = *h;
	p->bhint[p->nbound] = hint;
	p->nbound++;
}

/*
 * Plans what src must send so dst has heads.  It walks parents from each
 * head, stopping at any commit dst already knows, whatever its location
 * there: exported history, and discarded history it still names, is
 * never sent back implicitly.
 * With depth > 0, a path stops depth generations below its head; what lies
 * past becomes a boundary, hinting at src.  A parent src itself holds no
 * content for is a boundary too, hinting where src says it lives.
 */
static void
plan_walk(Side *src, Side *dst, const PHash *heads, size_t nheads,
		  uint32 depth, Plan *p)
{
	PHashMap	seen,
				planned;
	PHash	   *stack = NULL;
	uint64	   *sbound = NULL,
			   *sgen = NULL;
	size_t		n = 0,
				cap = 0;
	bool		created;

	phmap_init(&seen);
	phmap_init(&planned);
	for (size_t i = 0; i < nheads; i++)
	{
		VcCommit	c;
		uint64		b = 0,
					g = PG_UINT64_MAX;

		if (side_has(src, &heads[i]) && vc_commit_load(&src->r->ctx, &heads[i], &c))
		{
			g = c.generation;
			if (depth > 0)
				b = c.generation > depth ? c.generation - depth : 0;
			vc_commit_release(&c);
		}
		px_grow((void **) &stack, &cap, n + 1, sizeof(PHash));
		sbound = px_realloc(sbound, cap * sizeof(uint64));
		sgen = px_realloc(sgen, cap * sizeof(uint64));
		stack[n] = heads[i];
		sgen[n] = g;
		sbound[n++] = b;
	}
	while (n > 0)
	{
		PHash		h = stack[--n];
		uint64		b = sbound[n];
		uint64		g = sgen[n];
		uint64	   *v;
		VcCommit	c;

		CHECK_FOR_INTERRUPTS();
		if (side_knows(dst, &h, g))
		{
			p->connected = true;
			continue;
		}
		v = phmap_ref(&seen, &h, &created);
		if (!created && *v <= b + 1)
			continue;
		*v = b + 1;
		if (!side_has(src, &h) || !vc_commit_load(&src->r->ctx, &h, &c))
		{
			if (!p->cut)
				p->first_cut = h;
			p->cut = true;
			plan_bound(p, &h, hint_in(src->r, &src->st, &h));
			continue;
		}
		phmap_ref(&planned, &h, &created);
		if (created)
		{
			px_grow((void **) &p->send, &p->sendcap, p->nsend + 1, sizeof(GenEnt));
			p->send[p->nsend].gen = c.generation;
			p->send[p->nsend].h = h;
			p->nsend++;
		}
		for (uint32 i = 0; i < c.nparents; i++)
		{
			if (c.parent_gen[i] > b)
			{
				px_grow((void **) &stack, &cap, n + 1, sizeof(PHash));
				sbound = px_realloc(sbound, cap * sizeof(uint64));
				sgen = px_realloc(sgen, cap * sizeof(uint64));
				stack[n] = c.parents[i];
				sgen[n] = c.parent_gen[i];
				sbound[n++] = b;
			}
			else if (!side_knows(dst, &c.parents[i], c.parent_gen[i]))
				plan_bound(p, &c.parents[i], pstrdup(src->st.name));
		}
		vc_commit_release(&c);
	}
	/* a boundary that some other path sends is no boundary */
	{
		size_t		k = 0;

		for (size_t i = 0; i < p->nbound; i++)
			if (!phmap_find(&planned, &p->bound[i]))
			{
				p->bound[k] = p->bound[i];
				p->bhint[k++] = p->bhint[i];
			}
		p->nbound = k;
	}
	if (p->nsend > 1)
		qsort(p->send, p->nsend, sizeof(GenEnt), gen_cmp);
	if (stack)
		px_free(stack);
	if (sbound)
		px_free(sbound);
	if (sgen)
		px_free(sgen);
	phmap_free(&seen);
	phmap_free(&planned);
}

/*
 * Histories must attach: walking back from the heads has to reach a commit
 * dst already knows, wherever it is there (a cut commit with the same hash
 * is the same history).  Everything down to it is sent.  If the walk ends
 * at roots without that, the histories are unrelated; if it ends at the
 * sender's own cut, they cannot be shown to attach until that history is
 * restored (its first cut commit in both repositories' err_hash).  Only a
 * clone (allow_unrelated) skips this, and stops at depth.
 */
static int
plan_transfer(Side *src, Side *dst, const PHash *heads, size_t nheads,
			  uint32 depth, bool allow_unrelated, Plan *p)
{
	memset(p, 0, sizeof(*p));
	if (allow_unrelated)
	{
		/* (a clone, into a store with nothing: nothing to attach to) */
		plan_walk(src, dst, heads, nheads, depth, p);
		return VC_OK;
	}
	plan_walk(src, dst, heads, nheads, 0, p);
	if (p->connected || (p->nsend == 0 && !p->cut))
		return VC_OK;
	if (p->cut)
	{
		src->r->ctx.err_hash = dst->r->ctx.err_hash = p->first_cut;
		plan_free(p);
		return VC_ERR_DETACHED;
	}
	plan_free(p);
	return VC_ERR_UNRELATED;
}

/*
 * Copies the planned commits' chunks into dst's store, held there until
 * the transaction ends (each checked against its name).
 */
static void
plan_copy(Side *src, Side *dst, const Plan *p)
{
	uint8	   *roots;

	if (p->nsend == 0)
		return;
	if (p->nsend > PG_INT32_MAX)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("a transfer sends at most %d commits", PG_INT32_MAX)));
	roots = palloc(p->nsend * PHASH_LEN);
	for (size_t i = 0; i < p->nsend; i++)
		memcpy(roots + i * PHASH_LEN, p->send[i].h.b, PHASH_LEN);
	(void) cs_copy_closure(src->r->ctx.cs, dst->r->ctx.cs, roots, (int) p->nsend,
						   "transfer", src->st.name[0] ? src->st.name : "the sender");
	pfree(roots);
}

/*
 * Takes the copied commits into dst's window, each by a reference of its
 * own, and records the boundaries in its state (written back).
 */
static void
plan_adopt(Side *dst, const Plan *p)
{
	LocEdits	le = {0};

	for (size_t i = 0; i < p->nsend; i++)
	{
		char	   *ref = commit_ref(&p->send[i].h);

		(void) vc_refs_add(dst->r->refs, ref, &p->send[i].h);
		pfree(ref);
	}
	for (size_t i = 0; i < p->nbound; i++)
	{
		uint64		v = loc_get(dst->r, &dst->st.loc_root, &p->bound[i]);

		/* (a boundary known already stopped the walk: never one here) */
		if (v == LOC_GONE)
			loc_set(&le, &p->bound[i], LOC_BOUND(hint_number(&dst->st, p->bhint[i])));
	}
	if (le.n > 0)
	{
		dst->st.loc_root = loc_apply(dst->r, &dst->st.loc_root, &le);
		repo_write(dst->r, &dst->st);
	}
}

/* Sends src's commits down from heads to dst, which takes them in. */
static int
transfer(VcRepo *src, VcRepo *dst, const PHash *heads, size_t nheads,
		 uint32 depth, bool allow_unrelated)
{
	Side		s,
				d;
	Plan		p;
	int			rc;

	window_call_isolation("a transfer");
	/* (the receiver's state locked first: its boundaries may change) */
	side_open(&d, dst, VC_REF_UPDATE);
	side_open(&s, src, VC_REF_READ);
	if (rewrites(&s) || rewrites(&d))
	{
		side_close(&s);
		side_close(&d);
		return VC_ERR_REWRITTEN;
	}
	rc = plan_transfer(&s, &d, heads, nheads, depth, allow_unrelated, &p);
	if (rc == VC_OK)
	{
		plan_copy(&s, &d, &p);
		plan_adopt(&d, &p);
		plan_free(&p);
	}
	side_close(&s);
	side_close(&d);
	return rc;
}

/*
 * Whether a push may move the remote's branch rb to lb's head: 1 if so,
 * VC_OK if it is there already, VC_ERR if work there not committed, or a
 * merge under way, would be overwritten.
 */
static int
push_target(const VcBranch *rb, const VcBranch *lb)
{
	if (phash_eq(&rb->head, &lb->head))
		return VC_OK;
	if (rb->merging || !phash_eq(&rb->working, &rb->head_root))
		return VC_ERR;
	return 1;
}

/*
 * The remote checks fast-forward itself, against its own history plus the
 * commits just received, so it never needs history the sender has cut.
 * A rejected push leaves nothing behind (what was copied, unheld, freed).
 */
int
vc_push(VcRepo *local, VcRepo *remote, const char *branch)
{
	VcBranch	lb,
				rb;
	bool		had;
	Side		s,
				d;
	Plan		p;
	PHash		base;
	int			rc;

	window_call_isolation("a push");
	/* (refused first, whatever the branches: neither's hashes may travel) */
	if (vc_history_mode(local) == VC_HISTORY_REWRITE ||
		vc_history_mode(remote) == VC_HISTORY_REWRITE)
		return VC_ERR_REWRITTEN;
	if (!branch_read(local, branch, VC_REF_READ, &lb))
		return VC_ERR;

	/* (the receiver's branch first, then its state: every call's order) */
	had = branch_read(remote, branch, VC_REF_UPDATE, &rb);
	if (had && (rc = push_target(&rb, &lb)) != 1)
	{
		vc_branch_free(&rb);
		vc_branch_free(&lb);
		return rc;
	}
	side_open(&d, remote, VC_REF_UPDATE);

	/*
	 * A branch not there may have been made since (made, or pushed to, by
	 * another that committed first): read again, not locked (that would
	 * take a branch after the repository's state), and refused if there
	 * now: pushed again, it is locked first and checked as any.
	 */
	if (!had && branch_read(remote, branch, VC_REF_READ, &rb))
	{
		side_close(&d);
		vc_branch_free(&rb);
		vc_branch_free(&lb);
		return VC_ERR;
	}
	side_open(&s, local, VC_REF_READ);
	rc = rewrites(&s) || rewrites(&d) ? VC_ERR_REWRITTEN :
		plan_transfer(&s, &d, &lb.head, 1, 0, false, &p);
	if (rc == VC_OK)
	{
		plan_copy(&s, &d, &p);
		if (had)
		{
			rc = vc_merge_base(&remote->ctx, &lb.head, &rb.head, &base);
			if (rc == 1 && phash_eq(&base, &rb.head))
				rc = VC_OK;
			else if (rc >= 0)
				rc = VC_ERR;
		}
		if (rc == VC_OK)
			plan_adopt(&d, &p);
		plan_free(&p);
	}
	side_close(&s);
	side_close(&d);
	if (rc == VC_OK)
	{
		VcBranch	nb = {0};

		nb.name = pstrdup(branch);
		nb.head = lb.head;
		nb.head_root = lb.head_root;
		nb.working = lb.head_root;
		if (had)
			branch_write(remote, &nb);
		else if (!branch_add(remote, &nb))
			rc = VC_ERR;		/* (made meanwhile: push again) */
		vc_branch_free(&nb);
	}
	if (had)
		vc_branch_free(&rb);
	vc_branch_free(&lb);
	return rc;
}

int
vc_fetch(VcRepo *local, VcRepo *remote, const char *branch, PHash *head)
{
	VcBranch	rb;
	int			rc;

	if (vc_history_mode(local) == VC_HISTORY_REWRITE ||
		vc_history_mode(remote) == VC_HISTORY_REWRITE)
		return VC_ERR_REWRITTEN;
	if (!branch_read(remote, branch, VC_REF_READ, &rb))
		return VC_ERR;
	rc = transfer(remote, local, &rb.head, 1, 0, false);
	if (rc == VC_OK)
		*head = rb.head;
	vc_branch_free(&rb);
	return rc;
}

typedef struct CloneArg
{
	VcBranch   *v;
	size_t		n;
	size_t		cap;
} CloneArg;

static void
clone_branch_fn(void *arg, const VcBranch *b)
{
	CloneArg   *a = arg;

	px_grow((void **) &a->v, &a->cap, a->n + 1, sizeof(VcBranch));
	a->v[a->n] = *b;
	a->v[a->n].name = pstrdup(b->name);
	a->n++;
}

int
vc_repo_clone(VcRepo *dst, ChunkStore *cs, VcRefs *refs, const ChunkPolicy *policy,
			  VcRepo *src, uint32 depth)
{
	CloneArg	a = {0};
	PHash	   *heads;
	PHash		h;
	int			rc;

	memset(dst, 0, sizeof(*dst));
	prolly_ctx_init(&dst->ctx, cs, policy);
	dst->refs = refs;
	dst->cur = pstrdup(src->cur);
	if (vc_history_mode(src) == VC_HISTORY_REWRITE)
		return VC_ERR_REWRITTEN;
	/* into a store with no repository, whose state is made first */
	{
		RepoState	st;

		vc_must(&dst->ctx, prolly_empty(&dst->ctx, 0, &st.loc_root));
		st.next_seg = 0;
		st.forgot_gen = 0;
		st.history = VC_HISTORY_KEEP;
		st.name = "";
		st.hints = NULL;
		st.nhints = 0;
		h = repo_put(&dst->ctx, &st);
		if (!vc_refs_add(refs, REPO_REF, &h))
			return VC_ERR;
	}
	vc_branch_list(src, clone_branch_fn, &a);
	heads = palloc_array(PHash, Max(a.n, 1));
	for (size_t i = 0; i < a.n; i++)
		heads[i] = a.v[i].head;
	rc = transfer(src, dst, heads, a.n, depth, true);
	for (size_t i = 0; i < a.n; i++)
	{
		if (rc == VC_OK)
		{
			a.v[i].working = a.v[i].head_root;
			a.v[i].merging = false;
			(void) branch_add(dst, &a.v[i]);
		}
		vc_branch_free(&a.v[i]);
	}
	pfree(heads);
	if (a.v)
		px_free(a.v);
	return rc;
}

/*
 * Fetches history past a cut from any chunk source (a remote's store, an
 * archive, another repository's), verified by hash, into a new local
 * segment: old history comes back without entering the window.  The walk
 * starts at from and goes back generations (0: as far as the source has),
 * stopping at commits this repository holds or has exported.
 */
int
vc_deepen(VcRepo *r, const VcSource *src, const PHash *from,
		  uint32 generations, uint32 *seg_out)
{
	ProllyCtx	sctx;
	Side		d;
	HashStack	stack = {0};
	PHashMap	seen;
	GenEnt	   *todo = NULL;
	size_t		ntodo = 0,
				todocap = 0;
	HashStack	bounds = {0};
	VcCommit	c;
	uint64		bound;
	bool		created;
	VcLocation	where;

	*seg_out = VC_NO_SEGMENT;
	window_call_isolation("a deepening");
	if (vc_history_mode(r) == VC_HISTORY_REWRITE)
		return VC_ERR_REWRITTEN;
	where = vc_locate(r, from, NULL);
	if (where != VC_LOC_UNKNOWN && where != VC_LOC_BOUNDARY &&
		where != VC_LOC_DISCARDED)
		return VC_ERR;			/* present here, or exported: attach it */
	prolly_ctx_init(&sctx, src->cs, &r->ctx.policy);
	if (!vc_commit_load(&sctx, from, &c))
	{
		prolly_ctx_free(&sctx);
		r->ctx.err_hash = *from;
		return VC_ERR_SEVERED;
	}
	bound = generations && c.generation > generations ? c.generation - generations : 0;
	vc_commit_release(&c);
	side_open(&d, r, VC_REF_UPDATE);
	if (d.st.next_seg >= VC_NO_SEGMENT)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("the repository has made its last segment")));
	phmap_init(&seen);
	stack_push(&stack, from);
	while (stack.n > 0)
	{
		PHash		h = stack.v[--stack.n];
		uint64		v;

		CHECK_FOR_INTERRUPTS();
		if (vc_in_window(r, &h))
			continue;
		v = loc_get(r, &d.st.loc_root, &h);
		if (LOC_IS_SEG(v))
			continue;
		phmap_ref(&seen, &h, &created);
		if (!created)
			continue;
		if (!vc_commit_load(&sctx, &h, &c))
			continue;			/* the source lacks it: stays a boundary */
		px_grow((void **) &todo, &todocap, ntodo + 1, sizeof(GenEnt));
		todo[ntodo].gen = c.generation;
		todo[ntodo].h = h;
		ntodo++;
		for (uint32 i = 0; i < c.nparents; i++)
		{
			if (c.parent_gen[i] > bound)
				stack_push(&stack, &c.parents[i]);
			else if (!vc_in_window(r, &c.parents[i]) &&
					 !LOC_IS_SEG(loc_get(r, &d.st.loc_root, &c.parents[i])))
				stack_push(&bounds, &c.parents[i]);
		}
		vc_commit_release(&c);
	}
	if (ntodo > PG_INT32_MAX)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("a deepening fetches at most %d commits", PG_INT32_MAX)));
	if (ntodo > 0)
	{
		VcSegment	seg = {0};
		LocEdits	le = {0};
		uint8	   *roots = palloc(ntodo * PHASH_LEN);
		uint32		hint = hint_number(&d.st, src->name);

		if (ntodo > 1)
			qsort(todo, ntodo, sizeof(GenEnt), gen_cmp);
		for (size_t i = 0; i < ntodo; i++)
			memcpy(roots + i * PHASH_LEN, todo[i].h.b, PHASH_LEN);
		(void) cs_copy_closure(src->cs, r->ctx.cs, roots, (int) ntodo, "deepen",
							   src->name ? src->name : "the source");
		pfree(roots);
		seg.id = (uint32) d.st.next_seg;
		seg.commits = palloc_array(PHash, ntodo);
		seg.n = ntodo;
		for (size_t i = 0; i < ntodo; i++)
		{
			seg.commits[i] = todo[i].h;
			loc_set(&le, &todo[i].h, LOC_SEG(seg.id, false));
		}
		segment_write(r, &seg);
		for (size_t i = 0; i < bounds.n; i++)
		{
			uint64		v;

			if (phmap_find(&seen, &bounds.v[i]))
				continue;
			v = loc_get(r, &d.st.loc_root, &bounds.v[i]);
			if (v == LOC_GONE || (LOC_IS_BOUND(v) && LOC_BOUND_HINT(v) == 0))
				loc_set(&le, &bounds.v[i], LOC_BOUND(hint));
		}
		d.st.loc_root = loc_apply(r, &d.st.loc_root, &le);
		d.st.next_seg++;
		repo_write(r, &d.st);
		*seg_out = seg.id;
		pfree(seg.commits);
	}
	side_close(&d);
	prolly_ctx_free(&sctx);
	if (stack.v)
		px_free(stack.v);
	if (bounds.v)
		px_free(bounds.v);
	if (todo)
		px_free(todo);
	phmap_free(&seen);
	return VC_OK;
}

/*
 * Fetches branch from remote into a tracking branch "<remote>/<branch>"
 * and merges it into the current branch.  If the merge base lies past this
 * repository's cut and max_deepen > 0, deepens from the remote in growing
 * steps, up to max_deepen generations in all, until the merge sees it.
 */
int
vc_pull(VcRepo *local, VcRepo *remote, const char *branch, uint32 max_deepen,
		VcMergeResult *res)
{
	PHash		head;
	RepoState	rst;
	char	   *tracking;
	VcBranch	tb;
	VcSource	s;
	uint32		step = 4,
				used = 0;
	bool		had_tracking;
	int			rc;

	memset(res, 0, sizeof(*res));
	/* (refused first, as push: before any branch is locked) */
	if (vc_history_mode(local) == VC_HISTORY_REWRITE ||
		vc_history_mode(remote) == VC_HISTORY_REWRITE)
		return VC_ERR_REWRITTEN;
	repo_read(remote, VC_REF_READ, &rst);
	if (rst.name[0] == '\0')
	{
		repo_state_free(&rst);
		return VC_ERR;			/* a remote names its tracking branches */
	}
	tracking = psprintf("%s/%s", rst.name, branch);
	s.name = rst.name;
	s.cs = remote->ctx.cs;

	/*
	 * (the branches first, the one checked out and the tracking one, then
	 * the repository's state, as the fetch and the merge take it: every
	 * call's order)
	 */
	{
		VcBranch	cur;

		lock_head(local, &cur);
		vc_branch_free(&cur);
	}
	had_tracking = branch_read(local, tracking, VC_REF_UPDATE, &tb);
	if ((rc = vc_fetch(local, remote, branch, &head)) != VC_OK)
	{
		if (had_tracking)
			vc_branch_free(&tb);
		pfree(tracking);
		repo_state_free(&rst);
		return rc;
	}
	{
		VcCommit	c = must_load_commit(&local->ctx, &head);
		bool		had = had_tracking;

		if (!had)
			tb.name = pstrdup(tracking);
		tb.head = head;
		tb.head_root = c.root;
		tb.working = c.root;
		tb.merging = false;
		vc_commit_release(&c);
		if (had)
			branch_write(local, &tb);
		else if (!branch_add(local, &tb))
			branch_write(local, &tb);
		vc_branch_free(&tb);
	}
	for (;;)
	{
		PHash		missing;
		uint32		g,
					seg;

		rc = vc_merge(local, tracking, res);
		if (rc != VC_ERR_SEVERED || used >= max_deepen)
			break;
		missing = local->ctx.err_hash;
		g = step < max_deepen - used ? step : max_deepen - used;
		if (vc_deepen(local, &s, &missing, g, &seg) != VC_OK ||
			seg == VC_NO_SEGMENT)
			break;
		used += g;
		step *= 2;
	}
	pfree(tracking);
	repo_state_free(&rst);
	return rc;
}
