/*-------------------------------------------------------------------------
 *
 * remote_libpq_load.c
 *	  Remote implementations "postgresql" and "postgres": another database
 *	  over a libpq connection, its URL a libpq one.
 *
 * Core doesn't link libpq, as it doesn't for replication: these forward
 * each call to the implementation the loadable module prolly_remote_libpq
 * sets (prolly_remote_libpq_routine), loading it at the first.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/remote_libpq_load.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/prolly_builtins.h"
#include "access/prolly_remote.h"
#include "fmgr.h"

const ProllyRemoteRoutine *prolly_remote_libpq_routine = NULL;

static const ProllyRemoteRoutine *
libpq(void)
{
	if (prolly_remote_libpq_routine == NULL)
	{
		load_file("prolly_remote_libpq", false);
		if (prolly_remote_libpq_routine == NULL)
			elog(ERROR, "prolly_remote_libpq didn't initialize correctly");
	}
	return prolly_remote_libpq_routine;
}

static void
libpq_check_url(const char *url)
{
	PROLLY_CALL(libpq(), check_url, url);
}

static ProllyRemote *
libpq_open(const char *url, bool create)
{
	return PROLLY_CALL(libpq(), open, url, create);
}

static void
libpq_close(ProllyRemote *remote)
{
	PROLLY_CALL(libpq(), close, remote);
}

static void
libpq_format(ProllyRemote *remote, StringInfo out)
{
	PROLLY_CALL(libpq(), format, remote, out);
}

static void
libpq_heads(ProllyRemote *remote, ProllyRemoteHeadVisit visit, void *arg)
{
	PROLLY_CALL(libpq(), heads, remote, visit, arg);
}

static bool
libpq_get(ProllyRemote *remote, const ProllyHash *hash, StringInfo out)
{
	return PROLLY_CALL(libpq(), get, remote, hash, out);
}

static bool
libpq_has(ProllyRemote *remote, const ProllyHash *hash)
{
	return PROLLY_CALL(libpq(), has, remote, hash);
}

static void
libpq_add_chunks(ProllyRemote *remote, ProllyChunkNext next, void *arg)
{
	PROLLY_CALL(libpq(), add_chunks, remote, next, arg);
}

static bool
libpq_move_head(ProllyRemote *remote, const char *branch,
				const ProllyHash *expected, const ProllyHash *head,
				const char *format, bool force)
{
	return PROLLY_CALL(libpq(), move_head, remote, branch, expected, head,
					   format, force);
}

static void
libpq_get_many(ProllyRemote *remote, const ProllyHash *hashes, int n,
			   ProllyRemoteChunkVisit visit, void *arg)
{
	PROLLY_CALL(libpq(), get_many, remote, hashes, n, visit, arg);
}

static void
libpq_has_many(ProllyRemote *remote, const ProllyHash *hashes, int n, bool *has)
{
	PROLLY_CALL(libpq(), has_many, remote, hashes, n, has);
}

#define LIBPQ_CALLBACKS \
	.check_url = libpq_check_url, \
	.open = libpq_open, \
	.close = libpq_close, \
	.format = libpq_format, \
	.heads = libpq_heads, \
	.get = libpq_get, \
	.has = libpq_has, \
	.add_chunks = libpq_add_chunks, \
	.move_head = libpq_move_head, \
	.get_many = libpq_get_many, \
	.has_many = libpq_has_many

const ProllyRemoteRoutine prolly_remote_postgresql = {
	.info = {
		.iface = PROLLY_IFACE_REMOTE,
		.impl_id = 2,
		.format_version = 1,
		.name = "postgresql",
		.description = "another database, over a libpq connection",
	},
	LIBPQ_CALLBACKS
};

const ProllyRemoteRoutine prolly_remote_postgres = {
	.info = {
		.iface = PROLLY_IFACE_REMOTE,
		.impl_id = 3,
		.format_version = 1,
		.name = "postgres",
		.description = "another database, over a libpq connection (postgres://)",
	},
	LIBPQ_CALLBACKS
};
