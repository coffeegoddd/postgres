/*
 * vc.c
 *	  Root values, commits, branches, merge and remotes.
 *
 * Schema and commit layouts are in chunkfmt.h.  A root value is a map from
 * table name to (schema hash, data root hash), both held strongly.
 *
 * Holds: every commit is held by a root ref (history is kept whole until
 * the window decides otherwise), and each branch holds its working root.
 * Mutating repo calls release the context's pins once results are held.
 */
#include "vc.h"

#define ENTRY_LEN (2 * PHASH_LEN)
#define RV_VREFS 2				/* a table entry holds its schema and data root */

/*
 * The version-control layer does not yet handle history cut off by the
 * window, so a tree error here is fatal.
 */
static void
vc_must(ProllyCtx *ctx, int rc)
{
	char		hex[PHASH_HEX_LEN + 1];

	if (rc >= 0)
		return;
	phash_hex(&ctx->err_hash, hex);
	fprintf(stderr, "vc: tree error %d at chunk %s\n", rc, hex);
	abort();
}

/* ---------------------------------------------------------- root values */

static void
entry_encode(uint8_t out[ENTRY_LEN], const VcTable *t)
{
	memcpy(out, t->schema.b, PHASH_LEN);
	memcpy(out + PHASH_LEN, t->data.b, PHASH_LEN);
}

static void
entry_decode(const uint8_t *v, size_t vl, VcTable *t)
{
	if (vl != ENTRY_LEN)
	{
		fprintf(stderr, "vc: corrupt table entry\n");
		abort();
	}
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
	const uint8_t *v;
	size_t		vl;

	int			rc = prolly_get(ctx, rv, (const uint8_t *) name, strlen(name), &v, &vl);

	if (rc == PROLLY_NOT_FOUND)
		return false;
	vc_must(ctx, rc);
	entry_decode(v, vl, out);
	return true;
}

static PHash
put_entry(ProllyCtx *ctx, const PHash *rv, const char *name, const VcTable *t)
{
	uint8_t		val[ENTRY_LEN];
	ProllyEdit	e = {(const uint8_t *) name, strlen(name), val, ENTRY_LEN, false};

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

const uint8_t *
vc_schema_get(ProllyCtx *ctx, const PHash *schema, size_t *len)
{
	size_t		l;
	const uint8_t *d = cs_get(ctx->cs, schema, &l);

	if (d == NULL || l < 2 || d[0] != CHUNK_TAG_SCHEMA ||
		d[1] != CHUNK_FORMAT_VERSION)
		return NULL;
	*len = l - 2;
	return d + 2;
}

int
vc_table_create(ProllyCtx *ctx, const PHash *rv, const char *name,
				const void *schema, size_t slen, PHash *out)
{
	VcTable		t;

	if (vc_table_get(ctx, rv, name, &t))
		return -1;
	t.schema = put_schema(ctx, schema, slen);
	vc_must(ctx, prolly_empty(ctx, 0, &t.data));
	*out = put_entry(ctx, rv, name, &t);
	return 0;
}

int
vc_table_alter(ProllyCtx *ctx, const PHash *rv, const char *name,
			   const void *schema, size_t slen, PHash *out)
{
	VcTable		t;

	if (!vc_table_get(ctx, rv, name, &t))
		return -1;
	t.schema = put_schema(ctx, schema, slen);
	*out = put_entry(ctx, rv, name, &t);
	return 0;
}

int
vc_table_drop(ProllyCtx *ctx, const PHash *rv, const char *name, PHash *out)
{
	VcTable		t;
	ProllyEdit	e = {(const uint8_t *) name, strlen(name), NULL, 0, true};

	if (!vc_table_get(ctx, rv, name, &t))
		return -1;
	vc_must(ctx, prolly_apply(ctx, rv, &e, 1, out));
	return 0;
}

int
vc_table_edit(ProllyCtx *ctx, const PHash *rv, const char *name,
			  const ProllyEdit *edits, size_t n, PHash *out)
{
	VcTable		t;

	if (!vc_table_get(ctx, rv, name, &t))
		return -1;
	vc_must(ctx, prolly_apply(ctx, &t.data, edits, n, &t.data));
	*out = put_entry(ctx, rv, name, &t);
	return 0;
}

int
vc_table_set_data(ProllyCtx *ctx, const PHash *rv, const char *name,
				  const PHash *data, PHash *out)
{
	VcTable		t;

	if (!vc_table_get(ctx, rv, name, &t))
		return -1;
	t.data = *data;
	*out = put_entry(ctx, rv, name, &t);
	return 0;
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
add_conflict(ConflictList *l, VcConflictKind kind, const uint8_t *name,
			 size_t nlen, const uint8_t *key, size_t klen)
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
	uint8_t    *vals;
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
	free(edits);
	free(vals);
	prolly_diff_list_free(&dours);
	prolly_diff_list_free(&dtheirs);
}

void
vc_conflicts_free(VcConflict *c, size_t n)
{
	for (size_t i = 0; i < n; i++)
	{
		free(c[i].table);
		free(c[i].key);
	}
	free(c);
}

/* -------------------------------------------------------------- commits */

bool
vc_commit_load(ProllyCtx *ctx, const PHash *h, VcCommit *out)
{
	size_t		len;
	const uint8_t *d = cs_get(ctx->cs, h, &len);
	const uint8_t *p,
			   *end;
	uint64_t	np,
				ml;

	if (d == NULL || len < 2 + PHASH_LEN || d[0] != CHUNK_TAG_COMMIT ||
		d[1] != CHUNK_FORMAT_VERSION)
		return false;
	p = d + 2;
	end = d + len;
	out->hash = *h;
	memcpy(out->root.b, p, PHASH_LEN);
	p += PHASH_LEN;
	if (!read_uvarint(&p, end, &out->generation) ||
		!read_uvarint(&p, end, &np) || np > VC_MAX_PARENTS)
		return false;
	out->nparents = (uint32_t) np;
	for (uint32_t i = 0; i < out->nparents; i++)
	{
		if ((size_t) (end - p) < PHASH_LEN)
			return false;
		memcpy(out->parents[i].b, p, PHASH_LEN);
		p += PHASH_LEN;
		if (!read_uvarint(&p, end, &out->parent_gen[i]))
			return false;
	}
	if (!read_uvarint(&p, end, &out->time) || !read_uvarint(&p, end, &ml) ||
		ml != (uint64_t) (end - p))
		return false;
	out->msg = (const char *) p;
	out->msglen = ml;
	return true;
}

/* For commits that must exist: the parents of a commit being written. */
static VcCommit
must_load_commit(ProllyCtx *ctx, const PHash *h)
{
	VcCommit	c;

	if (!vc_commit_load(ctx, h, &c))
	{
		char		hex[PHASH_HEX_LEN + 1];

		phash_hex(h, hex);
		fprintf(stderr, "vc: bad or missing commit %s\n", hex);
		abort();
	}
	return c;
}

PHash
vc_commit_write(ProllyCtx *ctx, const PHash *root, const PHash *parents,
				uint32_t nparents, uint64_t time, const char *msg)
{
	Buf			b = BUF_INIT;
	uint64_t	gens[VC_MAX_PARENTS];
	uint64_t	gen = 0;
	PHash		h;

	if (nparents > VC_MAX_PARENTS)
		abort();
	for (uint32_t i = 0; i < nparents; i++)
	{
		gens[i] = must_load_commit(ctx, &parents[i]).generation;
		if (gens[i] > gen)
			gen = gens[i];
	}
	buf_append_byte(&b, CHUNK_TAG_COMMIT);
	buf_append_byte(&b, CHUNK_FORMAT_VERSION);
	buf_append(&b, root->b, PHASH_LEN);
	buf_append_uvarint(&b, gen + 1);
	buf_append_uvarint(&b, nparents);
	for (uint32_t i = 0; i < nparents; i++)
	{
		buf_append(&b, parents[i].b, PHASH_LEN);
		buf_append_uvarint(&b, gens[i]);
	}
	buf_append_uvarint(&b, time);
	buf_append_uvarint(&b, strlen(msg));
	buf_append(&b, msg, strlen(msg));
	vc_must(ctx, prolly_put_chunk(ctx, b.data, b.len, &h));
	buf_free(&b);
	return h;
}

/* Max-heap of commits ordered by generation. */
typedef struct GenEnt
{
	uint64_t	gen;
	PHash		h;
} GenEnt;

typedef struct GenHeap
{
	GenEnt	   *v;
	size_t		n;
	size_t		cap;
} GenHeap;

static void
heap_push(GenHeap *hp, uint64_t gen, const PHash *h)
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

/* Loads a commit that may lie beyond a cut; an absent one is VC_ERR_SEVERED. */
static int
load_commit(ProllyCtx *ctx, const PHash *h, VcCommit *out)
{
	if (vc_commit_load(ctx, h, out))
		return VC_OK;
	ctx->err_hash = *h;
	ctx->err = cs_has(ctx->cs, h) ? PROLLY_ERR_CORRUPT : VC_ERR_SEVERED;
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
	if ((gate && ((rc = gate(garg, a)) != VC_OK || (rc = gate(garg, b)) != VC_OK)) ||
		(rc = load_commit(ctx, a, &ca)) != VC_OK ||
		(rc = load_commit(ctx, b, &cb)) != VC_OK)
		return rc;
	phmap_init(&flags);
	*phmap_ref(&flags, a, &created) |= MB_A;
	*phmap_ref(&flags, b, &created) |= MB_B;
	heap_push(&hp, ca.generation, a);
	heap_push(&hp, cb.generation, b);
	while (hp.n > 0)
	{
		GenEnt		e = heap_pop(&hp);
		uint64_t   *f = phmap_find(&flags, &e.h);
		uint64_t	mine;
		VcCommit	c;

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
		for (uint32_t i = 0; i < c.nparents; i++)
		{
			uint64_t   *pf = phmap_ref(&flags, &c.parents[i], &created);

			if ((*pf & mine) != mine)
			{
				*pf |= mine;
				heap_push(&hp, c.parent_gen[i], &c.parents[i]);
			}
		}
	}
	free(hp.v);
	phmap_free(&flags);
	return result;
}

int
vc_merge_base(ProllyCtx *ctx, const PHash *a, const PHash *b, PHash *out)
{
	return merge_base_core(ctx, a, b, NULL, NULL, out);
}

/* ----------------------------------------------------------------- repo */

/*
 * Where each known commit is, in r->loc: held by the window's history,
 * a member of segment n (stored as n + LOC_SEG_BASE), or a boundary: a
 * commit named as a parent whose content this repo never had.
 */
#define LOC_WINDOW 1
#define LOC_SEG_BASE 2
#define LOC_BOUNDARY UINT64_MAX

static int	reach_commit(VcRepo *r, const PHash *h, unsigned flags, VcCommit *c);

static void
set_working(VcRepo *r, VcBranch *b, const PHash *rv)
{
	PHash		old = b->working;

	cs_ref_root(r->ctx.cs, rv);
	b->working = *rv;
	cs_unref_root(r->ctx.cs, &old);
	prolly_release_pins(&r->ctx);
}

static VcBranch *
add_branch(VcRepo *r, const char *name, const PHash *head, const PHash *root)
{
	VcBranch   *b;

	px_grow((void **) &r->br, &r->cap, r->nbr + 1, sizeof(VcBranch));
	b = &r->br[r->nbr++];
	memset(b, 0, sizeof(*b));
	b->name = px_memdup(name, strlen(name) + 1);
	b->head = *head;
	b->head_root = *root;
	b->working = *root;
	cs_ref_root(r->ctx.cs, root);
	return b;
}

static void
set_loc(VcRepo *r, const PHash *h, uint64_t where)
{
	bool		created;

	*phmap_ref(&r->loc, h, &created) = where;
}

static bool
in_window(VcRepo *r, const PHash *h)
{
	uint64_t   *v = phmap_find(&r->loc, h);

	return v && *v == LOC_WINDOW;
}

/* Writes a commit and keeps it in the window's history. */
static PHash
keep_commit(VcRepo *r, const PHash *root, const PHash *parents, uint32_t np,
			const char *msg)
{
	PHash		h = vc_commit_write(&r->ctx, root, parents, np, r->clock++, msg);

	cs_ref_root(r->ctx.cs, &h);
	set_loc(r, &h, LOC_WINDOW);
	return h;
}

static void
repo_init_empty(VcRepo *r, ChunkStore *cs, const ChunkPolicy *policy)
{
	memset(r, 0, sizeof(*r));
	prolly_ctx_init(&r->ctx, cs, policy);
	phmap_init(&r->loc);
}

void
vc_repo_init(VcRepo *r, ChunkStore *cs, const ChunkPolicy *policy)
{
	PHash		root,
				commit;

	repo_init_empty(r, cs, policy);
	root = vc_root_empty(&r->ctx);
	commit = keep_commit(r, &root, NULL, 0, "init");
	add_branch(r, "main", &commit, &root);
	prolly_release_pins(&r->ctx);
}

void
vc_repo_free(VcRepo *r)
{
	for (size_t i = 0; i < r->nbr; i++)
		free(r->br[i].name);
	free(r->br);
	for (size_t i = 0; i < r->nsegs; i++)
		free(r->segs[i].commits);
	free(r->segs);
	phmap_free(&r->loc);
	prolly_ctx_free(&r->ctx);
	memset(r, 0, sizeof(*r));
}

VcBranch *
vc_branch_find(VcRepo *r, const char *name)
{
	for (size_t i = 0; i < r->nbr; i++)
		if (strcmp(r->br[i].name, name) == 0)
			return &r->br[i];
	return NULL;
}

VcBranch *
vc_head(VcRepo *r)
{
	return &r->br[r->cur];
}

PHash
vc_working(VcRepo *r)
{
	return vc_head(r)->working;
}

void
vc_set_working(VcRepo *r, const PHash *rv)
{
	set_working(r, vc_head(r), rv);
}

int
vc_branch_create(VcRepo *r, const char *name, const PHash *commit)
{
	VcBranch   *cur = vc_head(r);
	PHash		head = cur->head,
				root = cur->head_root;

	if (vc_branch_find(r, name))
		return VC_ERR;
	if (commit && !phash_eq(commit, &cur->head))
	{
		VcCommit	c;
		int			rc;

		/* a commit outside the window must be attached before use */
		if ((rc = reach_commit(r, commit, VC_WINDOW_ONLY, &c)) != VC_OK)
			return rc;
		head = *commit;
		root = c.root;
	}
	add_branch(r, name, &head, &root);
	return VC_OK;
}

int
vc_checkout(VcRepo *r, const char *name)
{
	VcBranch   *b = vc_branch_find(r, name);

	if (b == NULL)
		return VC_ERR;
	r->cur = (size_t) (b - r->br);
	return VC_OK;
}

PHash
vc_commit(VcRepo *r, const char *msg)
{
	VcBranch   *b = vc_head(r);
	PHash		parents[2] = {b->head, b->merge_head};
	PHash		h = keep_commit(r, &b->working, parents, b->merging ? 2 : 1, msg);

	b->head = h;
	b->head_root = b->working;
	b->merging = false;
	prolly_release_pins(&r->ctx);
	return h;
}

int
vc_reset_hard(VcRepo *r, const PHash *commit)
{
	VcBranch   *b = vc_head(r);
	VcCommit	c;
	int			rc;

	if ((rc = reach_commit(r, commit, VC_WINDOW_ONLY, &c)) != VC_OK)
		return rc;
	b->head = *commit;
	b->head_root = c.root;
	b->merging = false;
	set_working(r, b, &c.root);
	return VC_OK;
}

int
vc_merge(VcRepo *r, const char *branch, VcMergeResult *res)
{
	VcBranch   *b = vc_head(r);
	VcBranch   *other = vc_branch_find(r, branch);
	PHash		ours,
				theirs,
				base,
				merged;
	VcCommit	cbase,
				ctheirs;
	int			rc;

	memset(res, 0, sizeof(*res));
	if (other == NULL || b->merging || !phash_eq(&b->working, &b->head_root))
		return VC_ERR;			/* unknown branch, or uncommitted changes */
	ours = b->head;
	theirs = other->head;
	rc = vc_merge_base(&r->ctx, &ours, &theirs, &base);
	if (rc != 1)
		return rc < 0 ? rc : VC_ERR;
	if (phash_eq(&base, &theirs))
	{
		res->status = VC_MERGE_UP_TO_DATE;
		res->commit = ours;
		return VC_OK;
	}
	if ((rc = load_commit(&r->ctx, &theirs, &ctheirs)) != VC_OK)
		return rc;
	if (phash_eq(&base, &ours))
	{
		b->head = theirs;
		b->head_root = ctheirs.root;
		set_working(r, b, &ctheirs.root);
		res->status = VC_MERGE_FAST_FORWARD;
		res->commit = theirs;
		return VC_OK;
	}
	/* the base may be beyond a cut even when both heads are not */
	if ((rc = load_commit(&r->ctx, &base, &cbase)) != VC_OK)
		return rc;
	vc_merge_roots(&r->ctx, &cbase.root, &b->head_root, &ctheirs.root, &merged,
				   &res->conflicts, &res->nconflicts);
	b->merging = true;
	b->merge_head = theirs;
	set_working(r, b, &merged);
	if (res->nconflicts > 0)
	{
		res->status = VC_MERGE_CONFLICTS;
		return VC_OK;
	}
	res->commit = vc_commit(r, "merge");
	res->status = VC_MERGE_COMMITTED;
	return VC_OK;
}

void
vc_merge_result_free(VcMergeResult *res)
{
	vc_conflicts_free(res->conflicts, res->nconflicts);
	memset(res, 0, sizeof(*res));
}

/* With VC_WINDOW_ONLY, a commit outside the window is refused, present or not. */
static int
reach_commit(VcRepo *r, const PHash *h, unsigned flags, VcCommit *c)
{
	if ((flags & VC_WINDOW_ONLY) && !in_window(r, h))
	{
		r->ctx.err_hash = *h;
		r->ctx.err = VC_ERR_OUTSIDE_WINDOW;
		return VC_ERR_OUTSIDE_WINDOW;
	}
	return load_commit(&r->ctx, h, c);
}

static int
window_gate(void *arg, const PHash *h)
{
	VcRepo	   *r = arg;

	if (in_window(r, h))
		return VC_OK;
	r->ctx.err_hash = *h;
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
	if (rc == 1 && only)
		rc = window_gate(r, out);
	return rc == VC_OK ? 1 : rc;
}

int
vc_diff_commits(VcRepo *r, const PHash *from, const PHash *to, unsigned flags,
				VcTableChangeFn fn, void *arg)
{
	VcCommit	a,
				b;
	int			rc;

	if ((rc = reach_commit(r, from, flags, &a)) != VC_OK ||
		(rc = reach_commit(r, to, flags, &b)) != VC_OK)
		return rc;
	vc_diff_tables(&r->ctx, &a.root, &b.root, fn, arg);
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
		int			rc = reach_commit(r, &h, flags, &c);

		if (rc != VC_OK)
			return rc;
		fn(arg, &c);
		if (c.nparents == 0)
			break;
		h = c.parents[0];
	}
	return VC_OK;
}

/* --------------------------------------------------------------- window */

/*
 * Adds to win every commit within depth generations of head: those with
 * generation > gen(head) - depth on some path from it.  win maps each
 * commit to 1 + the lowest generation bound it was expanded under, so a
 * commit reached again under a lower bound is expanded again.  Parent
 * generations live in the child, so nothing outside the window is read.
 */
static int
window_add(VcRepo *r, PHashMap *win, const PHash *head, uint64_t depth)
{
	VcCommit	c;
	PHash	   *stack = NULL;
	size_t		n = 0,
				cap = 0;
	uint64_t	bound;
	int			rc = load_commit(&r->ctx, head, &c);

	if (rc != VC_OK)
		return rc;
	bound = c.generation > depth ? c.generation - depth : 0;
	px_grow((void **) &stack, &cap, 1, sizeof(PHash));
	stack[n++] = *head;
	while (n > 0 && rc == VC_OK)
	{
		PHash		h = stack[--n];
		bool		created;
		uint64_t   *v = phmap_ref(win, &h, &created);

		if (!created && *v <= bound + 1)
			continue;
		*v = bound + 1;
		/* a wider window can reach commits already cut; they stay cut */
		if (!vc_commit_load(&r->ctx, &h, &c))
			continue;
		for (uint32_t i = 0; i < c.nparents; i++)
			if (c.parent_gen[i] > bound)
			{
				px_grow((void **) &stack, &cap, n + 1, sizeof(PHash));
				stack[n++] = c.parents[i];
			}
	}
	free(stack);
	return rc;
}

int
vc_sever(VcRepo *r, const VcWindow *w, uint32_t *seg_out)
{
	PHashMap	win;
	PHash	   *members = NULL;
	size_t		n = 0,
				cap = 0;
	int			rc = VC_OK;
	VcSegment  *seg;

	*seg_out = VC_NO_SEGMENT;
	if (w->depth == 0)
		return VC_ERR;			/* branch heads are always in the window */
	phmap_init(&win);
	for (size_t i = 0; i < r->nbr && rc == VC_OK; i++)
	{
		rc = window_add(r, &win, &r->br[i].head, w->depth);
		if (rc == VC_OK && r->br[i].merging)
			rc = window_add(r, &win, &r->br[i].merge_head, w->depth);
	}
	for (size_t i = 0; i < w->npinned && rc == VC_OK; i++)
		rc = window_add(r, &win, &w->pinned[i], 1);
	if (rc != VC_OK)
	{
		phmap_free(&win);
		return rc;
	}

	/* everything the window's history holds that the window no longer covers */
	for (size_t i = 0; i < r->loc.cap; i++)
		if (r->loc.used[i] && r->loc.vals[i] == LOC_WINDOW &&
			!phmap_find(&win, &r->loc.keys[i]))
		{
			px_grow((void **) &members, &cap, n + 1, sizeof(PHash));
			members[n++] = r->loc.keys[i];
		}
	phmap_free(&win);
	if (n == 0)
		return VC_OK;

	/* detach: the holds move to the segment; no chunk is read, copied or freed */
	px_grow((void **) &r->segs, &r->segcap, r->nsegs + 1, sizeof(VcSegment));
	seg = &r->segs[r->nsegs];
	memset(seg, 0, sizeof(*seg));
	seg->state = VC_SEG_LOCAL;
	seg->commits = members;
	seg->n = n;
	for (size_t i = 0; i < n; i++)
		*phmap_find(&r->loc, &members[i]) = LOC_SEG_BASE + r->nsegs;
	*seg_out = (uint32_t) r->nsegs++;
	return VC_OK;
}

int
vc_segment_export(VcRepo *r, uint32_t id, ChunkStore *archive)
{
	VcSegment  *seg;
	PinSet		ps;
	PHash		bad;

	if (id >= r->nsegs || archive == r->ctx.cs)
		return VC_ERR;
	seg = &r->segs[id];
	if (seg->state != VC_SEG_LOCAL)
		return VC_ERR;
	/* a self-contained copy first: each commit with its whole tree */
	pinset_init(&ps, archive);
	for (size_t i = 0; i < seg->n; i++)
	{
		int			rc = cs_copy_closure(r->ctx.cs, &ps, &seg->commits[i], true, &bad);

		if (rc != CS_OK)
		{
			pinset_free(&ps);
			r->ctx.err_hash = bad;
			return rc;
		}
	}
	for (size_t i = 0; i < seg->n; i++)
		cs_ref_root(archive, &seg->commits[i]);
	pinset_free(&ps);
	/* then let go locally: frees what only this segment reached */
	for (size_t i = 0; i < seg->n; i++)
		cs_unref_root(r->ctx.cs, &seg->commits[i]);
	seg->state = VC_SEG_EXPORTED;
	seg->archive = archive;
	return VC_OK;
}

int
vc_segment_discard(VcRepo *r, uint32_t id)
{
	VcSegment  *seg;

	if (id >= r->nsegs)
		return VC_ERR;
	seg = &r->segs[id];
	if (seg->state == VC_SEG_LOCAL)
		for (size_t i = 0; i < seg->n; i++)
			cs_unref_root(r->ctx.cs, &seg->commits[i]);
	else if (seg->state != VC_SEG_EXPORTED)
		return VC_ERR;
	seg->state = VC_SEG_DISCARDED;
	seg->archive = NULL;
	return VC_OK;
}

int
vc_segment_attach(VcRepo *r, uint32_t id)
{
	VcSegment  *seg;

	if (id >= r->nsegs)
		return VC_ERR;
	seg = &r->segs[id];
	if (seg->state == VC_SEG_DISCARDED)
		return VC_ERR_SEVERED;
	if (seg->state == VC_SEG_EXPORTED)
	{
		PHash		bad;

		for (size_t i = 0; i < seg->n; i++)
		{
			int			rc = cs_copy_closure(seg->archive, &r->ctx.pins,
											 &seg->commits[i], true, &bad);

			if (rc != CS_OK)
			{
				prolly_release_pins(&r->ctx);
				r->ctx.err_hash = bad;
				return rc;
			}
		}
		for (size_t i = 0; i < seg->n; i++)
			cs_ref_root(r->ctx.cs, &seg->commits[i]);
		prolly_release_pins(&r->ctx);
	}
	else if (seg->state != VC_SEG_LOCAL)
		return VC_ERR;
	/* the holds move back to the window's history */
	for (size_t i = 0; i < seg->n; i++)
		set_loc(r, &seg->commits[i], LOC_WINDOW);
	seg->state = VC_SEG_ATTACHED;
	seg->archive = NULL;
	return VC_OK;
}

VcLocation
vc_locate(VcRepo *r, const PHash *h, uint32_t *seg)
{
	uint64_t   *v = phmap_find(&r->loc, h);
	uint64_t	id;

	if (v == NULL)
		return VC_LOC_UNKNOWN;
	if (*v == LOC_WINDOW)
		return VC_LOC_WINDOW;
	if (*v == LOC_BOUNDARY)
		return VC_LOC_BOUNDARY;
	id = *v - LOC_SEG_BASE;
	if (seg)
		*seg = (uint32_t) id;
	switch (r->segs[id].state)
	{
		case VC_SEG_LOCAL:
			return VC_LOC_SEGMENT;
		case VC_SEG_EXPORTED:
			return VC_LOC_EXPORTED;
		case VC_SEG_DISCARDED:
			return VC_LOC_DISCARDED;
		default:
			return VC_LOC_UNKNOWN;
	}
}

/* -------------------------------------------------------------- remotes */

static int
gen_cmp(const void *pa, const void *pb)
{
	const GenEnt *a = pa;
	const GenEnt *b = pb;

	return (a->gen > b->gen) - (a->gen < b->gen);
}

/*
 * Copies every commit reachable from commit that dst lacks, each with the
 * strong closure of its root value, into dst's window history.  Parents
 * are weak references, so they are walked here, oldest first.  A parent
 * src does not have (cut from its history) becomes a boundary in dst.
 */
int
vc_sync_commit(VcRepo *src, VcRepo *dst, const PHash *commit)
{
	PHashMap	seen;
	GenEnt	   *todo = NULL;
	size_t		ntodo = 0,
				todocap = 0;
	PHash	   *stack = NULL;
	size_t		nstack = 0,
				stackcap = 0;
	bool		created;
	int			rc = VC_OK;

	phmap_init(&seen);
	px_grow((void **) &stack, &stackcap, 1, sizeof(PHash));
	stack[nstack++] = *commit;
	while (nstack > 0)
	{
		PHash		h = stack[--nstack];
		VcCommit	c;

		phmap_ref(&seen, &h, &created);
		if (!created || cs_has(dst->ctx.cs, &h))
			continue;
		if (!vc_commit_load(&src->ctx, &h, &c))
		{
			uint64_t   *v = phmap_ref(&dst->loc, &h, &created);

			if (created)
				*v = LOC_BOUNDARY;
			continue;
		}
		px_grow((void **) &todo, &todocap, ntodo + 1, sizeof(GenEnt));
		todo[ntodo].gen = c.generation;
		todo[ntodo].h = h;
		ntodo++;
		for (uint32_t i = 0; i < c.nparents; i++)
		{
			px_grow((void **) &stack, &stackcap, nstack + 1, sizeof(PHash));
			stack[nstack++] = c.parents[i];
		}
	}
	if (ntodo > 1)
		qsort(todo, ntodo, sizeof(GenEnt), gen_cmp);
	for (size_t i = 0; i < ntodo && rc == VC_OK; i++)
	{
		PHash		bad;

		rc = cs_copy_closure(src->ctx.cs, &dst->ctx.pins, &todo[i].h, true, &bad);
		if (rc != CS_OK)
		{
			dst->ctx.err_hash = bad;
			break;
		}
		cs_ref_root(dst->ctx.cs, &todo[i].h);
		set_loc(dst, &todo[i].h, LOC_WINDOW);
	}
	prolly_release_pins(&dst->ctx);
	free(todo);
	free(stack);
	phmap_free(&seen);
	return rc;
}

int
vc_push(VcRepo *local, VcRepo *remote, const char *branch)
{
	VcBranch   *lb = vc_branch_find(local, branch);
	VcBranch   *rb = vc_branch_find(remote, branch);
	PHash		base;
	int			rc;

	if (lb == NULL)
		return VC_ERR;
	if (rb != NULL)
	{
		/* fast-forward only: the remote head must be in our history */
		if (!cs_has(local->ctx.cs, &rb->head))
			return VC_ERR;
		rc = vc_merge_base(&local->ctx, &lb->head, &rb->head, &base);
		if (rc < 0)
			return rc;
		if (rc == 0 || !phash_eq(&base, &rb->head))
			return VC_ERR;
	}
	if ((rc = vc_sync_commit(local, remote, &lb->head)) != VC_OK)
		return rc;
	if (rb == NULL)
		add_branch(remote, branch, &lb->head, &lb->head_root);
	else
	{
		rb->head = lb->head;
		rb->head_root = lb->head_root;
		set_working(remote, rb, &lb->head_root);
	}
	return VC_OK;
}

int
vc_fetch(VcRepo *local, VcRepo *remote, const char *branch, PHash *head)
{
	VcBranch   *rb = vc_branch_find(remote, branch);
	int			rc;

	if (rb == NULL)
		return VC_ERR;
	if ((rc = vc_sync_commit(remote, local, &rb->head)) != VC_OK)
		return rc;
	*head = rb->head;
	return VC_OK;
}

int
vc_repo_clone(VcRepo *dst, ChunkStore *cs, VcRepo *src)
{
	repo_init_empty(dst, cs, &src->ctx.policy);
	dst->clock = src->clock;
	for (size_t i = 0; i < src->nbr; i++)
	{
		int			rc = vc_sync_commit(src, dst, &src->br[i].head);

		if (rc != VC_OK)
			return rc;
		add_branch(dst, src->br[i].name, &src->br[i].head, &src->br[i].head_root);
	}
	dst->cur = src->cur;
	return VC_OK;
}
