/*
 * remote.h
 *	  A plain-storage remote: named objects, as a directory (file://) or an
 *	  object server over HTTP (S3's verbs) holds them.  No logic runs there;
 *	  everything a repo needs is built on these calls by the client
 *	  (packstore.h).
 */
#ifndef REMOTE_H
#define REMOTE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define REMOTE_ETAG_LEN 80

typedef struct RemoteStats
{
	uint64_t	requests;
	uint64_t	gets;
	uint64_t	puts;
	uint64_t	lists;
	uint64_t	bytes_up;
	uint64_t	bytes_down;
	double		secs;			/* wall time spent in calls */
} RemoteStats;

typedef struct Remote Remote;

/* One range of an object to read: off, len (0: to the end). */
typedef struct RemoteRange
{
	const char *key;
	uint64_t	off;
	uint64_t	len;
	uint8_t    *data;			/* out: malloc'd, or NULL if absent */
	size_t		got;			/* out */
} RemoteRange;

extern Remote *remote_dir_open(const char *dir);
extern Remote *remote_http_open(const char *base_url, int parallel);
extern void remote_close(Remote *r);
extern RemoteStats *remote_stats(Remote *r);
extern const char *remote_describe(Remote *r);

/* Stores an object; 0, or -1 on error. */
extern int	remote_put(Remote *r, const char *key, const uint8_t *data, size_t len);

/*
 * Stores an object only if its ETag is 'if_match' (NULL: only if it doesn't
 * exist): 0 done (etag_out set), 1 the condition failed, -1 an error.
 */
extern int	remote_put_if(Remote *r, const char *key, const uint8_t *data,
						  size_t len, const char *if_match, char *etag_out);

/* An object, or a range of it (len 0: to the end); NULL if absent. */
extern uint8_t *remote_get(Remote *r, const char *key, uint64_t off,
						   uint64_t len, size_t *got, char *etag_out);

/* Several ranges at once, up to the remote's parallelism; false on error. */
extern bool remote_get_many(Remote *r, RemoteRange *ranges, size_t n);

/* Keys under a prefix (malloc'd array of malloc'd strings). */
extern char **remote_list(Remote *r, const char *prefix, size_t *n);

#endif							/* REMOTE_H */
