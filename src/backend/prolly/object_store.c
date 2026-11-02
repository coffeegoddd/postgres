/*-------------------------------------------------------------------------
 *
 * object_store.c
 *	  Object stores (prolly/object_store.h), and "directory", which keeps
 *	  objects as files in a directory.
 *
 * An object is a file of the directory itself, its name the object's with
 * each "/" made "#" (names have no "#"), so that every name is a file of
 * its own, "a" and "a/b" both, and there are no directories to make,
 * empty out or sync but the one.  It is written to a temporary file (a
 * name of "~" and random digits, which no object's has), synced, and
 * linked into place: a link, unlike a rename, fails if the name is taken,
 * so an object is never replaced, and is there whole or not at all.  The
 * directory is synced after.  A crash mid-write leaves a temporary file,
 * which listings pass by; it is not reclaimed (another writer may be
 * writing it).
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

/* whether the n bytes at s, a name's first part to its first ".", are a device's */
static bool
windows_device(const char *s, size_t n)
{
	static const char *const devices[] = {"con", "prn", "aux", "nul"};

	for (int i = 0; i < lengthof(devices); i++)
		if (n == 3 && strncmp(s, devices[i], 3) == 0)
			return true;
	return n == 4 && (strncmp(s, "com", 3) == 0 || strncmp(s, "lpt", 3) == 0) &&
		s[3] >= '0' && s[3] <= '9';
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
	if (name[0] == '.')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("object name \"%s\" begins with \".\"", name)));
	for (const char *c = name;; c++)
	{
		if (*c == '/' || *c == '\0')
		{
			size_t		n = c - part;

			if (n == 0 || part[n - 1] == '.')
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("object name \"%s\" has an empty part, or one ending in \".\"", name)));
			if (*c == '\0')
				break;
			part = c + 1;
		}
		else if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') ||
				   *c == '.' || *c == '_' || *c == '-'))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("object name \"%s\" has a character other than lowercase letters, digits, \"._-\" and \"/\"",
							name)));
	}
	if (windows_device(name, strcspn(name, "./")))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("object name \"%s\" begins with a name Windows keeps for a device", name)));
}

/* ------------------------------------------------------------- directory */

typedef struct DirectoryStore
{
	ObjectStore store;
	char	   *dir;			/* absolute, or the data directory's */
} DirectoryStore;

/* the file of the object name: in the directory, each "/" made "#" */
static char *
dir_path(DirectoryStore *ds, const char *name)
{
	char	   *path;

	ObjectNameCheck(name);
	path = psprintf("%s/%s", ds->dir, name);
	for (char *c = path + strlen(ds->dir) + 1; *c; c++)
		if (*c == '/')
			*c = '#';
	return path;
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
	/* (its trailing "/"s off, but the root's) */
	for (size_t n = strlen(ds->dir); n > 1 && ds->dir[n - 1] == '/'; n--)
		ds->dir[n - 1] = '\0';

	if (MakePGDirectory(ds->dir) != 0 && errno != EEXIST)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create directory \"%s\": %m", ds->dir)));

	/*
	 * (it and its entry in its parent synced, at every open: one made just
	 * before a crash, and not synced then, is synced before it is used)
	 */
	{
		char	   *parent = pstrdup(ds->dir);
		char	   *slash = strrchr(parent, '/');

		fsync_fname(ds->dir, true);
		if (slash == NULL)
			fsync_fname(".", true);
		else if (slash[1] != '\0')
		{
			*slash = '\0';
			fsync_fname(slash == parent ? "/" : parent, true);
		}
		pfree(parent);
	}
	return &ds->store;
}

static void
dir_close(ObjectStore *os)
{
	DirectoryStore *ds = (DirectoryStore *) os;

	pfree(ds->dir);
	pfree(ds);
}

static void
dir_put(ObjectStore *os, const char *name, const void *data, size_t len)
{
	DirectoryStore *ds = (DirectoryStore *) os;
	char	   *path = dir_path(ds, name);
	uint64		rand;
	char	   *tmp;
	int			fd;
	size_t		done = 0;

	if (!pg_strong_random(&rand, sizeof(rand)))
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("could not make a random name for a temporary file")));
	tmp = psprintf("%s/~%016llx", ds->dir, (unsigned long long) rand);
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
	/* (a temporary file, never used again: its failure an error alone) */
	if (pg_fsync(fd) != 0)
	{
		int			save_errno = errno;

		CloseTransientFile(fd);
		unlink(tmp);
		errno = save_errno;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m", tmp)));
	}
	CloseTransientFile(fd);
	INJECTION_POINT("object-store-written", NULL);

	/*
	 * (a link fails if the name is taken: an object is never replaced; the
	 * one there made durable before it is said to be, whoever linked it)
	 */
	if (link(tmp, path) != 0)
	{
		int			save_errno = errno;

		unlink(tmp);
		if (save_errno == EEXIST)
		{
			fsync_fname(ds->dir, true);
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("object \"%s\" is there already", name)));
		}
		errno = save_errno;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not link file \"%s\" to \"%s\": %m", tmp, path)));
	}
	unlink(tmp);
	fsync_fname(ds->dir, true);
	pfree(path);
	pfree(tmp);
}

/* the object's size, its file's (a regular one); false if it is not there */
static bool
dir_stat(DirectoryStore *ds, const char *name, uint64 *size)
{
	char	   *path = dir_path(ds, name);
	struct stat st;

	if (lstat(path, &st) != 0)
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
	if (!S_ISREG(st.st_mode))
		return false;
	*size = (uint64) st.st_size;
	return true;
}

static void
dir_read(ObjectStore *os, const char *name, uint64 off, void *dst, size_t len)
{
	DirectoryStore *ds = (DirectoryStore *) os;
	char	   *path = dir_path(ds, name);
	uint64		size;
	int			fd;
	size_t		done = 0;

	if (!dir_stat(ds, name, &size))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("object \"%s\" is not there", name)));
	if (off > size || len > size - off)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("bytes %llu to %llu are not all in object \"%s\" (%llu bytes)",
						(unsigned long long) off, (unsigned long long) (off + len),
						name, (unsigned long long) size)));
	fd = OpenTransientFile(path, O_RDONLY | PG_BINARY);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open object \"%s\": %m", name)));
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
		/* (shorter than its size said: never written so, so damaged) */
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
	return dir_stat((DirectoryStore *) os, name, size);
}

typedef struct DirEntry
{
	char	   *name;
	uint64		size;
} DirEntry;

static int
dir_entry_cmp(const void *a, const void *b)
{
	return strcmp(((const DirEntry *) a)->name, ((const DirEntry *) b)->name);
}

static void
dir_list(ObjectStore *os, const char *prefix, ObjectListFn fn, void *arg)
{
	DirectoryStore *ds = (DirectoryStore *) os;
	DIR		   *dir = AllocateDir(ds->dir);
	struct dirent *de;
	DirEntry   *entries = NULL;
	int			n = 0,
				cap = 0;
	size_t		plen = strlen(prefix);

	while ((de = ReadDir(dir, ds->dir)) != NULL)
	{
		char	   *name;
		char	   *full;
		struct stat st;

		/* (".", "..", temporary files, and none an object's) */
		if (de->d_name[0] == '.' || de->d_name[0] == '~')
			continue;
		name = pstrdup(de->d_name);
		for (char *c = name; *c; c++)
			if (*c == '#')
				*c = '/';
		if (strncmp(name, prefix, plen) != 0)
		{
			pfree(name);
			continue;
		}
		full = psprintf("%s/%s", ds->dir, de->d_name);
		if (lstat(full, &st) != 0)
		{
			/* (removed meanwhile) */
			if (errno == ENOENT)
			{
				pfree(full);
				pfree(name);
				continue;
			}
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not stat file \"%s\": %m", full)));
		}
		pfree(full);
		if (!S_ISREG(st.st_mode))
		{
			pfree(name);
			continue;
		}
		if (n == cap)
		{
			cap = Max(16, cap * 2);
			entries = entries ? repalloc_array(entries, DirEntry, cap) :
				palloc_array(DirEntry, cap);
		}
		entries[n].name = name;
		entries[n].size = (uint64) st.st_size;
		n++;
	}
	FreeDir(dir);
	if (n > 1)
		qsort(entries, n, sizeof(DirEntry), dir_entry_cmp);
	for (int i = 0; i < n; i++)
		fn(arg, entries[i].name, entries[i].size);
	for (int i = 0; i < n; i++)
		pfree(entries[i].name);
	if (entries)
		pfree(entries);
}

static void
dir_remove(ObjectStore *os, const char *name)
{
	DirectoryStore *ds = (DirectoryStore *) os;
	char	   *path = dir_path(ds, name);

	if (unlink(path) != 0 && errno != ENOENT)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not remove object \"%s\": %m", name)));
	fsync_fname(ds->dir, true);
	pfree(path);
}

const ObjectStoreRoutine DirectoryObjectStore = {
	.open = dir_open,
	.close = dir_close,
	.put = dir_put,
	.read = dir_read,
	.size = dir_size,
	.list = dir_list,
	.remove = dir_remove,
};
