/*
 * vcg.h
 *	  Version control with parents kept in a commit graph (see cgraph.h)
 *	  instead of in the commits.
 *
 * A commit names its root value, its generation, a random nonce, a time
 * and a message.  Its parents are edges in the repo's commit graph.  So
 * severing history rewrites edges, never commits: the oldest commit the
 * window keeps becomes a root of the graph, with the same hash it always
 * had, and no hash anyone recorded stops resolving.
 *
 * What a parent hash chain used to prove, hashes still prove:
 *	- the graph's root hash covers every edge in it, so edges read from
 *	  another repo are checked against the graph root sent with its ref;
 *	- a sever puts what it cuts (the cut commits' edges and the edges from
 *	  the kept commits into them) in a segment graph, and records that
 *	  graph's root hash H on each kept commit it cut from.  Relinking takes
 *	  the segment from any source, and refuses it unless it hashes to H.
 * A generation is part of the commit's hash, so an edge must also agree
 * with the generations of the commits at both ends.
 *
 * Commits are held by the repo while they are in the window's history or a
 * local segment; the graph and segments hold nothing.
 */
#ifndef VCG_H
#define VCG_H

#include "cgraph.h"

#define VCG_OK 0
#define VCG_ERR (-10)			/* bad argument or state */
#define VCG_ERR_SEVERED (-3)	/* the answer lies past a cut */
#define VCG_ERR_UNRELATED (-5)	/* the histories share no commit */
#define VCG_ERR_DETACHED (-6)	/* the sender's history is cut before it attaches */
#define VCG_ERR_FORGED (-7)		/* what was sent does not match its hashes */
#define VCG_ERR_CONFLICT (-8)	/* an edge disagrees with one already known */
#define VCG_ERR_ORDER (-9)		/* a newer segment must be relinked first */
#define VCG_ERR_NONFF (-11)		/* a push that is not a fast-forward */

#define VCG_NONCE_LEN 16
#define VCG_NO_SEGMENT UINT32_MAX

typedef struct VcgCommit
{
	PHash		hash;
	PHash		root;
	uint64_t	generation;		/* 1 + the largest parent generation */
	uint8_t		nonce[VCG_NONCE_LEN];
	uint64_t	time;
	const char *msg;			/* not NUL-terminated */
	size_t		msglen;
} VcgCommit;

typedef struct VcgBranch
{
	char	   *name;
	PHash		head;
} VcgBranch;

typedef enum VcgSegState
{
	VCG_SEG_LOCAL,				/* cut, still held here */
	VCG_SEG_EXPORTED,			/* copied to an archive, let go here */
	VCG_SEG_DISCARDED,			/* let go; only its hash remains */
	VCG_SEG_RELINKED			/* its edges and commits are back in the graph */
} VcgSegState;

/*
 * A segment is named by its graph's root, which lists its commits, so the
 * repo keeps no list of them: a discarded segment costs its hash.
 */
typedef struct VcgSegment
{
	VcgSegState state;
	PHash		hash;			/* root of the segment's graph */
	ChunkStore *archive;		/* where an exported segment went */
} VcgSegment;

typedef struct VcgRepo
{
	ProllyCtx	ctx;
	CommitGraph *graph;
	VcgBranch  *br;
	size_t		nbr;
	size_t		cap;
	size_t		cur;
	uint64_t	clock;
	PHashMap	held;			/* commit -> generation, for the window's history */
	VcgSegment *segs;
	size_t		nsegs;
	size_t		segcap;
	bool		has_state;
	PHash		state;
} VcgRepo;

typedef void (*VcgLogFn) (void *arg, const VcgCommit *c);

/* Commits. */
extern bool vcg_commit_load(ProllyCtx *ctx, const PHash *h, VcgCommit *out);
/* With nonce NULL a fresh random one is drawn. */
extern PHash vcg_commit_write(ProllyCtx *ctx, const PHash *root, uint64_t generation,
							  const uint8_t *nonce, uint64_t time, const char *msg);

/* Repositories and branches. */
extern void vcg_repo_init(VcgRepo *r, ChunkStore *cs, const ChunkPolicy *policy);
/* An empty repo, with no branches: only for cloning into. */
extern void vcg_repo_init_empty(VcgRepo *r, ChunkStore *cs, const ChunkPolicy *policy);
extern void vcg_repo_free(VcgRepo *r);
extern VcgBranch *vcg_head(VcgRepo *r);
extern VcgBranch *vcg_branch_find(VcgRepo *r, const char *name);
extern int	vcg_branch_create(VcgRepo *r, const char *name, const PHash *commit);
extern int	vcg_checkout(VcgRepo *r, const char *name);
/* Deletes a branch other than the current one. */
extern int	vcg_branch_delete(VcgRepo *r, const char *name);
/* Commits root (held by the caller until this returns) on the current branch. */
extern PHash vcg_commit(VcgRepo *r, const PHash *root, const char *msg);
/*
 * Merges branch into the current one: a fast-forward, a merge commit, or,
 * with conflicts, nothing (*nconflicts says how many).
 */
extern int	vcg_merge(VcgRepo *r, const char *branch, size_t *nconflicts);
/* A commit's edge in this repo's graph; PROLLY_NOT_FOUND if it has none. */
extern int	vcg_edge(VcgRepo *r, const PHash *commit, CgEdge *out);

/*
 * History.  vcg_log follows first parents to a root, at most max commits.
 * vcg_merge_base returns 1 with the base, 0 if the histories meet nowhere,
 * or VCG_ERR_SEVERED if they meet nowhere here but a walk reached a commit
 * whose history continues in a cut segment.
 */
extern int	vcg_log(VcgRepo *r, const PHash *start, size_t max, VcgLogFn fn, void *arg);
extern int	vcg_merge_base(VcgRepo *r, const PHash *a, const PHash *b, PHash *out);

/*
 * Severing.  vcg_sever cuts every commit held that is not within depth
 * generations of a branch head into a new local segment (or reports
 * VCG_NO_SEGMENT).  A segment can then be exported to an archive store or
 * discarded.  vcg_relink brings a segment back from src (any store; NULL
 * for a local segment), checking it against the hash recorded at the cut;
 * segments are relinked newest first.
 */
extern int	vcg_sever(VcgRepo *r, uint32_t depth, uint32_t *seg);
extern int	vcg_segment_export(VcgRepo *r, uint32_t seg, ChunkStore *archive);
extern int	vcg_segment_discard(VcgRepo *r, uint32_t seg);
extern int	vcg_relink(VcgRepo *r, uint32_t seg, ChunkStore *src);

/*
 * The segment a hash names, from any edge or archive that records it.  A
 * discarded segment is kept in no list: its hash is in the edges of the
 * commits it was cut from (or in the segment that cut those in turn), so
 * the saved repo stays the size of the window however much is cut.  This
 * finds or adds it, so segment ids last only for the session.
 */
extern uint32_t vcg_segment_find(VcgRepo *r, const PHash *hash);

/*
 * Remotes.  vcg_fetch sends dst what it lacks of src's branch, stopping at
 * commits dst already knows, and sets dst's branch into to it.  Edges are
 * read through src's graph root, every chunk is checked against its hash,
 * and an edge must agree with the generations at both ends and with any
 * edge dst already has for the same commit.  The histories must attach
 * (VCG_ERR_UNRELATED or VCG_ERR_DETACHED otherwise) unless dst is empty.
 * vcg_push is a fetch into the remote that must fast-forward.
 */
extern int	vcg_fetch(VcgRepo *dst, VcgRepo *src, const char *branch, const char *into,
					  PHash *head);
extern int	vcg_push(VcgRepo *local, VcgRepo *remote, const char *branch);

/* Saving and reopening, as for vc repos (see vc_repo_save). */
extern PHash vcg_repo_save(VcgRepo *r);
extern int	vcg_repo_open(VcgRepo *r, ChunkStore *cs, const ChunkPolicy *policy,
						  const PHash *state);

#endif							/* VCG_H */
