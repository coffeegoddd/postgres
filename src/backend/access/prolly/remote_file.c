/*-------------------------------------------------------------------------
 *
 * remote_file.c
 *	  Remote implementation "file": a remote in a directory of the server's
 *	  file system, file:///path.
 *
 * The directory holds
 *
 *		prolly_remote		the manifest: text lines
 *								prolly remote 1
 *								format <the canonical format, or nothing>
 *								last <the last table file id given>
 *								file <id, 16 hex digits> <size in bytes>
 *								head <commit, hex> <branch name>
 *							(files and heads one a line, heads by name)
 *		prolly_remote.lock	locked (flock) by whoever changes the manifest
 *		table_<id>			table files, in the table file format, never
 *							changed once named
 *
 * Every change writes what it adds first (a table file, synced), then a
 * new manifest aside, synced and renamed over the old one: a reader sees
 * the old manifest or the new, and a file a crash left unnamed is never
 * read.  The lock serializes changes; reads take none.
 *
 * A file remote is the server's files: reading one takes the privileges
 * of pg_read_server_files, changing one those of pg_write_server_files,
 * as COPY with a file name does.
 *
 * IDENTIFICATION
 *	  src/backend/access/prolly/remote_file.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef WIN32
#include <sys/file.h>
#endif

#include "access/prolly_builtins.h"
#include "access/prolly_tablefile.h"
#include "catalog/pg_authid.h"
#include "common/file_perm.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "utils/acl.h"
#include "utils/memutils.h"

#define MANIFEST_NAME	"prolly_remote"
#define LOCK_NAME		"prolly_remote.lock"
#define MANIFEST_MAGIC	"prolly remote 1"

/* a table file of the remote's */
typedef struct RemoteTable
{
	uint64		id;
	uint64		size;
	int			fd;				/* -1: not opened yet */
	char	   *path;
	ProllyTableFile *tf;
} RemoteTable;

typedef struct RemoteHead
{
	char	   *branch;
	ProllyHash	commit;
} RemoteHead;

typedef struct Manifest
{
	char	   *format;
	uint64		last;
	List	   *tables;			/* RemoteTable * */
	List	   *heads;			/* RemoteHead *, by name */
} Manifest;

struct ProllyRemote
{
	MemoryContext cxt;
	char	   *dir;
	Manifest	m;
	bool		read_checked;
	bool		write_checked;
};

static const ProllyTableFileRoutine *
table_format(void)
{
	static const ProllyTableFileRoutine *fmt = NULL;

	if (fmt == NULL)
		fmt = prolly_tablefile_lookup("basic");
	return fmt;
}

static void
check_privilege(ProllyRemote *r, bool write)
{
	Oid			role = write ? ROLE_PG_WRITE_SERVER_FILES : ROLE_PG_READ_SERVER_FILES;

	if (write ? r->write_checked : r->read_checked)
		return;
	if (!has_privs_of_role(GetUserId(), role))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to %s file remote \"%s\"",
						write ? "change" : "read", r->dir),
				 errdetail("Only roles with privileges of the \"%s\" role may %s a remote in the server's files.",
						   write ? "pg_write_server_files" : "pg_read_server_files",
						   write ? "change" : "read")));
	if (write)
		r->write_checked = true;
	else
		r->read_checked = true;
}

static char *
remote_path(ProllyRemote *r, const char *name)
{
	return psprintf("%s/%s", r->dir, name);
}

static char *
table_name(uint64 id)
{
	return psprintf("table_%016llx", (unsigned long long) id);
}

static void
hex_of(const ProllyHash *h, StringInfo out)
{
	for (int i = 0; i < h->len; i++)
		appendStringInfo(out, "%02x", h->bytes[i]);
}

static bool
hash_of_hex(const char *s, int len, ProllyHash *h)
{
	if (len % 2 != 0 || len / 2 > PROLLY_HASH_MAX_LEN)
		return false;
	memset(h, 0, sizeof(*h));
	for (int i = 0; i < len / 2; i++)
	{
		int			v = 0;

		for (int j = 0; j < 2; j++)
		{
			char		c = s[2 * i + j];

			v <<= 4;
			if (c >= '0' && c <= '9')
				v |= c - '0';
			else if (c >= 'a' && c <= 'f')
				v |= c - 'a' + 10;
			else
				return false;
		}
		h->bytes[i] = (uint8) v;
	}
	h->len = len / 2;
	return true;
}

static void
corrupt(ProllyRemote *r, const char *what)
{
	ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("file remote \"%s\" has an invalid manifest: %s", r->dir, what)));
}

/* Read the manifest into r->m; false if there is none. */
static bool
read_manifest(ProllyRemote *r)
{
	char	   *path = remote_path(r, MANIFEST_NAME);
	FILE	   *f;
	char		line[MAXPGPATH + 2 * PROLLY_HASH_MAX_LEN + 64];
	int			n = 0;
	MemoryContext old = MemoryContextSwitchTo(r->cxt);

	memset(&r->m, 0, sizeof(r->m));
	r->m.format = pstrdup("");
	f = AllocateFile(path, "r");
	if (f == NULL)
	{
		if (errno != ENOENT)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open file \"%s\": %m", path)));
		MemoryContextSwitchTo(old);
		return false;
	}
	while (fgets(line, sizeof(line), f) != NULL)
	{
		size_t		len = strlen(line);

		if (len == 0 || line[len - 1] != '\n')
			corrupt(r, "a line is too long or unterminated");
		line[--len] = '\0';
		if (n++ == 0)
		{
			if (strcmp(line, MANIFEST_MAGIC) != 0)
				corrupt(r, "it isn't a prolly remote's");
		}
		else if (strncmp(line, "format", 6) == 0 && (line[6] == '\0' || line[6] == ' '))
			r->m.format = pstrdup(line[6] ? line + 7 : "");
		else if (strncmp(line, "last ", 5) == 0)
			r->m.last = strtou64(line + 5, NULL, 10);
		else if (strncmp(line, "file ", 5) == 0)
		{
			RemoteTable *t = palloc0(sizeof(RemoteTable));
			char	   *end;

			t->id = strtou64(line + 5, &end, 16);
			if (*end != ' ')
				corrupt(r, "a file line is malformed");
			t->size = strtou64(end + 1, NULL, 10);
			t->fd = -1;
			r->m.tables = lappend(r->m.tables, t);
		}
		else if (strncmp(line, "head ", 5) == 0)
		{
			RemoteHead *h = palloc0(sizeof(RemoteHead));
			char	   *sp = strchr(line + 5, ' ');

			if (sp == NULL || !hash_of_hex(line + 5, sp - (line + 5), &h->commit))
				corrupt(r, "a head line is malformed");
			h->branch = pstrdup(sp + 1);
			r->m.heads = lappend(r->m.heads, h);
		}
		else
			corrupt(r, "a line is of no known kind");
	}
	if (ferror(f))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read file \"%s\": %m", path)));
	FreeFile(f);
	if (n == 0)
		corrupt(r, "it is empty");
	MemoryContextSwitchTo(old);
	return true;
}

static int
head_cmp(const ListCell *a, const ListCell *b)
{
	return strcmp(((RemoteHead *) lfirst(a))->branch,
				  ((RemoteHead *) lfirst(b))->branch);
}

/* Write m as the manifest, durably: aside, synced, renamed over it. */
static void
write_manifest(ProllyRemote *r, Manifest *m)
{
	StringInfoData buf;
	char	   *path = remote_path(r, MANIFEST_NAME);
	char	   *tmp = psprintf("%s.tmp", path);
	ListCell   *lc;
	int			fd;

	initStringInfo(&buf);
	appendStringInfo(&buf, "%s\nformat%s%s\nlast %llu\n", MANIFEST_MAGIC,
					 m->format[0] ? " " : "", m->format,
					 (unsigned long long) m->last);
	foreach(lc, m->tables)
	{
		RemoteTable *t = lfirst(lc);

		appendStringInfo(&buf, "file %016llx %llu\n", (unsigned long long) t->id,
						 (unsigned long long) t->size);
	}
	list_sort(m->heads, head_cmp);
	foreach(lc, m->heads)
	{
		RemoteHead *h = lfirst(lc);

		appendStringInfoString(&buf, "head ");
		hex_of(&h->commit, &buf);
		appendStringInfo(&buf, " %s\n", h->branch);
	}
	fd = OpenTransientFile(tmp, O_WRONLY | O_CREAT | O_TRUNC | PG_BINARY);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m", tmp)));
	if (write(fd, buf.data, buf.len) != buf.len)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write file \"%s\": %m", tmp)));
	if (pg_fsync(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m", tmp)));
	if (CloseTransientFile(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", tmp)));
	(void) durable_rename(tmp, path, ERROR);
	pfree(buf.data);
}

static void close_tables(Manifest *m);

/* Take the remote's lock, until unlock_remote(); then read its manifest. */
static int
lock_remote(ProllyRemote *r)
{
	char	   *path = remote_path(r, LOCK_NAME);
	int			fd = OpenTransientFile(path, O_RDWR | O_CREAT | PG_BINARY);

	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", path)));
#ifndef WIN32
	if (flock(fd, LOCK_EX) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not lock file \"%s\": %m", path)));
#else
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("file remotes can't be changed on this platform")));
#endif
	close_tables(&r->m);
	(void) read_manifest(r);
	return fd;
}

static void
unlock_remote(int fd)
{
	/* (closing the file releases its lock) */
	(void) CloseTransientFile(fd);
}

static void
file_check_url(const char *url)
{
	if (strncmp(url, "file://", 7) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("\"%s\" isn't a file remote's URL", url),
				 errhint("A file remote's URL is file:// and an absolute path.")));
	if (!is_absolute_path(url + 7))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("a file remote's path must be absolute: \"%s\"", url)));
}

static ProllyRemote *
file_open(const char *url, bool create)
{
	ProllyRemote *r;
	MemoryContext cxt;
	const char *path;
	struct stat st;

	file_check_url(url);
	path = url + 7;
	cxt = AllocSetContextCreate(CurrentMemoryContext, "prolly file remote",
								ALLOCSET_DEFAULT_SIZES);
	r = MemoryContextAllocZero(cxt, sizeof(ProllyRemote));
	r->cxt = cxt;
	r->dir = MemoryContextStrdup(cxt, path);
	canonicalize_path(r->dir);
	check_privilege(r, create);

	if (stat(r->dir, &st) != 0)
	{
		if (errno != ENOENT || !create)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("no file remote at \"%s\": %m", r->dir)));
		if (pg_mkdir_p(r->dir, pg_dir_create_mode) != 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not create directory \"%s\": %m", r->dir)));
	}
	else if (!S_ISDIR(st.st_mode))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("\"%s\" isn't a directory", r->dir)));
	if (!read_manifest(r) && !create)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("no file remote at \"%s\"", r->dir),
				 errdetail("The directory has no %s file.", MANIFEST_NAME)));
	return r;
}

static void
close_tables(Manifest *m)
{
	ListCell   *lc;

	foreach(lc, m->tables)
	{
		RemoteTable *t = lfirst(lc);

		if (t->tf)
			PROLLY_CALL(table_format(), close, t->tf);
		if (t->fd >= 0)
			(void) CloseTransientFile(t->fd);
		t->tf = NULL;
		t->fd = -1;
	}
}

static void
file_close(ProllyRemote *r)
{
	close_tables(&r->m);
	MemoryContextDelete(r->cxt);
}

static void
file_format(ProllyRemote *r, StringInfo out)
{
	appendStringInfoString(out, r->m.format);
}

static void
file_heads(ProllyRemote *r, ProllyRemoteHeadVisit visit, void *arg)
{
	ListCell   *lc;

	foreach(lc, r->m.heads)
	{
		RemoteHead *h = lfirst(lc);

		visit(h->branch, &h->commit, arg);
	}
}

static bool
table_read(void *arg, uint64 offset, char *buf, Size len)
{
	RemoteTable *t = arg;

	return pg_pread(t->fd, buf, len, offset) == len;
}

/* A table file, opened (its index read) if it wasn't. */
static ProllyTableFile *
open_table(ProllyRemote *r, RemoteTable *t)
{
	MemoryContext old;

	if (t->tf)
		return t->tf;
	old = MemoryContextSwitchTo(r->cxt);
	t->path = remote_path(r, table_name(t->id));
	t->fd = OpenTransientFile(t->path, O_RDONLY | PG_BINARY);
	if (t->fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", t->path)));
	t->tf = PROLLY_CALL(table_format(), open, table_read, t, t->size, r->cxt);
	if (t->tf == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("file \"%s\" isn't a whole table file", t->path)));
	MemoryContextSwitchTo(old);
	return t->tf;
}

static bool
file_find(ProllyRemote *r, const ProllyHash *hash, RemoteTable **found,
		  uint64 *offset, uint32 *len)
{
	ListCell   *lc;

	foreach(lc, r->m.tables)
	{
		RemoteTable *t = lfirst(lc);

		if (PROLLY_CALL(table_format(), find, open_table(r, t), hash, offset, len))
		{
			*found = t;
			return true;
		}
	}
	return false;
}

static bool
file_get(ProllyRemote *r, const ProllyHash *hash, StringInfo out)
{
	RemoteTable *t;
	uint64		offset;
	uint32		len;
	char	   *bytes;

	if (!file_find(r, hash, &t, &offset, &len))
		return false;
	bytes = palloc(len);
	if (!table_read(t, offset, bytes, len))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read %u bytes at %llu in file \"%s\": %m",
						len, (unsigned long long) offset, t->path)));
	PROLLY_CALL(table_format(), decode, t->tf, bytes, len, out);
	pfree(bytes);
	return true;
}

static bool
file_has(ProllyRemote *r, const ProllyHash *hash)
{
	RemoteTable *t;
	uint64		offset;
	uint32		len;

	return file_find(r, hash, &t, &offset, &len);
}

static void
file_add_chunks(ProllyRemote *r, ProllyChunkNext next, void *arg)
{
	const ProllyTableFileRoutine *fmt = table_format();
	StringInfoData data;
	StringInfoData buf;
	ProllyHash	hash;
	ProllyTableFileWriter *w;
	Manifest	m;
	RemoteTable *t;
	char	   *path;
	uint64		size = 0;
	int			lock;
	int			fd;

	check_privilege(r, true);
	initStringInfo(&data);
	if (!next(arg, &hash, &data))
		return;
	lock = lock_remote(r);
	m = r->m;
	path = remote_path(r, table_name(m.last + 1));
	fd = OpenTransientFile(path, O_WRONLY | O_CREAT | O_TRUNC | PG_BINARY);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m", path)));
	w = PROLLY_CALL(fmt, write_begin, CurrentMemoryContext);
	initStringInfo(&buf);
	do
	{
		PROLLY_CALL(fmt, write_add, w, &hash, data.data, data.len, &buf);
		resetStringInfo(&data);
		if (buf.len >= 1024 * 1024)
		{
			if (write(fd, buf.data, buf.len) != buf.len)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not write file \"%s\": %m", path)));
			size += buf.len;
			resetStringInfo(&buf);
		}
		CHECK_FOR_INTERRUPTS();
	} while (next(arg, &hash, &data));
	PROLLY_CALL(fmt, write_finish, w, &buf);
	if (write(fd, buf.data, buf.len) != buf.len)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write file \"%s\": %m", path)));
	size += buf.len;
	if (pg_fsync(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m", path)));
	if (CloseTransientFile(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", path)));

	/* named: the newest file first, where recent chunks are */
	t = MemoryContextAllocZero(r->cxt, sizeof(RemoteTable));
	t->id = m.last + 1;
	t->size = size;
	t->fd = -1;
	m.last = t->id;
	m.tables = lcons(t, list_copy(m.tables));
	write_manifest(r, &m);
	r->m = m;
	unlock_remote(lock);
}

static bool
file_move_head(ProllyRemote *r, const char *branch, const ProllyHash *expected,
			   const ProllyHash *head, const char *format)
{
	Manifest	m;
	RemoteHead *at = NULL;
	ListCell   *lc;
	int			lock;

	check_privilege(r, true);
	if (strchr(branch, '\n') != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("a file remote's branch name can't hold a newline")));
	lock = lock_remote(r);
	m = r->m;
	foreach(lc, m.heads)
	{
		RemoteHead *h = lfirst(lc);

		if (strcmp(h->branch, branch) == 0)
			at = h;
	}
	if (at ? !(expected->len == at->commit.len &&
			   memcmp(expected->bytes, at->commit.bytes, expected->len) == 0) :
		expected->len != 0)
	{
		unlock_remote(lock);
		return false;
	}
	if (head && m.format[0] != '\0' && strcmp(m.format, format) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("file remote \"%s\" holds commits of canonical format %s, not %s",
						r->dir, m.format, format)));
	m.heads = list_copy(m.heads);
	if (at)
		m.heads = list_delete_ptr(m.heads, at);
	if (head)
	{
		RemoteHead *h = MemoryContextAllocZero(r->cxt, sizeof(RemoteHead));

		h->branch = MemoryContextStrdup(r->cxt, branch);
		h->commit = *head;
		m.heads = lappend(m.heads, h);
		if (m.format[0] == '\0')
			m.format = MemoryContextStrdup(r->cxt, format);
	}
	write_manifest(r, &m);
	r->m = m;
	unlock_remote(lock);
	return true;
}

const ProllyRemoteRoutine prolly_remote_file = {
	.info = {
		.iface = PROLLY_IFACE_REMOTE,
		.impl_id = 1,
		.format_version = 1,
		.name = "file",
		.description = "a remote in a directory of the server's files",
	},
	.check_url = file_check_url,
	.open = file_open,
	.close = file_close,
	.format = file_format,
	.heads = file_heads,
	.get = file_get,
	.has = file_has,
	.add_chunks = file_add_chunks,
	.move_head = file_move_head,
};

const ProllyRemoteRoutine *
prolly_remote_for_url(const char *url)
{
	const char *colon = strstr(url, "://");
	char	   *scheme;
	const ProllyImplInfo *impl;

	if (colon == NULL || colon == url)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("\"%s\" isn't a remote's URL", url),
				 errhint("A remote's URL begins with its scheme, as file:///path does.")));
	scheme = pnstrdup(url, colon - url);
	impl = prolly_lookup_impl(PROLLY_IFACE_REMOTE, scheme);
	if (impl == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("remotes of scheme \"%s\" aren't supported", scheme)));
	pfree(scheme);
	PROLLY_CALL((const ProllyRemoteRoutine *) impl, check_url, url);
	return (const ProllyRemoteRoutine *) impl;
}
