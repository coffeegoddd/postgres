/*
 * cgraph.h
 *	  A commit graph: the parent edges of commits, kept apart from the
 *	  commits themselves.
 *
 * A commit's hash covers its root value and metadata but not its parents,
 * so edges can be cut or restored without changing any commit's identity.
 * What a hash chain of parents would prove, the graph proves instead: it is
 * a map whose root hash covers every edge, so a holder of that hash can
 * check any edge read through it.
 *
 * An edge is keyed by (generation, commit), so the edges of new commits
 * sort together at the high end and the edges a sever cuts sort together at
 * the low end: both touch a few leaves, not one leaf per edge.
 *
 * An edge lists the commit's parents still in the graph and, for parents
 * cut away, the hash of each segment they went into.  A commit with no
 * parents and no segments is a root of history; one with no parents but a
 * segment is a root here whose history continues in that segment.
 *
 * The same structure holds a severed segment's edges, so a segment is
 * named by its graph's root hash.
 */
#ifndef CGRAPH_H
#define CGRAPH_H

#include "prolly.h"

#define CG_MAX_PARENTS 8
#define CG_MAX_SEAMS 8

/* Set on a segment's edge for a commit that stayed in the graph it was cut from. */
#define CG_SEAM_CHILD 0x1

typedef struct CgEdge
{
	uint32_t	flags;
	uint32_t	nparents;
	PHash		parents[CG_MAX_PARENTS];
	uint64_t	parent_gen[CG_MAX_PARENTS];
	uint32_t	nseams;
	PHash		seams[CG_MAX_SEAMS];	/* segments holding cut parents */
} CgEdge;

/* One edit: an edge set (edge != NULL) or removed. */
typedef struct CgPut
{
	uint64_t	gen;
	PHash		commit;
	const CgEdge *edge;
} CgPut;

typedef struct CommitGraph CommitGraph;

/* Called for each edge in key order; a nonzero return stops the walk and is returned. */
typedef int (*CgEdgeFn) (void *arg, uint64_t gen, const PHash *commit,
						 const CgEdge *e);

typedef struct CommitGraphOps
{
	/* VC_OK-style 0 with the edge, PROLLY_NOT_FOUND, or a negative error. */
	int			(*get) (CommitGraph *g, uint64_t gen, const PHash *commit, CgEdge *out);
	int			(*apply) (CommitGraph *g, const CgPut *puts, size_t n);
	/* The hash covering every edge. */
	PHash		(*root) (CommitGraph *g);
	int			(*count) (CommitGraph *g, uint64_t *n);
	int			(*foreach) (CommitGraph *g, CgEdgeFn fn, void *arg);
	void		(*destroy) (CommitGraph *g);
} CommitGraphOps;

struct CommitGraph
{
	const CommitGraphOps *ops;
};

/*
 * A graph kept in a prolly map in ctx's store.  With held, the graph holds
 * its root in the store (a root ref, moved on every apply); otherwise the
 * caller holds it, as for a view of another repo's graph or a segment
 * being built.  root may be NULL for an empty graph.
 */
extern CommitGraph *cgraph_prolly_open(ProllyCtx *ctx, const PHash *root, bool held);

/* Edge values, for anything that must compare or copy them. */
extern void cg_edge_encode(const CgEdge *e, Buf *out);
extern bool cg_edge_decode(const uint8_t *p, size_t len, CgEdge *out);
extern bool cg_edge_eq(const CgEdge *a, const CgEdge *b);

#endif							/* CGRAPH_H */
