/*
 * pgoutput.h
 *	  A logical replication stream from the pgoutput plugin, decoded into
 *	  callbacks.
 *
 * Values arrive as text (each type's output function, under the
 * replication connection's settings).  Pointers handed to a callback are
 * valid only during it.
 */
#ifndef PGOUTPUT_H
#define PGOUTPUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <libpq-fe.h>

#define PGO_NULL 'n'
#define PGO_UNCHANGED 'u'		/* an unchanged TOASTed value, not sent */
#define PGO_TEXT 't'

typedef struct PgoTuple
{
	int			ncols;
	const char *kind;			/* PGO_NULL, PGO_UNCHANGED or PGO_TEXT per column */
	const char *const *val;
	const int  *len;
} PgoTuple;

typedef struct PgoCallbacks
{
	/* op is 'I', 'U' or 'D'; old is NULL unless sent ('K' key or 'O' row). */
	void		(*change) (void *arg, char op, uint32_t relid, char oldkind,
						   const PgoTuple *old, const PgoTuple *new_);
	void		(*truncate) (void *arg, int n, const uint32_t *relids);
	void		(*commit) (void *arg, uint64_t lsn);
	void		(*message) (void *arg, bool transactional, const char *prefix,
							const char *data, size_t len, uint64_t lsn);
} PgoCallbacks;

typedef struct PgoStream PgoStream;

/*
 * Starts streaming from a slot made with the pgoutput plugin.  conninfo is
 * an ordinary connection string; replication=database is added.
 */
extern PgoStream *pgo_start(const char *conninfo, const char *slot,
							const char *publication);

/*
 * Reads and dispatches messages until a callback sets *stop.  With
 * PGO_TRACE in the environment, waits over half a second are reported.
 */
extern void pgo_run(PgoStream *s, const PgoCallbacks *cb, void *arg, bool *stop);

/* Tells the server everything through lsn is consumed. */
extern void pgo_feedback(PgoStream *s, uint64_t lsn);
extern void pgo_close(PgoStream *s);

#endif
