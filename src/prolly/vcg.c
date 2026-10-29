/*
 * vcg.c
 *	  Version control with parents in a commit graph.
 */
#define _DEFAULT_SOURCE
#include <sys/random.h>

#include "vcg.h"
#include "vc.h"

static void
vcg_must(ProllyCtx *ctx, int rc)
{
	char		hex[PHASH_HEX_LEN + 1];

	if (rc >= 0)
		return;
	phash_hex(&ctx->err_hash, hex);
	fprintf(stderr, "vcg: tree error %d at chunk %s\n", rc, hex);
	abort();
}

/* -------------------------------------------------------------- commits */

bool
vcg_commit_load(ProllyCtx *ctx, const PHash *h, VcgCommit *out)
{
	size_t		len;
	const uint8_t *d = cs_get(ctx->cs, h, &len);
	const uint8_t *p,
			   *end;
	uint64_t	ml;

	if (d == NULL || len < 2 + PHASH_LEN || d[0] != CHUNK_TAG_GCOMMIT ||
		d[1] != CHUNK_FORMAT_VERSION)
		return false;
	p = d + 2;
	end = d + len;
	out->hash = *h;
	memcpy(out->root.b, p, PHASH_LEN);
	p += PHASH_LEN;
	if (!read_uvarint(&p, end, &out->generation) || out->generation == 0 ||
		end - p < VCG_NONCE_LEN)
		return false;
	memcpy(out->nonce, p, VCG_NONCE_LEN);
	p += VCG_NONCE_LEN;
	if (!read_uvarint(&p, end, &out->time) || !read_uvarint(&p, end, &ml) ||
		ml != (uint64_t) (end - p))
		return false;
	out->msg = (const char *) p;
	out->msglen = ml;
	return true;
}

PHash
vcg_commit_write(ProllyCtx *ctx, const PHash *root, uint64_t generation,
				 const uint8_t *nonce, uint64_t time, const char *msg)
{
	Buf			b = BUF_INIT;
	uint8_t		fresh[VCG_NONCE_LEN];
	PHash		h;

	if (nonce == NULL)
	{
		if (getrandom(fresh, sizeof(fresh), 0) != (ssize_t) sizeof(fresh))
			abort();
		nonce = fresh;
	}
	buf_append_byte(&b, CHUNK_TAG_GCOMMIT);
	buf_append_byte(&b, CHUNK_FORMAT_VERSION);
	buf_append(&b, root->b, PHASH_LEN);
	buf_append_uvarint(&b, generation);
	buf_append(&b, nonce, VCG_NONCE_LEN);
	buf_append_uvarint(&b, time);
	buf_append_uvarint(&b, strlen(msg));
	buf_append(&b, msg, strlen(msg));
	vcg_must(ctx, prolly_put_chunk(ctx, b.data, b.len, &h));
	buf_free(&b);
	return h;
}

/* ----------------------------------------------------------------- repo */

static uint64_t
held_gen(VcgRepo *r, const PHash *h)
{
	uint64_t   *v = phmap_find(&r->held, h);

	return v ? *v : 0;
}

/* A commit enters the window's history: the repo holds it. */
static void
hold(VcgRepo *r, const PHash *h, uint64_t gen)
{
	bool		created;
	uint64_t   *v = phmap_ref(&r->held, h, &created);

	if (created)
		cs_ref_root(r->ctx.cs, h);
	*v = gen;
}

static void
put_edge(VcgRepo *r, uint64_t gen, const PHash *h, const CgEdge *e)
{
	CgPut		p = {gen, *h, e};

	vcg_must(&r->ctx, r->graph->ops->apply(r->graph, &p, 1));
}

static VcgBranch *
add_branch(VcgRepo *r, const char *name, const PHash *head)
{
	VcgBranch  *b;

	px_grow((void **) &r->br, &r->cap, r->nbr + 1, sizeof(VcgBranch));
	b = &r->br[r->nbr++];
	b->name = px_memdup(name, strlen(name) + 1);
	b->head = *head;
	return b;
}

void
vcg_repo_init_empty(VcgRepo *r, ChunkStore *cs, const ChunkPolicy *policy)
{
	memset(r, 0, sizeof(*r));
	prolly_ctx_init(&r->ctx, cs, policy);
	phmap_init(&r->held);
	r->graph = cgraph_prolly_open(&r->ctx, NULL, true);
	prolly_release_pins(&r->ctx);
}

void
vcg_repo_init(VcgRepo *r, ChunkStore *cs, const ChunkPolicy *policy)
{
	PHash		root,
				h;
	CgEdge		e = {0};

	vcg_repo_init_empty(r, cs, policy);
	root = vc_root_empty(&r->ctx);
	h = vcg_commit_write(&r->ctx, &root, 1, NULL, r->clock++, "init");
	hold(r, &h, 1);
	put_edge(r, 1, &h, &e);
	add_branch(r, "main", &h);
	prolly_release_pins(&r->ctx);
}

void
vcg_repo_free(VcgRepo *r)
{
	for (size_t i = 0; i < r->nbr; i++)
		free(r->br[i].name);
	free(r->br);
	free(r->segs);
	phmap_free(&r->held);
	/* the graph's hold stays: the store's roots outlive the repo */
	free(r->graph);
	prolly_ctx_free(&r->ctx);
	memset(r, 0, sizeof(*r));
}

VcgBranch *
vcg_head(VcgRepo *r)
{
	return r->nbr ? &r->br[r->cur] : NULL;
}

VcgBranch *
vcg_branch_find(VcgRepo *r, const char *name)
{
	for (size_t i = 0; i < r->nbr; i++)
		if (strcmp(r->br[i].name, name) == 0)
			return &r->br[i];
	return NULL;
}

int
vcg_branch_create(VcgRepo *r, const char *name, const PHash *commit)
{
	if (vcg_branch_find(r, name) || held_gen(r, commit) == 0)
		return VCG_ERR;
	add_branch(r, name, commit);
	return VCG_OK;
}

int
vcg_checkout(VcgRepo *r, const char *name)
{
	VcgBranch  *b = vcg_branch_find(r, name);

	if (b == NULL)
		return VCG_ERR;
	r->cur = (size_t) (b - r->br);
	return VCG_OK;
}

int
vcg_branch_delete(VcgRepo *r, const char *name)
{
	VcgBranch  *b = vcg_branch_find(r, name);
	size_t		i;

	if (b == NULL || (size_t) (b - r->br) == r->cur)
		return VCG_ERR;
	i = (size_t) (b - r->br);
	free(b->name);
	memmove(&r->br[i], &r->br[i + 1], (r->nbr - i - 1) * sizeof(VcgBranch));
	r->nbr--;
	if (r->cur > i)
		r->cur--;
	return VCG_OK;
}

static PHash
commit_parents(VcgRepo *r, const PHash *root, const PHash *parents, uint32_t np,
			   const char *msg)
{
	CgEdge		e = {0};
	uint64_t	gen = 0;
	PHash		h;

	e.nparents = np;
	for (uint32_t i = 0; i < np; i++)
	{
		e.parents[i] = parents[i];
		e.parent_gen[i] = held_gen(r, &parents[i]);
		if (e.parent_gen[i] == 0)
			abort();			/* heads are always held */
		if (e.parent_gen[i] > gen)
			gen = e.parent_gen[i];
	}
	h = vcg_commit_write(&r->ctx, root, gen + 1, NULL, r->clock++, msg);
	hold(r, &h, gen + 1);
	put_edge(r, gen + 1, &h, &e);
	prolly_release_pins(&r->ctx);
	return h;
}

PHash
vcg_commit(VcgRepo *r, const PHash *root, const char *msg)
{
	VcgBranch  *b = vcg_head(r);
	PHash		h = commit_parents(r, root, &b->head, 1, msg);

	b->head = h;
	return h;
}

/* The generation of a commit, from the repo or its chunk. */
static bool
commit_gen(VcgRepo *r, const PHash *h, uint64_t *gen)
{
	VcgCommit	c;

	if ((*gen = held_gen(r, h)) != 0)
		return true;
	if (!vcg_commit_load(&r->ctx, h, &c))
		return false;
	*gen = c.generation;
	return true;
}

int
vcg_edge(VcgRepo *r, const PHash *commit, CgEdge *out)
{
	uint64_t	gen;

	if (!commit_gen(r, commit, &gen))
		return PROLLY_NOT_FOUND;
	return r->graph->ops->get(r->graph, gen, commit, out);
}

/* -------------------------------------------------------------- history */

int
vcg_log(VcgRepo *r, const PHash *start, size_t max, VcgLogFn fn, void *arg)
{
	PHash		h = *start;

	for (size_t i = 0; i < max; i++)
	{
		VcgCommit	c;
		CgEdge		e;
		int			rc;

		if (!vcg_commit_load(&r->ctx, &h, &c))
			return VCG_ERR_SEVERED;
		if ((rc = r->graph->ops->get(r->graph, c.generation, &h, &e)) != PROLLY_OK)
			return rc == PROLLY_NOT_FOUND ? VCG_ERR_SEVERED : rc;
		fn(arg, &c);
		if (e.nparents == 0)
			break;
		h = e.parents[0];
	}
	return VCG_OK;
}

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

/*
 * Newest generation first, so the first commit reached from both sides is
 * a best common ancestor; only edges are read.
 */
int
vcg_merge_base(VcgRepo *r, const PHash *a, const PHash *b, PHash *out)
{
	PHashMap	flags;
	GenHeap		hp = {0};
	bool		created,
				cut = false;
	int			result = 0;
	uint64_t	ga,
				gb;

	if (phash_eq(a, b))
	{
		*out = *a;
		return 1;
	}
	if (!commit_gen(r, a, &ga) || !commit_gen(r, b, &gb))
		return VCG_ERR;
	phmap_init(&flags);
	*phmap_ref(&flags, a, &created) |= MB_A;
	*phmap_ref(&flags, b, &created) |= MB_B;
	heap_push(&hp, ga, a);
	heap_push(&hp, gb, b);
	while (hp.n > 0)
	{
		GenEnt		e = heap_pop(&hp);
		uint64_t   *f = phmap_find(&flags, &e.h);
		uint64_t	mine;
		CgEdge		edge;
		int			rc;

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
		rc = r->graph->ops->get(r->graph, e.gen, &e.h, &edge);
		if (rc != PROLLY_OK)
		{
			result = rc == PROLLY_NOT_FOUND ? VCG_ERR_SEVERED : rc;
			break;
		}
		if (edge.nseams)
			cut = true;
		for (uint32_t i = 0; i < edge.nparents; i++)
		{
			uint64_t   *pf = phmap_ref(&flags, &edge.parents[i], &created);

			if ((*pf & mine) != mine)
			{
				*pf |= mine;
				heap_push(&hp, edge.parent_gen[i], &edge.parents[i]);
			}
		}
	}
	free(hp.v);
	phmap_free(&flags);
	if (result == 0 && cut)
		return VCG_ERR_SEVERED;
	return result;
}

int
vcg_merge(VcgRepo *r, const char *branch, size_t *nconflicts)
{
	VcgBranch  *ours = vcg_head(r),
			   *theirs = vcg_branch_find(r, branch);
	PHash		base,
				merged,
				parents[2];
	VcgCommit	cb,
				co,
				ct;
	VcConflict *conf;
	int			rc;

	*nconflicts = 0;
	if (theirs == NULL)
		return VCG_ERR;
	if ((rc = vcg_merge_base(r, &ours->head, &theirs->head, &base)) != 1)
		return rc == 0 ? VCG_ERR_UNRELATED : rc;
	if (phash_eq(&base, &theirs->head))
		return VCG_OK;
	if (phash_eq(&base, &ours->head))
	{
		ours->head = theirs->head;
		return VCG_OK;
	}
	if (!vcg_commit_load(&r->ctx, &base, &cb) || !vcg_commit_load(&r->ctx, &ours->head, &co) ||
		!vcg_commit_load(&r->ctx, &theirs->head, &ct))
		return VCG_ERR_SEVERED;
	vc_merge_roots(&r->ctx, &cb.root, &co.root, &ct.root, &merged, &conf, nconflicts);
	vc_conflicts_free(conf, *nconflicts);
	if (*nconflicts)
	{
		prolly_release_pins(&r->ctx);
		return VCG_OK;
	}
	parents[0] = ours->head;
	parents[1] = theirs->head;
	ours->head = commit_parents(r, &merged, parents, 2, "merge");
	return VCG_OK;
}

/* -------------------------------------------------------------- sever */

/* Adds every commit within depth generations of head, following edges. */
static void
window_add(VcgRepo *r, PHashMap *win, const PHash *head, uint64_t depth)
{
	uint64_t	gen = held_gen(r, head),
				bound = gen > depth ? gen - depth : 0;
	GenEnt	   *stack = NULL;
	size_t		n = 0,
				cap = 0;

	px_grow((void **) &stack, &cap, 1, sizeof(GenEnt));
	stack[n++] = (GenEnt) {gen, *head};
	while (n > 0)
	{
		GenEnt		e = stack[--n];
		bool		created;
		uint64_t   *v = phmap_ref(win, &e.h, &created);
		CgEdge		edge;

		if (!created && *v <= bound + 1)
			continue;
		*v = bound + 1;
		if (r->graph->ops->get(r->graph, e.gen, &e.h, &edge) != PROLLY_OK)
			continue;
		for (uint32_t i = 0; i < edge.nparents; i++)
			if (edge.parent_gen[i] > bound)
			{
				px_grow((void **) &stack, &cap, n + 1, sizeof(GenEnt));
				stack[n++] = (GenEnt) {edge.parent_gen[i], edge.parents[i]};
			}
	}
	free(stack);
}

static VcgSegment *
add_segment(VcgRepo *r, VcgSegState state, const PHash *hash)
{
	VcgSegment *s;

	px_grow((void **) &r->segs, &r->segcap, r->nsegs + 1, sizeof(VcgSegment));
	s = &r->segs[r->nsegs++];
	memset(s, 0, sizeof(*s));
	s->state = state;
	s->hash = *hash;
	return s;
}

int
vcg_sever(VcgRepo *r, uint32_t depth, uint32_t *seg_out)
{
	PHashMap	win,
				cut;
	CgPut	   *seg_puts = NULL,
			   *graph_puts = NULL;
	CgEdge	   *edges = NULL;
	size_t		nseg = 0,
				ngraph = 0,
				nedges = 0,
				cap1 = 0,
				cap2 = 0,
				cap3 = 0;
	CommitGraph *sg;
	PHash		H;
	bool		created;

	*seg_out = VCG_NO_SEGMENT;
	if (depth == 0)
		return VCG_ERR;
	phmap_init(&win);
	for (size_t i = 0; i < r->nbr; i++)
		window_add(r, &win, &r->br[i].head, depth);

	/* the cut: commits held that the window no longer covers */
	phmap_init(&cut);
	for (size_t i = 0; i < r->held.cap; i++)
		if (r->held.used[i] && !phmap_find(&win, &r->held.keys[i]))
			*phmap_ref(&cut, &r->held.keys[i], &created) = r->held.vals[i];
	if (cut.n == 0)
	{
		phmap_free(&win);
		phmap_free(&cut);
		return VCG_OK;
	}

	/*
	 * Edges are copied into an array sized up front, since puts point at
	 * them: every cut commit's edge, and every kept edge that names one.
	 */
	px_grow((void **) &edges, &cap3, 2 * r->held.n, sizeof(CgEdge));
	for (size_t i = 0; i < r->held.cap; i++)
	{
		const PHash *h;
		uint64_t	gen;
		CgEdge		e;
		bool		is_cut,
					names_cut = false;

		if (!r->held.used[i])
			continue;
		h = &r->held.keys[i];
		gen = r->held.vals[i];
		vcg_must(&r->ctx, r->graph->ops->get(r->graph, gen, h, &e));
		is_cut = phmap_find(&cut, h) != NULL;
		for (uint32_t j = 0; j < e.nparents && !is_cut; j++)
			if (phmap_find(&cut, &e.parents[j]))
				names_cut = true;
		if (!is_cut && !names_cut)
			continue;
		edges[nedges] = e;
		if (names_cut)
			edges[nedges].flags |= CG_SEAM_CHILD;
		px_grow((void **) &seg_puts, &cap1, nseg + 1, sizeof(CgPut));
		seg_puts[nseg++] = (CgPut) {gen, *h, &edges[nedges]};
		nedges++;
		px_grow((void **) &graph_puts, &cap2, ngraph + 1, sizeof(CgPut));
		if (is_cut)
			graph_puts[ngraph++] = (CgPut) {gen, *h, NULL};
		else
			graph_puts[ngraph++] = (CgPut) {gen, *h, &edges[nedges]};	/* filled below */
		if (!is_cut)
			nedges++;
	}

	/* the segment: its graph's root names it */
	sg = cgraph_prolly_open(&r->ctx, NULL, false);
	vcg_must(&r->ctx, sg->ops->apply(sg, seg_puts, nseg));
	H = sg->ops->root(sg);
	sg->ops->destroy(sg);
	cs_ref_root(r->ctx.cs, &H);

	/* kept commits lose their cut parents and record where they went */
	for (size_t i = 0; i < ngraph; i++)
		if (graph_puts[i].edge)
		{
			CgEdge	   *ke = (CgEdge *) graph_puts[i].edge;
			const CgEdge *se = ke - 1;
			uint32_t	m = 0;

			*ke = *se;
			ke->flags = 0;
			for (uint32_t j = 0; j < se->nparents; j++)
				if (!phmap_find(&cut, &se->parents[j]))
				{
					ke->parents[m] = se->parents[j];
					ke->parent_gen[m++] = se->parent_gen[j];
				}
			ke->nparents = m;
			if (ke->nseams >= CG_MAX_SEAMS)
				abort();
			ke->seams[ke->nseams++] = H;
		}
	vcg_must(&r->ctx, r->graph->ops->apply(r->graph, graph_puts, ngraph));

	/* the cut commits stay held, by the segment now */
	for (size_t i = 0; i < cut.cap; i++)
		if (cut.used[i])
			phmap_remove(&r->held, &cut.keys[i]);
	add_segment(r, VCG_SEG_LOCAL, &H);
	*seg_out = (uint32_t) (r->nsegs - 1);

	free(seg_puts);
	free(graph_puts);
	free(edges);
	phmap_free(&win);
	phmap_free(&cut);
	prolly_release_pins(&r->ctx);
	return VCG_OK;
}

uint32_t
vcg_segment_find(VcgRepo *r, const PHash *hash)
{
	for (size_t i = 0; i < r->nsegs; i++)
		if (phash_eq(&r->segs[i].hash, hash))
			return (uint32_t) i;
	add_segment(r, VCG_SEG_DISCARDED, hash);
	return (uint32_t) (r->nsegs - 1);
}

typedef struct MemberAcc
{
	PHash	   *h;
	uint64_t   *gen;
	size_t		n,
				cap,
				gcap;
} MemberAcc;

static int
member_fn(void *arg, uint64_t gen, const PHash *commit, const CgEdge *e)
{
	MemberAcc  *m = arg;

	if (e->flags & CG_SEAM_CHILD)
		return 0;
	px_grow((void **) &m->h, &m->cap, m->n + 1, sizeof(PHash));
	px_grow((void **) &m->gen, &m->gcap, m->n + 1, sizeof(uint64_t));
	m->h[m->n] = *commit;
	m->gen[m->n++] = gen;
	return 0;
}

/* The commits a segment's graph cut, read from ctx's store. */
static int
segment_members(ProllyCtx *ctx, const PHash *H, MemberAcc *m)
{
	CommitGraph *sg = cgraph_prolly_open(ctx, H, false);
	int			rc;

	memset(m, 0, sizeof(*m));
	rc = sg->ops->foreach(sg, member_fn, m);
	sg->ops->destroy(sg);
	return rc;
}

static void
member_free(MemberAcc *m)
{
	free(m->h);
	free(m->gen);
}

int
vcg_segment_discard(VcgRepo *r, uint32_t id)
{
	VcgSegment *s;

	if (id >= r->nsegs)
		return VCG_ERR;
	s = &r->segs[id];
	if (s->state == VCG_SEG_LOCAL)
	{
		MemberAcc	m;

		vcg_must(&r->ctx, segment_members(&r->ctx, &s->hash, &m));
		for (size_t i = 0; i < m.n; i++)
			cs_unref_root(r->ctx.cs, &m.h[i]);
		member_free(&m);
		cs_unref_root(r->ctx.cs, &s->hash);
	}
	else if (s->state != VCG_SEG_EXPORTED)
		return VCG_ERR;
	s->state = VCG_SEG_DISCARDED;
	s->archive = NULL;
	return VCG_OK;
}

int
vcg_segment_export(VcgRepo *r, uint32_t id, ChunkStore *archive)
{
	VcgSegment *s;
	MemberAcc	m;
	PinSet		pins;
	PHash		bad;
	int			rc;

	if (id >= r->nsegs || r->segs[id].state != VCG_SEG_LOCAL)
		return VCG_ERR;
	s = &r->segs[id];
	vcg_must(&r->ctx, segment_members(&r->ctx, &s->hash, &m));
	pinset_init(&pins, archive);
	rc = cs_copy_closure(r->ctx.cs, &pins, &s->hash, false, &bad);
	for (size_t i = 0; i < m.n && rc == CS_OK; i++)
		rc = cs_copy_closure(r->ctx.cs, &pins, &m.h[i], false, &bad);
	if (rc == CS_OK)
	{
		cs_ref_root(archive, &s->hash);
		for (size_t i = 0; i < m.n; i++)
			cs_ref_root(archive, &m.h[i]);
	}
	pinset_free(&pins);
	member_free(&m);
	if (rc != CS_OK)
		return rc;
	vcg_must(&r->ctx, vcg_segment_discard(r, id));
	s->state = VCG_SEG_EXPORTED;
	s->archive = archive;
	return VCG_OK;
}

typedef struct RelinkAcc
{
	VcgRepo    *r;
	const PHash *H;
	PHashMap	members;		/* commit -> generation */
	CgPut	   *puts;
	CgEdge	   *edges;
	size_t		n,
				cap,
				ecap;
	int			err;
} RelinkAcc;

static int
relink_members_fn(void *arg, uint64_t gen, const PHash *commit, const CgEdge *e)
{
	RelinkAcc  *a = arg;
	bool		created;

	if (!(e->flags & CG_SEAM_CHILD))
		*phmap_ref(&a->members, commit, &created) = gen;
	return 0;
}

/*
 * One edge of the segment: a cut commit gets its edge back as it was; a
 * kept commit gets back the parents it lost, and no longer names H.
 */
static int
relink_edge_fn(void *arg, uint64_t gen, const PHash *commit, const CgEdge *e)
{
	RelinkAcc  *a = arg;
	CgEdge	   *out;
	uint64_t	maxgen = 0;

	for (uint32_t i = 0; i < e->nparents; i++)
	{
		uint64_t   *mg = phmap_find(&a->members, &e->parents[i]);

		if (mg && *mg != e->parent_gen[i])
			return VCG_ERR_FORGED;
		if (e->parent_gen[i] >= gen)
			return VCG_ERR_FORGED;
		if (e->parent_gen[i] > maxgen)
			maxgen = e->parent_gen[i];
	}
	if (e->nseams == 0 && gen != maxgen + 1)
		return VCG_ERR_FORGED;
	px_grow((void **) &a->puts, &a->cap, a->n + 1, sizeof(CgPut));
	if (a->n >= a->ecap)
		return VCG_ERR;			/* sized from the count */
	out = &a->edges[a->n];
	if (e->flags & CG_SEAM_CHILD)
	{
		CgEdge		cur;
		uint32_t	m = 0;
		bool		names = false;

		if (a->r->graph->ops->get(a->r->graph, gen, commit, &cur) != PROLLY_OK)
			return VCG_ERR_ORDER;
		for (uint32_t i = 0; i < cur.nseams; i++)
			if (phash_eq(&cur.seams[i], a->H))
				names = true;
			else
				cur.seams[m++] = cur.seams[i];
		if (!names)
			return VCG_ERR_ORDER;
		/* every parent it gets back is cut here or still held */
		for (uint32_t i = 0; i < e->nparents; i++)
			if (!phmap_find(&a->members, &e->parents[i]) &&
				held_gen(a->r, &e->parents[i]) == 0)
				return VCG_ERR_ORDER;
		cur.nseams = m;
		/* the parents as they were at the cut, which include those kept */
		cur.nparents = e->nparents;
		memcpy(cur.parents, e->parents, sizeof(cur.parents));
		memcpy(cur.parent_gen, e->parent_gen, sizeof(cur.parent_gen));
		*out = cur;
	}
	else
	{
		VcgCommit	c;

		if (!vcg_commit_load(&a->r->ctx, commit, &c) || c.generation != gen)
			return VCG_ERR_FORGED;
		*out = *e;
	}
	a->puts[a->n++] = (CgPut) {gen, *commit, out};
	return 0;
}

int
vcg_relink(VcgRepo *r, uint32_t id, ChunkStore *src)
{
	VcgSegment *s;
	PinSet		pins;
	PHash		bad;
	RelinkAcc	a;
	CommitGraph *sg;
	uint64_t	count;
	int			rc;

	if (id >= r->nsegs)
		return VCG_ERR;
	s = &r->segs[id];
	if (s->state == VCG_SEG_RELINKED)
		return VCG_ERR;
	if (src == NULL)
	{
		if (s->state != VCG_SEG_LOCAL)
			return VCG_ERR_SEVERED;
		src = r->ctx.cs;
	}

	/* the segment's graph must be the one cut: it must hash to H */
	pinset_init(&pins, r->ctx.cs);
	rc = cs_copy_closure(src, &pins, &s->hash, true, &bad);
	if (rc != CS_OK)
	{
		pinset_free(&pins);
		return rc == CS_ERR_MISSING ? VCG_ERR_SEVERED : VCG_ERR_FORGED;
	}
	sg = cgraph_prolly_open(&r->ctx, &s->hash, false);
	memset(&a, 0, sizeof(a));
	a.r = r;
	a.H = &s->hash;
	phmap_init(&a.members);
	rc = sg->ops->count(sg, &count);
	if (rc == PROLLY_OK)
		rc = sg->ops->foreach(sg, relink_members_fn, &a);

	/* and the commits it names must be the ones cut */
	for (size_t i = 0; i < a.members.cap && rc == PROLLY_OK; i++)
		if (a.members.used[i])
		{
			rc = cs_copy_closure(src, &pins, &a.members.keys[i], true, &bad);
			if (rc != CS_OK)
				rc = rc == CS_ERR_MISSING ? VCG_ERR_SEVERED : VCG_ERR_FORGED;
		}
	if (rc == PROLLY_OK)
	{
		a.ecap = count;
		a.edges = px_malloc((count ? count : 1) * sizeof(CgEdge));
		rc = sg->ops->foreach(sg, relink_edge_fn, &a);
	}
	if (rc == PROLLY_OK)
	{
		vcg_must(&r->ctx, r->graph->ops->apply(r->graph, a.puts, a.n));
		for (size_t i = 0; i < a.members.cap; i++)
			if (a.members.used[i])
				hold(r, &a.members.keys[i], a.members.vals[i]);
		if (s->state == VCG_SEG_LOCAL)
		{
			/* the segment's holds pass to the window */
			for (size_t i = 0; i < a.members.cap; i++)
				if (a.members.used[i])
					cs_unref_root(r->ctx.cs, &a.members.keys[i]);
			cs_unref_root(r->ctx.cs, &s->hash);
		}
		s->state = VCG_SEG_RELINKED;
		s->archive = NULL;
	}
	sg->ops->destroy(sg);
	phmap_free(&a.members);
	free(a.puts);
	free(a.edges);
	pinset_free(&pins);
	prolly_release_pins(&r->ctx);
	return rc;
}

/* -------------------------------------------------------------- remotes */

typedef struct Want
{
	PHash		h;
	uint64_t	gen;			/* the generation the child's edge names */
} Want;

/*
 * Whether dst already knows h: held, or in a segment it cut.  A cut commit
 * is the same history whatever happened to it here, so the walk stops.
 */
static bool
knows(VcgRepo *dst, const PHash *h, uint64_t gen, CgEdge *edge, bool *has_edge)
{
	*has_edge = false;
	if (held_gen(dst, h))
	{
		*has_edge = dst->graph->ops->get(dst->graph, gen, h, edge) == PROLLY_OK;
		return true;
	}
	for (size_t i = 0; i < dst->nsegs; i++)
	{
		VcgSegment *s = &dst->segs[i];
		CommitGraph *sg;
		CgEdge		e;
		bool		found;

		if (s->state != VCG_SEG_LOCAL)
			continue;
		sg = cgraph_prolly_open(&dst->ctx, &s->hash, false);
		found = sg->ops->get(sg, gen, h, &e) == PROLLY_OK && !(e.flags & CG_SEAM_CHILD);
		sg->ops->destroy(sg);
		if (found)
			return true;
	}
	return false;
}

/* Two edges for one commit agree on every parent both still name. */
static bool
edges_agree(const CgEdge *a, const CgEdge *b)
{
	if (a->nseams == 0 && b->nseams == 0)
		return a->nparents == b->nparents &&
			memcmp(a->parents, b->parents, a->nparents * sizeof(PHash)) == 0 &&
			memcmp(a->parent_gen, b->parent_gen, a->nparents * sizeof(uint64_t)) == 0;
	for (uint32_t i = 0; i < a->nparents; i++)
	{
		bool		found = false;

		for (uint32_t j = 0; j < b->nparents; j++)
			if (phash_eq(&a->parents[i], &b->parents[j]))
				found = a->parent_gen[i] == b->parent_gen[j];
		if (!found && b->nseams == 0)
			return false;
	}
	return true;
}

int
vcg_fetch(VcgRepo *dst, VcgRepo *src, const char *branch, const char *into,
		  PHash *head)
{
	VcgBranch  *sb = vcg_branch_find(src, branch),
			   *db;
	PHash		G,
				bad;
	PinSet		pins;
	CommitGraph *view;
	Want	   *stack = NULL;
	CgPut	   *puts = NULL;
	CgEdge	   *edges = NULL;
	size_t		n = 0,
				cap = 0,
				np = 0,
				pcap = 0,
				ecap = 0;
	PHashMap	seen;
	bool		attached = false,
				reached_root = false,
				reached_cut = false,
				empty = dst->nbr == 0;
	uint64_t	head_gen;
	int			rc = VCG_OK;

	if (sb == NULL)
		return VCG_ERR;
	*head = sb->head;

	/* the ref: a head and the graph root that vouches for its edges */
	G = src->graph->ops->root(src->graph);
	pinset_init(&pins, dst->ctx.cs);
	if ((rc = cs_copy_closure(src->ctx.cs, &pins, &G, true, &bad)) != CS_OK)
	{
		pinset_free(&pins);
		return VCG_ERR_FORGED;
	}
	view = cgraph_prolly_open(&dst->ctx, &G, false);
	if (!commit_gen(src, &sb->head, &head_gen))
		rc = VCG_ERR;
	phmap_init(&seen);
	px_grow((void **) &stack, &cap, 1, sizeof(Want));
	stack[n++] = (Want) {sb->head, head_gen};
	while (n > 0 && rc == VCG_OK)
	{
		Want		w = stack[--n];
		bool		created,
					has_mine;
		CgEdge		theirs,
					mine;
		VcgCommit	c;
		int			grc;

		phmap_ref(&seen, &w.h, &created);
		if (!created)
			continue;
		grc = view->ops->get(view, w.gen, &w.h, &theirs);
		if (knows(dst, &w.h, w.gen, &mine, &has_mine))
		{
			/* a commit both know: the edges they hold must agree */
			if (grc == PROLLY_OK && has_mine && !edges_agree(&mine, &theirs))
				rc = VCG_ERR_CONFLICT;
			attached = true;
			continue;
		}
		if (grc != PROLLY_OK)
		{
			/* the sender's graph does not vouch for a commit it led to */
			rc = grc == PROLLY_NOT_FOUND ? VCG_ERR_FORGED : grc;
			break;
		}
		if (cs_copy_closure(src->ctx.cs, &pins, &w.h, true, &bad) != CS_OK)
		{
			rc = VCG_ERR_FORGED;
			break;
		}
		if (!vcg_commit_load(&dst->ctx, &w.h, &c) || c.generation != w.gen)
		{
			rc = VCG_ERR_FORGED;
			break;
		}
		{
			uint64_t	maxgen = 0;

			for (uint32_t i = 0; i < theirs.nparents; i++)
			{
				if (theirs.parent_gen[i] >= w.gen)
					rc = VCG_ERR_FORGED;
				if (theirs.parent_gen[i] > maxgen)
					maxgen = theirs.parent_gen[i];
			}
			if (theirs.nseams == 0 && w.gen != maxgen + 1)
				rc = VCG_ERR_FORGED;
			if (rc != VCG_OK)
				break;
		}
		if (theirs.nparents == 0)
		{
			if (theirs.nseams)
				reached_cut = true;
			else
				reached_root = true;
		}
		px_grow((void **) &puts, &pcap, np + 1, sizeof(CgPut));
		if (np >= ecap)
		{
			/* puts point into edges: copy them once all are read */
			px_grow((void **) &edges, &ecap, np + 1, sizeof(CgEdge));
		}
		edges[np] = theirs;
		puts[np++] = (CgPut) {w.gen, w.h, NULL};
		for (uint32_t i = 0; i < theirs.nparents; i++)
		{
			px_grow((void **) &stack, &cap, n + 1, sizeof(Want));
			stack[n++] = (Want) {theirs.parents[i], theirs.parent_gen[i]};
		}
	}
	if (rc == VCG_OK && !empty && np > 0 && !attached)
		rc = reached_cut && !reached_root ? VCG_ERR_DETACHED : VCG_ERR_UNRELATED;
	if (rc == VCG_OK && np > 0)
	{
		for (size_t i = 0; i < np; i++)
			puts[i].edge = &edges[i];
		vcg_must(&dst->ctx, dst->graph->ops->apply(dst->graph, puts, np));
		for (size_t i = 0; i < np; i++)
		{
			hold(dst, &puts[i].commit, puts[i].gen);
		}
	}
	if (rc == VCG_OK)
	{
		if ((db = vcg_branch_find(dst, into)) != NULL)
			db->head = sb->head;
		else
			add_branch(dst, into, &sb->head);
	}
	view->ops->destroy(view);
	phmap_free(&seen);
	free(stack);
	free(puts);
	free(edges);
	pinset_free(&pins);
	prolly_release_pins(&dst->ctx);
	return rc;
}

int
vcg_push(VcgRepo *local, VcgRepo *remote, const char *branch)
{
	VcgBranch  *lb = vcg_branch_find(local, branch),
			   *rb = vcg_branch_find(remote, branch);
	PHash		base,
				head;

	if (lb == NULL)
		return VCG_ERR;
	if (rb && !phash_eq(&rb->head, &lb->head) &&
		(vcg_merge_base(local, &rb->head, &lb->head, &base) != 1 ||
		 !phash_eq(&base, &rb->head)))
		return VCG_ERR_NONFF;
	return vcg_fetch(remote, local, branch, branch, &head);
}

/* ------------------------------------------------------- saving and opening */

static void
put_hash(Buf *b, const PHash *h)
{
	buf_append(b, h->b, PHASH_LEN);
}

PHash
vcg_repo_save(VcgRepo *r)
{
	Buf			b = BUF_INIT;
	PHash		h,
				g = r->graph->ops->root(r->graph);

	buf_append_byte(&b, CHUNK_TAG_GREPO);
	buf_append_byte(&b, CHUNK_FORMAT_VERSION);
	buf_append_uvarint(&b, r->clock);
	buf_append_uvarint(&b, r->cur);
	put_hash(&b, &g);
	buf_append_uvarint(&b, r->nbr);
	for (size_t i = 0; i < r->nbr; i++)
	{
		buf_append_uvarint(&b, strlen(r->br[i].name));
		buf_append(&b, r->br[i].name, strlen(r->br[i].name));
		put_hash(&b, &r->br[i].head);
	}
	buf_append_uvarint(&b, r->held.n);
	for (size_t i = 0; i < r->held.cap; i++)
		if (r->held.used[i])
		{
			put_hash(&b, &r->held.keys[i]);
			buf_append_uvarint(&b, r->held.vals[i]);
		}
	/* only segments the repo holds or must find again: the rest are in edges */
	{
		size_t		n = 0;

		for (size_t i = 0; i < r->nsegs; i++)
			n += r->segs[i].state == VCG_SEG_LOCAL || r->segs[i].state == VCG_SEG_EXPORTED;
		buf_append_uvarint(&b, n);
	}
	for (size_t i = 0; i < r->nsegs; i++)
		if (r->segs[i].state == VCG_SEG_LOCAL || r->segs[i].state == VCG_SEG_EXPORTED)
		{
			buf_append_byte(&b, (uint8_t) r->segs[i].state);
			put_hash(&b, &r->segs[i].hash);
		}
	vcg_must(&r->ctx, prolly_put_chunk(&r->ctx, b.data, b.len, &h));
	buf_free(&b);
	if (!r->has_state || !phash_eq(&r->state, &h))
	{
		cs_ref_root(r->ctx.cs, &h);
		if (r->has_state)
			cs_unref_root(r->ctx.cs, &r->state);
	}
	r->state = h;
	r->has_state = true;
	prolly_release_pins(&r->ctx);
	return h;
}

static bool
rd_hash(const uint8_t **p, const uint8_t *end, PHash *h)
{
	if (end - *p < PHASH_LEN)
		return false;
	memcpy(h->b, *p, PHASH_LEN);
	*p += PHASH_LEN;
	return true;
}

int
vcg_repo_open(VcgRepo *r, ChunkStore *cs, const ChunkPolicy *policy, const PHash *state)
{
	size_t		len;
	const uint8_t *d = cs_get(cs, state, &len),
			   *p,
			   *end;
	uint64_t	n = 0,
				v = 0;
	PHash		g;
	bool		ok;

	memset(r, 0, sizeof(*r));
	prolly_ctx_init(&r->ctx, cs, policy);
	phmap_init(&r->held);
	if (d == NULL || len < 2 || d[0] != CHUNK_TAG_GREPO || d[1] != CHUNK_FORMAT_VERSION)
		return VCG_ERR;
	p = d + 2;
	end = d + len;
	ok = read_uvarint(&p, end, &r->clock) && read_uvarint(&p, end, &v) && rd_hash(&p, end, &g);
	r->cur = (size_t) v;
	ok = ok && read_uvarint(&p, end, &n);
	for (uint64_t i = 0; i < n && ok; i++)
	{
		uint64_t	nl;
		char	   *name;
		PHash		h;

		ok = read_uvarint(&p, end, &nl) && nl <= (uint64_t) (end - p);
		if (!ok)
			break;
		name = px_malloc(nl + 1);
		memcpy(name, p, nl);
		name[nl] = '\0';
		p += nl;
		ok = rd_hash(&p, end, &h);
		add_branch(r, name, &h);
		free(name);
	}
	ok = ok && read_uvarint(&p, end, &n);
	for (uint64_t i = 0; i < n && ok; i++)
	{
		PHash		h;
		bool		created;

		ok = rd_hash(&p, end, &h) && read_uvarint(&p, end, &v);
		if (ok)
			*phmap_ref(&r->held, &h, &created) = v;
	}
	ok = ok && read_uvarint(&p, end, &n);
	for (uint64_t i = 0; i < n && ok; i++)
	{
		PHash		h;
		uint8_t		st;

		ok = p < end;
		if (!ok)
			break;
		st = *p++;
		ok = rd_hash(&p, end, &h);
		if (ok)
			add_segment(r, (VcgSegState) st, &h);
	}
	if (!ok || p != end || r->cur >= r->nbr)
		return VCG_ERR;
	/* the store's roots already hold the graph and the state */
	r->graph = cgraph_prolly_open(&r->ctx, &g, true);
	cs_unref_root(cs, &g);
	r->state = *state;
	r->has_state = true;
	return VCG_OK;
}
