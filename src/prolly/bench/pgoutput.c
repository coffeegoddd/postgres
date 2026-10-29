/*
 * pgoutput.c
 *	  Decodes pgoutput's protocol (version 1, text values, with logical
 *	  messages) from a START_REPLICATION stream.
 */
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "pgoutput.h"

struct PgoStream
{
	PGconn	   *c;
	uint64_t	received;		/* end of the last WAL data seen */
};

static void
pgo_die(PgoStream *s, const char *what)
{
	fprintf(stderr, "pgoutput: %s: %s", what, s ? PQerrorMessage(s->c) : "");
	exit(1);
}

/* A bounds-checked reader over one message. */
typedef struct Rd
{
	const char *p;
	const char *end;
} Rd;

static void
need(Rd *r, size_t n)
{
	if ((size_t) (r->end - r->p) < n)
	{
		fprintf(stderr, "pgoutput: truncated message\n");
		exit(1);
	}
}

static uint8_t
rd8(Rd *r)
{
	need(r, 1);
	return (uint8_t) *r->p++;
}

static uint64_t
rdn(Rd *r, int n)
{
	uint64_t	v = 0;

	need(r, (size_t) n);
	for (int i = 0; i < n; i++)
		v = (v << 8) | (uint8_t) r->p[i];
	r->p += n;
	return v;
}

static const char *
rdstr(Rd *r)
{
	const char *s = r->p;
	const char *z = memchr(r->p, '\0', (size_t) (r->end - r->p));

	if (z == NULL)
		need(r, (size_t) (r->end - r->p) + 1);
	r->p = z + 1;
	return s;
}

/* Tuple storage reused across messages. */
typedef struct TupBuf
{
	char	   *kind;
	const char **val;
	int		   *len;
	int			cap;
	PgoTuple	t;
} TupBuf;

static void
rdtuple(Rd *r, TupBuf *b)
{
	int			n = (int) rdn(r, 2);

	if (n > b->cap)
	{
		b->cap = n * 2;
		b->kind = realloc(b->kind, (size_t) b->cap);
		b->val = realloc(b->val, (size_t) b->cap * sizeof(char *));
		b->len = realloc(b->len, (size_t) b->cap * sizeof(int));
		if (!b->kind || !b->val || !b->len)
			abort();
	}
	for (int i = 0; i < n; i++)
	{
		char		k = (char) rd8(r);

		b->kind[i] = k;
		b->val[i] = NULL;
		b->len[i] = 0;
		if (k == 't')
		{
			b->len[i] = (int) rdn(r, 4);
			need(r, (size_t) b->len[i]);
			b->val[i] = r->p;
			r->p += b->len[i];
		}
		else if (k != 'n' && k != 'u')
		{
			fprintf(stderr, "pgoutput: column kind %c not handled\n", k);
			exit(1);
		}
	}
	b->t.ncols = n;
	b->t.kind = b->kind;
	b->t.val = (const char *const *) b->val;
	b->t.len = b->len;
}

static TupBuf oldb,
			newb;

static void
dispatch(const char *msg, size_t len, const PgoCallbacks *cb, void *arg)
{
	Rd			r = {msg, msg + len};
	char		tag = (char) rd8(&r);

	switch (tag)
	{
		case 'B':				/* begin */
		case 'R':				/* relation: columns follow the catalog */
		case 'Y':				/* type */
		case 'O':				/* origin */
			break;
		case 'C':
			{
				uint64_t	lsn;

				rd8(&r);
				lsn = rdn(&r, 8);
				if (cb->commit)
					cb->commit(arg, lsn);
				break;
			}
		case 'I':
			{
				uint32_t	relid = (uint32_t) rdn(&r, 4);

				if (rd8(&r) != 'N')
					pgo_die(NULL, "insert without a new tuple\n");
				rdtuple(&r, &newb);
				cb->change(arg, 'I', relid, 0, NULL, &newb.t);
				break;
			}
		case 'U':
			{
				uint32_t	relid = (uint32_t) rdn(&r, 4);
				char		k = (char) rd8(&r),
							oldkind = 0;

				if (k == 'K' || k == 'O')
				{
					oldkind = k;
					rdtuple(&r, &oldb);
					k = (char) rd8(&r);
				}
				if (k != 'N')
					pgo_die(NULL, "update without a new tuple\n");
				rdtuple(&r, &newb);
				cb->change(arg, 'U', relid, oldkind, oldkind ? &oldb.t : NULL, &newb.t);
				break;
			}
		case 'D':
			{
				uint32_t	relid = (uint32_t) rdn(&r, 4);
				char		k = (char) rd8(&r);

				rdtuple(&r, &oldb);
				cb->change(arg, 'D', relid, k, &oldb.t, NULL);
				break;
			}
		case 'T':
			{
				int			n = (int) rdn(&r, 4);
				uint32_t   *ids = malloc((size_t) (n ? n : 1) * sizeof(uint32_t));

				rd8(&r);
				for (int i = 0; i < n; i++)
					ids[i] = (uint32_t) rdn(&r, 4);
				cb->truncate(arg, n, ids);
				free(ids);
				break;
			}
		case 'M':
			{
				bool		tx = rd8(&r) & 1;
				uint64_t	lsn = rdn(&r, 8);
				const char *prefix = rdstr(&r);
				size_t		n = (size_t) rdn(&r, 4);

				need(&r, n);
				cb->message(arg, tx, prefix, r.p, n, lsn);
				break;
			}
		default:
			fprintf(stderr, "pgoutput: message %c not handled\n", tag);
			exit(1);
	}
}

PgoStream *
pgo_start(const char *conninfo, const char *slot, const char *publication)
{
	PgoStream  *s = calloc(1, sizeof(PgoStream));
	char		ci[1024],
				cmd[512];
	PGresult   *res;

	snprintf(ci, sizeof(ci), "%s replication=database", conninfo);
	s->c = PQconnectdb(ci);
	if (PQstatus(s->c) != CONNECTION_OK)
		pgo_die(s, "connect");
	snprintf(cmd, sizeof(cmd),
			 "START_REPLICATION SLOT %s LOGICAL 0/0 (proto_version '1', "
			 "publication_names '%s', messages 'true')", slot, publication);
	res = PQexec(s->c, cmd);
	if (PQresultStatus(res) != PGRES_COPY_BOTH)
		pgo_die(s, "START_REPLICATION");
	PQclear(res);
	return s;
}

static void
put_be64(char *p, uint64_t v)
{
	for (int i = 7; i >= 0; i--, v >>= 8)
		p[i] = (char) v;
}

void
pgo_feedback(PgoStream *s, uint64_t lsn)
{
	char		buf[34];
	struct timespec ts;
	uint64_t	us;

	clock_gettime(CLOCK_REALTIME, &ts);
	us = (uint64_t) ts.tv_sec * 1000000 + (uint64_t) ts.tv_nsec / 1000 -
		UINT64_C(946684800) * 1000000;	/* since 2000-01-01 */
	buf[0] = 'r';
	put_be64(buf + 1, lsn);
	put_be64(buf + 9, lsn);
	put_be64(buf + 17, lsn);
	put_be64(buf + 25, us);
	buf[33] = 0;
	if (PQputCopyData(s->c, buf, sizeof(buf)) != 1 || PQflush(s->c) != 0)
		pgo_die(s, "feedback");
}

static double
mono(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

void
pgo_run(PgoStream *s, const PgoCallbacks *cb, void *arg, bool *stop)
{
	bool		trace = getenv("PGO_TRACE") != NULL;
	double		last = mono();

	while (!*stop)
	{
		char	   *buf;
		int			n = PQgetCopyData(s->c, &buf, 0);

		if (trace && mono() - last > 0.5)
			fprintf(stderr, "pgo: %.2f s waiting, then %c (%d bytes) %c\n", mono() - last,
					n > 0 ? buf[0] : '-', n, n > 25 && buf[0] == 'w' ? buf[25] : ' ');
		last = mono();

		if (n < 0)
			pgo_die(s, "stream ended");
		if (buf[0] == 'w')
		{
			Rd			r = {buf + 1, buf + n};
			uint64_t	end;

			rdn(&r, 8);
			end = rdn(&r, 8);
			rdn(&r, 8);
			if (end > s->received)
				s->received = end;
			dispatch(r.p, (size_t) (r.end - r.p), cb, arg);
		}
		else if (buf[0] == 'k')
		{
			Rd			r = {buf + 1, buf + n};

			rdn(&r, 8);
			rdn(&r, 8);
			if (rd8(&r))
				pgo_feedback(s, s->received);
		}
		PQfreemem(buf);
	}
}

void
pgo_close(PgoStream *s)
{
	PQputCopyEnd(s->c, NULL);
	PQfinish(s->c);
	free(oldb.kind);
	free(oldb.val);
	free(oldb.len);
	free(newb.kind);
	free(newb.val);
	free(newb.len);
	free(s);
}
