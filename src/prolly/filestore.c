/*
 * filestore.c
 *	  File-backed ChunkStore: chunks are appended to one data file, read
 *	  through a memory mapping of it, and found through an in-memory index.
 *
 * Bytes returned by get point into the mapping and stay valid for the
 * life of the store.  Freed chunks leave their bytes in the file (the file
 * is never compacted); bytes() reports live bytes, file_bytes the file.
 */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "chunkstore.h"

/* Address space reserved for the mapping; the file grows into it. */
#define FS_RESERVE ((size_t) 1 << 41)	/* 2 TB */

#define SLOT_EMPTY 0
#define SLOT_USED 1
#define SLOT_DELETED 2

typedef struct FileEntry
{
	PHash		h;
	uint64_t	off;
	uint32_t	len;
	uint8_t		state;
	ChunkMeta	meta;
} FileEntry;

typedef struct FileStore
{
	ChunkStore	base;
	int			fd;
	char	   *path;
	uint8_t    *map;
	uint64_t	end;			/* bytes written to the file */
	FileEntry  *tab;
	size_t		cap;
	size_t		used;
	size_t		deleted;
	uint64_t	live;
} FileStore;

static size_t
fs_slot(const PHash *h, size_t cap)
{
	uint64_t	x;

	memcpy(&x, h->b, sizeof(x));
	return (size_t) (x & (cap - 1));
}

static FileEntry *
fs_find(FileStore *fs, const PHash *h)
{
	for (size_t i = fs_slot(h, fs->cap);; i = (i + 1) & (fs->cap - 1))
	{
		FileEntry  *e = &fs->tab[i];

		if (e->state == SLOT_EMPTY)
			return NULL;
		if (e->state == SLOT_USED && phash_eq(&e->h, h))
			return e;
	}
}

static void
fs_rehash(FileStore *fs, size_t cap)
{
	FileEntry  *old = fs->tab;
	size_t		oldcap = fs->cap;

	fs->tab = calloc(cap, sizeof(FileEntry));
	if (fs->tab == NULL)
		abort();
	fs->cap = cap;
	fs->deleted = 0;
	for (size_t i = 0; i < oldcap; i++)
	{
		if (old[i].state != SLOT_USED)
			continue;
		for (size_t j = fs_slot(&old[i].h, cap);; j = (j + 1) & (cap - 1))
			if (fs->tab[j].state == SLOT_EMPTY)
			{
				fs->tab[j] = old[i];
				break;
			}
	}
	free(old);
}

static const uint8_t *
fs_get(ChunkStore *cs, const PHash *h, size_t *len)
{
	FileStore  *fs = (FileStore *) cs;
	FileEntry  *e = fs_find(fs, h);

	if (e == NULL)
		return NULL;
	*len = e->len;
	return fs->map + e->off;
}

static bool
fs_put(ChunkStore *cs, const PHash *h, const uint8_t *data, size_t len)
{
	FileStore  *fs = (FileStore *) cs;
	FileEntry  *slot = NULL;
	size_t		done = 0;

	if (fs_find(fs, h))
		return false;
	if (fs->end + len > FS_RESERVE)
	{
		fprintf(stderr, "filestore: %s is full\n", fs->path);
		abort();
	}
	while (done < len)
	{
		ssize_t		n = pwrite(fs->fd, data + done, len - done, (off_t) (fs->end + done));

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
		{
			fprintf(stderr, "filestore: write to %s failed: %s\n", fs->path,
					strerror(errno));
			abort();
		}
		done += (size_t) n;
	}
	if ((fs->used + fs->deleted + 1) * 10 > fs->cap * 7)
		fs_rehash(fs, (fs->used + 1) * 10 > fs->cap * 5 ? fs->cap * 2 : fs->cap);
	for (size_t i = fs_slot(h, fs->cap);; i = (i + 1) & (fs->cap - 1))
		if (fs->tab[i].state != SLOT_USED)
		{
			slot = &fs->tab[i];
			break;
		}
	if (slot->state == SLOT_DELETED)
		fs->deleted--;
	slot->state = SLOT_USED;
	slot->h = *h;
	slot->off = fs->end;
	slot->len = (uint32_t) len;
	memset(&slot->meta, 0, sizeof(slot->meta));
	fs->end += len;
	fs->used++;
	fs->live += len;
	return true;
}

static bool
fs_has(ChunkStore *cs, const PHash *h)
{
	return fs_find((FileStore *) cs, h) != NULL;
}

static ChunkMeta *
fs_meta(ChunkStore *cs, const PHash *h)
{
	FileEntry  *e = fs_find((FileStore *) cs, h);

	return e ? &e->meta : NULL;
}

static void
fs_del(ChunkStore *cs, const PHash *h)
{
	FileStore  *fs = (FileStore *) cs;
	FileEntry  *e = fs_find(fs, h);

	if (e == NULL)
		return;
	fs->live -= e->len;
	e->state = SLOT_DELETED;
	fs->used--;
	fs->deleted++;
}

static void
fs_foreach(ChunkStore *cs, ChunkVisitFn fn, void *arg)
{
	FileStore  *fs = (FileStore *) cs;

	for (size_t i = 0; i < fs->cap; i++)
		if (fs->tab[i].state == SLOT_USED)
			fn(arg, &fs->tab[i].h);
}

static size_t
fs_count(ChunkStore *cs)
{
	return ((FileStore *) cs)->used;
}

static uint64_t
fs_bytes(ChunkStore *cs)
{
	return ((FileStore *) cs)->live;
}

static void
fs_destroy(ChunkStore *cs)
{
	FileStore  *fs = (FileStore *) cs;

	munmap(fs->map, FS_RESERVE);
	close(fs->fd);
	unlink(fs->path);
	free(fs->path);
	free(fs->tab);
	free(fs);
}

static const ChunkStoreOps file_ops = {
	fs_get, fs_put, fs_has, fs_meta, fs_del, fs_foreach,
	fs_count, fs_bytes, fs_destroy, NULL, NULL
};

/* Creates a store backed by a new file at path (replacing any file there). */
ChunkStore *
filestore_create(const char *path)
{
	FileStore  *fs = px_malloc(sizeof(FileStore));

	memset(fs, 0, sizeof(*fs));
	fs->base.ops = &file_ops;
	fs->path = px_memdup(path, strlen(path) + 1);
	fs->fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
	if (fs->fd < 0)
	{
		fprintf(stderr, "filestore: cannot create %s: %s\n", path, strerror(errno));
		abort();
	}
	fs->map = mmap(NULL, FS_RESERVE, PROT_READ, MAP_SHARED | MAP_NORESERVE, fs->fd, 0);
	if (fs->map == MAP_FAILED)
	{
		fprintf(stderr, "filestore: cannot map %s: %s\n", path, strerror(errno));
		abort();
	}
	fs->cap = 1 << 16;
	fs->tab = calloc(fs->cap, sizeof(FileEntry));
	if (fs->tab == NULL)
		abort();
	return &fs->base;
}

uint64_t
filestore_file_bytes(ChunkStore *cs)
{
	return ((FileStore *) cs)->end;
}
