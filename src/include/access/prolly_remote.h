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
 * - get() appends a chunk's bytes to out (false if it has none by that
 *   hash), as stored: the caller checks them against the hash.  has()
 *   answers without reading.
 * - add_chunks() adds the chunks next() gives (until it returns false),
 *   all at once: none is found before every one is durable.  A remote
 *   that has a chunk has every chunk it references, so the caller sends
 *   what the remote lacks of a closed set.
 * - move_head() moves branch from expected (an empty hash: the branch
 *   doesn't exist) to head (NULL: delete it), durably, as a compare-and-
 *   swap: false, changing nothing, if the branch isn't at expected.  The
 *   remote must have head's chunks.  A format is recorded with the first
 *   head.
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

/* Visits a remote's branch and the commit it is at. */
typedef void (*ProllyRemoteHeadVisit) (const char *branch,
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
							  const ProllyHash *head, const char *format);
} ProllyRemoteRoutine;

#define PROLLY_REMOTE_CALLBACK_NAMES \
	"check_url", "open", "close", "format", "heads", "get", "has", \
	"add_chunks", "move_head"
#define PROLLY_REMOTE_FIRST_CALLBACK check_url
#define PROLLY_REMOTE_LAST_CALLBACK move_head

/* The implementation serving url's scheme; an error if none does. */
extern const ProllyRemoteRoutine *prolly_remote_for_url(const char *url);

#endif							/* PROLLY_REMOTE_H */
