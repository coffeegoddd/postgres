/*
 * image.h
 *	  Commit images: the pages of a schema's working set as of a commit.
 *
 * A commit image is physical and local.  It holds every page of the
 * schema's relations (heap, indexes, TOAST, free space and visibility maps)
 * and each relation's catalog description, as they stood at a commit, so
 * the working set can be restored or diffed without going through rows.
 * It is valid only on a matching build of the same cluster (ImgFormat),
 * and only while its commit is in the window; the prolly tree stays the
 * portable truth.
 *
 * Layout: each page is a chunk, so identical pages are stored once.  The
 * page map is a prolly map from (relation, fork, block) to page hash that
 * holds its pages, so consecutive images share unchanged pages and two
 * images diff in O(changed pages).  The relation map is a prolly map from
 * relation to its description.  The image chunk names both roots, the
 * format, the commit, the image it was built on, and the MVCC snapshot
 * that decides which tuples on its pages belong to the commit.
 */
#ifndef IMAGE_H
#define IMAGE_H

#include "prolly.h"

#define IMG_NFORKS 4			/* main, free space map, visibility map, init */

#define IMG_ERR_INVALID (-20)	/* would not describe a whole set of relations */
#define IMG_ERR_FORMAT (-21)	/* built for another server or cluster */

typedef enum ImgRelKind
{
	IMG_REL_TABLE = 'r',
	IMG_REL_INDEX = 'i',
	IMG_REL_TOAST = 't',
	IMG_REL_SEQUENCE = 'S'
} ImgRelKind;

/* Which server an image's pages are laid out for. */
typedef struct ImgFormat
{
	uint32_t	pg_major;
	uint32_t	page_size;
	uint32_t	catversion;
	uint64_t	system_id;		/* the cluster */
} ImgFormat;

/* The MVCC snapshot taken at the commit. */
typedef struct ImgSnapshot
{
	uint64_t	xmin;
	uint64_t	xmax;
	uint64_t   *xip;			/* transactions in progress */
	size_t		nxip;
} ImgSnapshot;

typedef struct ImgRelation
{
	uint32_t	relid;
	ImgRelKind	kind;
	uint32_t	parent;			/* the table an index or TOAST table serves */
	const char *name;
	size_t		namelen;
	uint32_t	nblocks[IMG_NFORKS];
	const uint8_t *catalog;		/* catalog rows, opaque here */
	size_t		catlen;
} ImgRelation;

typedef struct ImgHeader
{
	PHash		hash;
	ImgFormat	format;
	PHash		commit;
	bool		has_base;
	PHash		base;
	uint64_t	lsn;
	ImgSnapshot snapshot;
	PHash		pages;			/* page map root */
	PHash		rels;			/* relation map root */
} ImgHeader;

typedef struct ImgBuilder ImgBuilder;

/*
 * Builds an image, from nothing or on top of a base image: only the
 * changes are given.  A relation whose block count shrinks drops its
 * trailing pages; one that grows must be given every new page; a dropped
 * relation takes its pages with it.  img_finish checks all of that in
 * O(changes) and refuses an image with holes or stray pages.
 */
extern int	img_begin(ProllyCtx *ctx, const ImgFormat *format, const PHash *base,
					  ImgBuilder **out);
extern int	img_put_relation(ImgBuilder *b, const ImgRelation *rel);
extern int	img_drop_relation(ImgBuilder *b, uint32_t relid);
extern int	img_put_page(ImgBuilder *b, uint32_t relid, int fork, uint32_t block,
						 const uint8_t *page);
extern int	img_finish(ImgBuilder *b, const PHash *commit, uint64_t lsn,
					   const ImgSnapshot *snapshot, PHash *out);
extern void img_abort(ImgBuilder *b);

/* Reading. */
extern int	img_load(ProllyCtx *ctx, const PHash *image, ImgHeader *out);
extern void img_header_free(ImgHeader *h);
extern bool img_compatible(const ImgHeader *h, const ImgFormat *format);
extern int	img_relation(ProllyCtx *ctx, const ImgHeader *h, uint32_t relid,
						 ImgRelation *out);
extern int	img_page(ProllyCtx *ctx, const ImgHeader *h, uint32_t relid, int fork,
					 uint32_t block, const uint8_t **page);

typedef void (*ImgRelFn) (void *arg, const ImgRelation *rel);
extern int	img_relations(ProllyCtx *ctx, const ImgHeader *h, ImgRelFn fn, void *arg);

/* A page that differs between two images; a NULL hash means absent. */
typedef void (*ImgPageDiffFn) (void *arg, uint32_t relid, int fork, uint32_t block,
							   const PHash *from, const PHash *to);
extern int	img_diff(ProllyCtx *ctx, const ImgHeader *from, const ImgHeader *to,
					 ImgPageDiffFn fn, void *arg);

/* Full check that every relation has exactly its blocks.  O(pages); for tests. */
extern int	img_check(ProllyCtx *ctx, const ImgHeader *h);

#endif							/* IMAGE_H */
