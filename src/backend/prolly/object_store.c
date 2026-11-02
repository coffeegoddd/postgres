/*-------------------------------------------------------------------------
 *
 * object_store.c
 *	  Object stores (prolly/object_store.h), and "directory", which keeps
 *	  objects as files under a directory.
 *
 * An object is written to a temporary file beside where it goes (its name,
 * ".tmp.", and the writer's pid and a count), synced, and linked into place:
 * a link, unlike a rename, fails if the name is taken, so an object is
 * never replaced, and is there whole or not at all.  The directory that
 * holds it is synced after, so that it is there after a crash.  A crash
 * mid-write leaves a temporary file, which listings pass by.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/backend/prolly/object_store.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/stat.h>
#include <unistd.h>

#include "common/file_perm.h"
#include "miscadmin.h"
#include "prolly/object_store.h"
#include "prolly/registry.h"
#include "storage/fd.h"
#include "utils/injection_point.h"
#include "utils/memutils.h"

static void
register_object_stores(void)
{
	static bool registered = false;

	if (registered)
		return;
	ProllyRegisterImplementation(OBJECT_STORE_IFACE, "directory", &DirectoryObjectStore);
	registered = true;
}

ObjectStore *
ObjectStoreOpen(const char *impl, const char *location)
{
	const ProllyImplementation *found;

	register_object_stores();
	found = ProllyLookupImplementation(OBJECT_STORE_IFACE, impl);
	if (found == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("there is no object store \"%s\"", impl)));
	return ((const ObjectStoreRoutine *) found->routine)->open(location);
}

List *
ObjectStoreList(void)
{
	register_object_stores();
	return ProllyListImplementations(OBJECT_STORE_IFACE);
}

void
ObjectNameCheck(const char *name)
{
	size_t		len = strlen(name);
	const char *part = name;

	if (len == 0 || len > OBJECT_NAME_MAX)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("an object's name must be 1 to %d bytes long", OBJECT_NAME_MAX)));
	for (const char *c = name;; c++)
	{
		if (*c == '/' || *c == '\0')
		{
			size_t		n = c - part;

			if (n == 0 || (n == 1 && part[0] == '.') ||
				(n == 2 && part[0] == '.' && part[1] == '.'))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("object name \"%s\" has an empty, \".\" or \"..\" part", name)));
			if (*c == '\0')
				break;
			part = c + 1;
		}
		else if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
				   (*c >= '0' && *c <= '9') || *c == '.' || *c == '_' || *c == '-'))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("object name \"%s\" has a character other than letters, digits, \"._-\" and \"/\"",
							name)));
	}
	/* (the temporary files' names are the store's own) */
	if (strstr(name, ".tmp.") != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("object name \"%s\" has \".tmp.\" in it", name)));
}

/* ------------------------------------------------------------- directory */

typedef struct DirectoryStore
{
	ObjectStore store;
	char	   *dir;			/* absolute, or the data directory's */
} DirectoryStore;

static char *
dir_path(DirectoryStore *ds, const char *name)
{
	ObjectNameCheck(name);
	return psprintf("%s/%s", ds->dir, name);
}

/* the directories above path made, each synced into its parent */
static void
dir_make_parents(DirectoryStore *ds, const char *path)
{
	char	   *p = pstrdup(path);

	for (char *s = p + strlen(ds->dir) + 1; *s; s++)
	{
		if (*s != '/')
			continue;
		*s = '\0';
		if (MakePGDirectory(p) == 0)
		{
			char	   *slash = strrchr(p, '/');

			fsync_fname(p, true);
			*slash = '\0';
			fsync_fname(p, true);
			*slash = '/';
		}
		else if (errno != EEXIST)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not create directory \"%s\": %m", p)));
		*s = '/';
	}
	pfree(p);
}

static ObjectStore *
dir_open(const char *location)
{
	DirectoryStore *ds = palloc0_object(DirectoryStore);

	if (location == NULL || location[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("a directory object store needs a directory")));
	ds->store.routine = &DirectoryObjectStore;
	ds->dir = pstrdup(location);
	if (MakePGDirectory(ds->dir) != 0 && errno != EEXIST)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create directory \"%s\": %m", ds->dir)));
	return &ds->store;
}

static void
dir_put(ObjectStore *os, const char *name, const void *data, size_t len)
{
	DirectoryStore *ds = (DirectoryStore *) os;
	static uint32 count = 0;
	char	   *path = dir_path(ds, name);
	char	   *tmp = psprintf("%s.tmp.%d.%u", path, MyProcPid, count++);
	char	   *slash;
	int			fd;
	size_t		done = 0;

	dir_make_parents(ds, path);
	fd = OpenTransientFile(tmp, O_WRONLY | O_CREAT | O_EXCL | PG_BINARY);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m", tmp)));
	while (done < len)
	{
		ssize_t		n = pg_pwrite(fd, (const char *) data + done, len - done, done);

		if (n <= 0)
		{
			int			save_errno = errno;

			CloseTransientFile(fd);
			unlink(tmp);
			errno = n < 0 ? save_errno : ENOSPC;
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not write file \"%s\": %m", tmp)));
		}
		done += n;
	}
	if (pg_fsync(fd) != 0)
	{
		int			save_errno = errno;

		CloseTransientFile(fd);
		unlink(tmp);
		errno = save_errno;
		ereport(data_sync_elevel(ERROR),
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m", tmp)));
	}
	CloseTransientFile(fd);
	INJECTION_POINT("object-store-written", NULL);

	/* (a link fails if the name is taken: an object is never replaced) */
	if (link(tmp, path) != 0)
	{
		int			save_errno = errno;

		unlink(tmp);
		errno = save_errno;
		if (save_errno == EEXIST)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("object \"%s\" is there already", name)));
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not link file \"%s\" to \"%s\": %m", tmp, path)));
	}
	unlink(tmp);
	slash = strrchr(path, '/');
	*slash = '\0';
	fsync_fname(path, true);
	pfree(path);
	pfree(tmp);
}

static void
dir_read(ObjectStore *os, const char *name, uint64 off, void *dst, size_t len)
{
	DirectoryStore *ds = (DirectoryStore *) os;
	char	   *path = dir_path(ds, name);
	int			fd = OpenTransientFile(path, O_RDONLY | PG_BINARY);
	size_t		done = 0;

	if (fd < 0)
	{
		if (errno == ENOENT)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("object \"%s\" is not there", name)));
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open object \"%s\": %m", name)));
	}
	while (done < len)
	{
		ssize_t		n = pg_pread(fd, (char *) dst + done, len - done, off + done);

		if (n < 0)
		{
			int			save_errno = errno;

			CloseTransientFile(fd);
			errno = save_errno;
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read object \"%s\": %m", name)));
		}
		if (n == 0)
		{
			CloseTransientFile(fd);
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("object \"%s\" has fewer than %llu bytes",
							name, (unsigned long long) (off + len))));
		}
		done += n;
	}
	CloseTransientFile(fd);
	pfree(path);
}

static bool
dir_size(ObjectStore *os, const char *name, uint64 *size)
{
	DirectoryStore *ds = (DirectoryStore *) os;
	char	   *path = dir_path(ds, name);
	struct stat st;

	if (stat(path, &st) != 0)
	{
		if (errno == ENOENT)
		{
			pfree(path);
			return false;
		}
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not stat object \"%s\": %m", name)));
	}
	pfree(path);
	*size = (uint64) st.st_size;
	return true;
}

typedef struct DirEntry
{
	char	   *name;
	uint64		size;
} DirEntry;

typedef struct DirListing
{
	DirEntry   *entries;
	int			n;
	int			cap;
} DirListing;

/* the objects under rel (a directory, relative to the store's) gathered */
static void
dir_gather(DirectoryStore *ds, const char *rel, DirListing *out)
{
	char	   *path = rel[0] ? psprintf("%s/%s", ds->dir, rel) : pstrdup(ds->dir);
	DIR		   *dir = AllocateDir(path);
	struct dirent *de;

	while ((de = ReadDir(dir, path)) != NULL)
	{
		char	   *name;
		char	   *full;
		struct stat st;

		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0 ||
			strstr(de->d_name, ".tmp.") != NULL)
			continue;
		name = rel[0] ? psprintf("%s/%s", rel, de->d_name) : pstrdup(de->d_name);
		full = psprintf("%s/%s", ds->dir, name);
		if (stat(full, &st) != 0)
		{
			/* (removed meanwhile) */
			if (errno == ENOENT)
				continue;
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not stat file \"%s\": %m", full)));
		}
		if (S_ISDIR(st.st_mode))
			dir_gather(ds, name, out);
		else if (S_ISREG(st.st_mode))
		{
			if (out->n == out->cap)
			{
				out->cap = Max(16, out->cap * 2);
				out->entries = out->entries ?
					repalloc_array(out->entries, DirEntry, out->cap) :
					palloc_array(DirEntry, out->cap);
			}
			out->entries[out->n].name = name;
			out->entries[out->n].size = (uint64) st.st_size;
			out->n++;
		}
		pfree(full);
	}
	FreeDir(dir);
	pfree(path);
}

static int
dir_entry_cmp(const void *a, const void *b)
{
	return strcmp(((const DirEntry *) a)->name, ((const DirEntry *) b)->name);
}

static void
dir_list(ObjectStore *os, const char *prefix, ObjectListFn fn, void *arg)
{
	DirectoryStore *ds = (DirectoryStore *) os;
	DirListing	all = {0};

	dir_gather(ds, "", &all);
	if (all.n > 1)
		qsort(all.entries, all.n, sizeof(DirEntry), dir_entry_cmp);
	for (int i = 0; i < all.n; i++)
		if (strncmp(all.entries[i].name, prefix, strlen(prefix)) == 0)
			fn(arg, all.entries[i].name, all.entries[i].size);
}

static bool
dir_remove(ObjectStore *os, const char *name)
{
	DirectoryStore *ds = (DirectoryStore *) os;
	char	   *path = dir_path(ds, name);
	char	   *slash;

	if (unlink(path) != 0)
	{
		if (errno == ENOENT)
		{
			pfree(path);
			return false;
		}
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not remove object \"%s\": %m", name)));
	}
	slash = strrchr(path, '/');
	*slash = '\0';
	fsync_fname(path, true);
	pfree(path);
	return true;
}

const ObjectStoreRoutine DirectoryObjectStore = {
	.open = dir_open,
	.put = dir_put,
	.read = dir_read,
	.size = dir_size,
	.list = dir_list,
	.remove = dir_remove,
};
