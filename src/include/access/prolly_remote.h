/*-------------------------------------------------------------------------
 *
 * prolly_remote.h
 *	  Remote interface: another store of a database's history, which
 *	  fetch, push and clone transfer commits from and to.
 *
 * A remote holds chunks, by hash, and heads: a branch name and the commit
 * it is at.  It has no working sets.  Each implementation serves the URLs
 * of one scheme (its name: "file" serves file:///path).
 *
 * Contract:
 * - check_url() refuses a URL of the scheme that names no remote it could
 *   open (prolly_remote_for_url() calls it).
 * - open() opens the remote at url, failing if there is none there; with
 *   create, an empty one is made where there is nothing (a push's first).
 *   close() releases it.
 * - format() is the canonical format its commits were made in (as a
 *   folded root's "!:format" item says it): empty if it has none yet.
 * - heads() visits each branch and its commit, in name order.
 * - Chunks move as stored: framed (prolly_chunk_frame()'s), compressed
 *   where they were made, decoded only to be checked.
 * - get() appends a chunk to out as stored (false if it has none by that
 *   hash): the caller checks it against the hash.  has() answers without
 *   reading.
 * - add_chunks() adds the chunks next() gives as stored (until it returns
 *   false), all at once: none is found before every one is durable.  A remote
 *   that has a chunk has every chunk it references, so the caller sends
 *   what the remote lacks of a closed set.
 * - get_many() visits each chunk of those it has, and has_many() answers
 *   for each, in one exchange with a remote across a network (optional:
 *   get() and has() each otherwise).
 * - get_since() visits, in one exchange, what a store that has the known
 *   commits lacks of the wanted ones, as the remote can tell (optional, for
 *   a remote that runs this code): a guess the caller checks, and
 *   completes, as it would without it.
 * - files() visits the remote's files, which a clone copies whole: each
 *   by the key it names it by, with its chunk count and size, and whether
 *   it is a journal (a file made as it is read, which grows); false, having
 *   visited none, if the remote can't list them (optional: a clone walks
 *   otherwise).  file_open() starts reading a file's chunks, as stored, in
 *   its order; file_next() gives the next (false at its end);
 *   file_close() ends it.
 * - move_head() moves branch from expected (an empty hash: the branch
 *   doesn't exist) to head (NULL: delete it), durably, as a compare-and-
 *   swap: false, changing nothing, if the branch isn't at expected.  The
 *   remote must have head's chunks.  A format is recorded with the first
 *   head.  A remote that is a database refuses to move a branch with
 *   uncommitted changes there, unless force.
 * - tags() visits each tag, in name order: its name, its tag object and
 *   the commit the remote says that tag is of (the caller reads the tag
 *   object for itself once received).  move_tag() moves a tag from
 *   expected (an empty hash: there is none) to tag, a tag object of commit
 *   (NULL: delete it), as move_head() moves a branch; the remote must have
 *   tag's chunks.
 *   Optional, together: a remote without them has no tags.
 *
 * Provisional (v0).
 *
 * src/include/access/prolly_remote.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_REMOTE_H
#define PROLLY_REMOTE_H

#include "access/prolly_chunkstore.h"
#include "access/prolly_registry.h"
#include "access/prolly_types.h"
#include "lib/stringinfo.h"

typedef struct ProllyRemote ProllyRemote;

/* Visits a chunk a remote gave: its hash and the chunk as stored (framed). */
typedef void (*ProllyRemoteChunkVisit) (const ProllyHash *hash,
										const char *data, int len, void *arg);

/* Visits a remote's file: its key, chunk count and size; a journal? */
typedef void (*ProllyRemoteFileVisit) (const char *key, uint32 nchunks,
									   uint64 size, bool journal, void *arg);

typedef struct ProllyRemoteFile ProllyRemoteFile;

/* Visits a remote's branch and the commit it is at. */
typedef void (*ProllyRemoteHeadVisit) (const char *branch,
									   const ProllyHash *commit, void *arg);

/* Visits a remote's tag: its name, its tag object and that tag's commit. */
typedef void (*ProllyRemoteTagVisit) (const char *name, const ProllyHash *tag,
									  const ProllyHash *commit, void *arg);

typedef struct ProllyRemoteRoutine
{
	ProllyImplInfo info;

	void		(*check_url) (const char *url);
	ProllyRemote *(*open) (const char *url, bool create);
	void		(*close) (ProllyRemote *remote);

	void		(*format) (ProllyRemote *remote, StringInfo out);
	void		(*heads) (ProllyRemote *remote, ProllyRemoteHeadVisit visit,
						  void *arg);

	bool		(*get) (ProllyRemote *remote, const ProllyHash *hash,
						StringInfo out);
	bool		(*has) (ProllyRemote *remote, const ProllyHash *hash);

	void		(*add_chunks) (ProllyRemote *remote, ProllyChunkNext next,
							   void *arg);
	bool		(*move_head) (ProllyRemote *remote, const char *branch,
							  const ProllyHash *expected,
							  const ProllyHash *head, const char *format,
							  bool force);

	void		(*get_many) (ProllyRemote *remote, const ProllyHash *hashes,
							 int n, ProllyRemoteChunkVisit visit, void *arg);
	void		(*has_many) (ProllyRemote *remote, const ProllyHash *hashes,
							 int n, bool *has);
	void		(*get_since) (ProllyRemote *remote, const ProllyHash *wanted,
							  int nwanted, const ProllyHash *known, int nknown,
							  ProllyRemoteChunkVisit visit, void *arg);

	/* Whole files (optional). */
	bool		(*files) (ProllyRemote *remote, ProllyRemoteFileVisit visit,
						  void *arg);
	ProllyRemoteFile *(*file_open) (ProllyRemote *remote, const char *key);
	bool		(*file_next) (ProllyRemoteFile *file, ProllyHash *hash,
							  StringInfo frame);
	void		(*file_close) (ProllyRemoteFile *file);

	/* Tags (optional, together). */
	void		(*tags) (ProllyRemote *remote, ProllyRemoteTagVisit visit,
						 void *arg);
	bool		(*move_tag) (ProllyRemote *remote, const char *name,
							 const ProllyHash *expected, const ProllyHash *tag,
							 const ProllyHash *commit);
} ProllyRemoteRoutine;

#define PROLLY_REMOTE_CALLBACK_NAMES \
	"check_url", "open", "close", "format", "heads", "get", "has", \
	"add_chunks", "move_head", "get_many", "has_many", "get_since", \
	"files", "file_open", "file_next", "file_close", "tags", "move_tag"
#define PROLLY_REMOTE_FIRST_CALLBACK check_url
#define PROLLY_REMOTE_LAST_CALLBACK move_tag

/*
 * The implementation of remotes over a libpq connection, which the loadable
 * module prolly_remote_libpq sets when it is loaded (core doesn't link
 * libpq); the "postgresql" and "postgres" schemes forward to it.
 */
extern PGDLLIMPORT const ProllyRemoteRoutine *prolly_remote_libpq_routine;

/* The implementation serving url's scheme; an error if none does. */
extern const ProllyRemoteRoutine *prolly_remote_for_url(const char *url);

#endif							/* PROLLY_REMOTE_H */
