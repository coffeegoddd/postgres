/*
 * dirstore.c
 *	  Durable ChunkStore in a directory of immutable chunk files.
 *
 * Layout of the directory:
 *	c-<id>.chunks	immutable chunk files: written whole to a temporary
 *					name, fsynced, renamed into place, never changed again
 *	MANIFEST		which chunk files are live, the durable root set and the
 *					repository-state root; replaced whole by rename
 *
 * Chunk file:
 *	records			per chunk: u32 length, 20-byte hash, the chunk's bytes
 *	index			per chunk, sorted by hash: 20-byte hash, u64 offset of
 *					the bytes, u32 length
 *	footer			"PRLYCHK1", u32 format, u64 chunk count, u64 index
 *					offset, u64 record bytes, 20-byte hash of the index
 * Manifest:
 *	"PRLYMAN1", u32 format, u64 next file id, u32 file count, u64 ids,
 *	u64 root count, (20-byte hash, u64 hold count)..., u8 has state,
 *	[20-byte state root], 20-byte hash of everything before it
 * All integers big-endian.
 *
 * New chunks are kept in memory until a flush writes them out as a new
 * file and then a new manifest.  Compaction (garbage collection) copies
 * the live chunks of files with enough dead space, or of small files, into
 * new files, installs a manifest without the old files, then deletes them:
 * that is where disk space comes back.  Reference counts are not stored:
 * opening the store rebuilds them by walking from the manifest's roots;
 * anything unreachable is dead.  Files the manifest does not name (left by
 * a crash) are deleted on open.
 *
 * Bytes returned by get stay valid until the next flush (for chunks not
 * yet written) or compaction (for chunks in the files it replaces).
 */
#define _DEFAULT_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "chunkstore.h"

#define FILE_MAGIC "PRLYCHK1"
#define MANIFEST_MAGIC "PRLYMAN1"
#define DS_FORMAT 1
#define REC_HDR (4 + PHASH_LEN)
#define INDEX_ENT (PHASH_LEN + 8 + 4)
#define FOOTER_LEN (8 + 4 + 8 + 8 + 8 + PHASH_LEN)
#define IN_MEM UINT32_MAX

#define SLOT_EMPTY 0
#define SLOT_USED 1
#define SLOT_DELETED 2

typedef struct DFile
{
	bool		used;
	uint64_t	id;
	uint8_t    *map;
	size_t		size;
	uint64_t	live_bytes;		/* record bytes of chunks still held */
	uint64_t	dead_bytes;
} DFile;

typedef struct DEntry
{
	PHash		h;
	ChunkMeta	meta;
	uint8_t    *mem;			/* chunks not yet written */
	uint64_t	off;			/* of the bytes, in the file */
	uint32_t	len;
	uint32_t	file;			/* slot in files, or IN_MEM */
	uint8_t		state;
} DEntry;

typedef struct DirStore
{
	ChunkStore	base;
	char	   *dir;
	DFile	   *files;
	size_t		nfiles;
	size_t		fcap;
	uint64_t	next_id;
	DEntry	   *tab;
	size_t		cap;
	size_t		used;
	size_t		deleted;
	uint64_t	live;
	PHashMap	roots;			/* chunk -> durable hold count */
	bool		has_state;
	PHash		state;
	int			failpoint;
	size_t		file_target;
} DirStore;

/* ----------------------------------------------------------------- helpers */

static void
put_be32(uint8_t *p, uint32_t v)
{
	for (int i = 3; i >= 0; i--, v >>= 8)
		p[i] = (uint8_t) v;
}

static void
put_be64(uint8_t *p, uint64_t v)
{
	for (int i = 7; i >= 0; i--, v >>= 8)
		p[i] = (uint8_t) v;
}

static uint32_t
get_be32(const uint8_t *p)
{
	return (uint32_t) p[0] << 24 | (uint32_t) p[1] << 16 | (uint32_t) p[2] << 8 | p[3];
}

static uint64_t
get_be64(const uint8_t *p)
{
	uint64_t	v = 0;

	for (int i = 0; i < 8; i++)
		v = v << 8 | p[i];
	return v;
}

static void
die(const char *what, const char *path)
{
	fprintf(stderr, "dirstore: %s %s: %s\n", what, path, strerror(errno));
	abort();
}

static void
fsync_dir(const char *dir)
{
	int			fd = open(dir, O_RDONLY | O_DIRECTORY);

	if (fd < 0 || fsync(fd) != 0)
		die("cannot sync directory", dir);
	close(fd);
}

/* Writes data to path.tmp, fsyncs it; renames it to path unless crash_first. */
static bool
write_file_atomic(const char *dir, const char *name, const uint8_t *data, size_t len,
				  bool crash_first)
{
	char		path[1024],
				tmp[1040];
	int			fd;
	size_t		done = 0;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0)
		die("cannot create", tmp);
	while (done < len)
	{
		ssize_t		n = write(fd, data + done, len - done);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			die("cannot write", tmp);
		done += (size_t) n;
	}
	if (fsync(fd) != 0)
		die("cannot sync", tmp);
	close(fd);
	if (crash_first)
		return false;			/* a crash between writing and renaming */
	if (rename(tmp, path) != 0)
		die("cannot rename", tmp);
	fsync_dir(dir);
	return true;
}

static void
file_name(char *buf, size_t n, uint64_t id)
{
	snprintf(buf, n, "c-%016llx.chunks", (unsigned long long) id);
}

/* ------------------------------------------------------------------ index */

static size_t
ds_slot(const PHash *h, size_t cap)
{
	uint64_t	x;

	memcpy(&x, h->b, sizeof(x));
	return (size_t) (x & (cap - 1));
}

static DEntry *
ds_find(DirStore *ds, const PHash *h)
{
	for (size_t i = ds_slot(h, ds->cap);; i = (i + 1) & (ds->cap - 1))
	{
		DEntry	   *e = &ds->tab[i];

		if (e->state == SLOT_EMPTY)
			return NULL;
		if (e->state == SLOT_USED && phash_eq(&e->h, h))
			return e;
	}
}

static void
ds_rehash(DirStore *ds, size_t cap)
{
	DEntry	   *old = ds->tab;
	size_t		oldcap = ds->cap;

	ds->tab = calloc(cap, sizeof(DEntry));
	if (ds->tab == NULL)
		abort();
	ds->cap = cap;
	ds->deleted = 0;
	for (size_t i = 0; i < oldcap; i++)
	{
		if (old[i].state != SLOT_USED)
			continue;
		for (size_t j = ds_slot(&old[i].h, cap);; j = (j + 1) & (cap - 1))
			if (ds->tab[j].state == SLOT_EMPTY)
			{
				ds->tab[j] = old[i];
				break;
			}
	}
	free(old);
}

static DEntry *
ds_insert(DirStore *ds, const PHash *h)
{
	if ((ds->used + ds->deleted + 1) * 10 > ds->cap * 7)
		ds_rehash(ds, (ds->used + 1) * 10 > ds->cap * 5 ? ds->cap * 2 : ds->cap);
	for (size_t i = ds_slot(h, ds->cap);; i = (i + 1) & (ds->cap - 1))
	{
		DEntry	   *e = &ds->tab[i];

		if (e->state != SLOT_USED)
		{
			if (e->state == SLOT_DELETED)
				ds->deleted--;
			memset(e, 0, sizeof(*e));
			e->state = SLOT_USED;
			e->h = *h;
			ds->used++;
			return e;
		}
	}
}

static void
ds_remove(DirStore *ds, DEntry *e)
{
	if (e->file == IN_MEM)
		free(e->mem);
	else
	{
		ds->files[e->file].live_bytes -= REC_HDR + e->len;
		ds->files[e->file].dead_bytes += REC_HDR + e->len;
	}
	ds->live -= e->len;
	e->mem = NULL;
	e->state = SLOT_DELETED;
	ds->used--;
	ds->deleted++;
}

/* ---------------------------------------------------------------- the ops */

static const uint8_t *
ds_get(ChunkStore *cs, const PHash *h, size_t *len)
{
	DirStore   *ds = (DirStore *) cs;
	DEntry	   *e = ds_find(ds, h);

	if (e == NULL)
		return NULL;
	*len = e->len;
	return e->file == IN_MEM ? e->mem : ds->files[e->file].map + e->off;
}

static bool
ds_put(ChunkStore *cs, const PHash *h, const uint8_t *data, size_t len)
{
	DirStore   *ds = (DirStore *) cs;
	DEntry	   *e;

	if (ds_find(ds, h))
		return false;
	e = ds_insert(ds, h);
	e->mem = px_memdup(data, len);
	e->len = (uint32_t) len;
	e->file = IN_MEM;
	ds->live += len;
	return true;
}

static bool
ds_has(ChunkStore *cs, const PHash *h)
{
	return ds_find((DirStore *) cs, h) != NULL;
}

static ChunkMeta *
ds_meta(ChunkStore *cs, const PHash *h)
{
	DEntry	   *e = ds_find((DirStore *) cs, h);

	return e ? &e->meta : NULL;
}

static void
ds_del(ChunkStore *cs, const PHash *h)
{
	DirStore   *ds = (DirStore *) cs;
	DEntry	   *e = ds_find(ds, h);

	if (e)
		ds_remove(ds, e);
}

static void
ds_foreach(ChunkStore *cs, ChunkVisitFn fn, void *arg)
{
	DirStore   *ds = (DirStore *) cs;

	for (size_t i = 0; i < ds->cap; i++)
		if (ds->tab[i].state == SLOT_USED)
			fn(arg, &ds->tab[i].h);
}

static size_t
ds_count(ChunkStore *cs)
{
	return ((DirStore *) cs)->used;
}

static uint64_t
ds_bytes(ChunkStore *cs)
{
	return ((DirStore *) cs)->live;
}

static void
ds_note_root(ChunkStore *cs, const PHash *h, uint64_t roots)
{
	DirStore   *ds = (DirStore *) cs;
	bool		created;

	if (roots == 0)
		phmap_remove(&ds->roots, h);
	else
		*phmap_ref(&ds->roots, h, &created) = roots;
}

static void ds_destroy(ChunkStore *cs);

static const ChunkStoreOps dir_ops = {
	ds_get, ds_put, ds_has, ds_meta, ds_del, ds_foreach,
	ds_count, ds_bytes, ds_destroy, ds_note_root
};

/* ------------------------------------------------------------ chunk files */

static size_t
new_file_slot(DirStore *ds)
{
	for (size_t i = 0; i < ds->nfiles; i++)
		if (!ds->files[i].used && ds->files[i].map == NULL)
			return i;
	px_grow((void **) &ds->files, &ds->fcap, ds->nfiles + 1, sizeof(DFile));
	memset(&ds->files[ds->nfiles], 0, sizeof(DFile));
	return ds->nfiles++;
}

typedef struct IdxEnt
{
	PHash		h;
	uint64_t	off;
	uint32_t	len;
} IdxEnt;

static int
idx_cmp(const void *a, const void *b)
{
	return memcmp(((const IdxEnt *) a)->h.b, ((const IdxEnt *) b)->h.b, PHASH_LEN);
}

static int
map_file(DirStore *ds, size_t slot, uint64_t id)
{
	char		name[64],
				path[1024];
	int			fd;
	struct stat st;
	DFile	   *f = &ds->files[slot];

	file_name(name, sizeof(name), id);
	snprintf(path, sizeof(path), "%s/%s", ds->dir, name);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	if (fstat(fd, &st) != 0 || st.st_size < FOOTER_LEN)
	{
		close(fd);
		return -1;
	}
	f->map = mmap(NULL, (size_t) st.st_size, PROT_READ, MAP_SHARED, fd, 0);
	close(fd);
	if (f->map == MAP_FAILED)
	{
		f->map = NULL;
		return -1;
	}
	f->size = (size_t) st.st_size;
	f->id = id;
	f->used = true;
	f->live_bytes = f->dead_bytes = 0;
	return 0;
}

static void
unmap_file(DFile *f)
{
	if (f->map)
		munmap(f->map, f->size);
	f->map = NULL;
	f->used = false;
}

/*
 * Writes the given chunks to a new immutable file and points their entries
 * at it.  Returns false at a crash point (the file left unrenamed).
 */
static bool
write_chunk_file(DirStore *ds, DEntry **ents, size_t n, int crash_point)
{
	Buf			b = BUF_INIT;
	IdxEnt	   *idx = px_malloc(n * sizeof(IdxEnt));
	uint64_t	id = ds->next_id++;
	uint8_t		tmp[INDEX_ENT];
	size_t		index_off,
				slot;
	PHash		ih;
	char		name[64];

	for (size_t i = 0; i < n; i++)
	{
		DEntry	   *e = ents[i];
		const uint8_t *data = e->file == IN_MEM ? e->mem : ds->files[e->file].map + e->off;

		put_be32(tmp, e->len);
		buf_append(&b, tmp, 4);
		buf_append(&b, e->h.b, PHASH_LEN);
		idx[i].h = e->h;
		idx[i].off = b.len;
		idx[i].len = e->len;
		buf_append(&b, data, e->len);
	}
	index_off = b.len;
	if (n > 1)
		qsort(idx, n, sizeof(IdxEnt), idx_cmp);
	for (size_t i = 0; i < n; i++)
	{
		memcpy(tmp, idx[i].h.b, PHASH_LEN);
		put_be64(tmp + PHASH_LEN, idx[i].off);
		put_be32(tmp + PHASH_LEN + 8, idx[i].len);
		buf_append(&b, tmp, INDEX_ENT);
	}
	phash_compute(b.data + index_off, b.len - index_off, &ih);
	{
		uint8_t		foot[FOOTER_LEN];

		memcpy(foot, FILE_MAGIC, 8);
		put_be32(foot + 8, DS_FORMAT);
		put_be64(foot + 12, n);
		put_be64(foot + 20, index_off);
		put_be64(foot + 28, index_off);
		memcpy(foot + 36, ih.b, PHASH_LEN);
		buf_append(&b, foot, FOOTER_LEN);
	}
	free(idx);
	file_name(name, sizeof(name), id);
	if (!write_file_atomic(ds->dir, name, b.data, b.len,
						   crash_point > 0 && ds->failpoint == crash_point))
	{
		buf_free(&b);
		return false;
	}
	buf_free(&b);

	slot = new_file_slot(ds);
	if (map_file(ds, slot, id) != 0)
		die("cannot map new chunk file", name);
	/* entries now live in the new file */
	{
		const uint8_t *p = ds->files[slot].map;

		for (size_t i = 0; i < n; i++)
		{
			DEntry	   *e = ents[i];
			uint64_t	off = (uint64_t) (p - ds->files[slot].map) + REC_HDR;

			if (e->file == IN_MEM)
				free(e->mem);
			else
			{
				ds->files[e->file].live_bytes -= REC_HDR + e->len;
				ds->files[e->file].dead_bytes += REC_HDR + e->len;
			}
			e->mem = NULL;
			e->file = (uint32_t) slot;
			e->off = off;
			ds->files[slot].live_bytes += REC_HDR + e->len;
			p += REC_HDR + e->len;
		}
	}
	return true;
}

/* -------------------------------------------------------------- manifest */

static bool
write_manifest(DirStore *ds, int crash_point)
{
	Buf			b = BUF_INIT;
	uint8_t		tmp[PHASH_LEN + 8];
	uint32_t	nf = 0;
	PHash		sum;
	bool		ok;

	buf_append(&b, MANIFEST_MAGIC, 8);
	put_be32(tmp, DS_FORMAT);
	buf_append(&b, tmp, 4);
	put_be64(tmp, ds->next_id);
	buf_append(&b, tmp, 8);
	for (size_t i = 0; i < ds->nfiles; i++)
		nf += ds->files[i].used;
	put_be32(tmp, nf);
	buf_append(&b, tmp, 4);
	for (size_t i = 0; i < ds->nfiles; i++)
		if (ds->files[i].used)
		{
			put_be64(tmp, ds->files[i].id);
			buf_append(&b, tmp, 8);
		}
	put_be64(tmp, ds->roots.n);
	buf_append(&b, tmp, 8);
	for (size_t i = 0; i < ds->roots.cap; i++)
		if (ds->roots.used[i])
		{
			memcpy(tmp, ds->roots.keys[i].b, PHASH_LEN);
			put_be64(tmp + PHASH_LEN, ds->roots.vals[i]);
			buf_append(&b, tmp, PHASH_LEN + 8);
		}
	buf_append_byte(&b, ds->has_state);
	if (ds->has_state)
		buf_append(&b, ds->state.b, PHASH_LEN);
	phash_compute(b.data, b.len, &sum);
	buf_append(&b, sum.b, PHASH_LEN);
	ok = write_file_atomic(ds->dir, "MANIFEST", b.data, b.len,
						   crash_point > 0 && ds->failpoint == crash_point);
	buf_free(&b);
	return ok;
}

/* ------------------------------------------------------------------- flush */

int
dirstore_flush(ChunkStore *cs)
{
	DirStore   *ds = (DirStore *) cs;
	DEntry	  **ents = NULL;
	size_t		n = 0,
				cap = 0,
				bytes = 0,
				start = 0;

	for (size_t i = 0; i < ds->cap; i++)
		if (ds->tab[i].state == SLOT_USED && ds->tab[i].file == IN_MEM)
		{
			px_grow((void **) &ents, &cap, n + 1, sizeof(DEntry *));
			ents[n++] = &ds->tab[i];
		}
	/* one file per file_target bytes */
	for (size_t i = 0; i <= n; i++)
	{
		if (i == n || (bytes > 0 && bytes + ents[i]->len > ds->file_target))
		{
			if (i > start &&
				!write_chunk_file(ds, ents + start, i - start, DS_FP_FLUSH_BEFORE_RENAME))
			{
				free(ents);
				return DS_CRASHED;
			}
			start = i;
			bytes = 0;
		}
		if (i < n)
			bytes += REC_HDR + ents[i]->len;
	}
	free(ents);
	if (ds->failpoint == DS_FP_FLUSH_BEFORE_MANIFEST)
		return DS_CRASHED;
	if (!write_manifest(ds, -1))
		return DS_CRASHED;
	return CS_OK;
}

/* ------------------------------------------------------------- compaction */

uint64_t
dirstore_disk_bytes(ChunkStore *cs)
{
	DirStore   *ds = (DirStore *) cs;
	DIR		   *d = opendir(ds->dir);
	struct dirent *de;
	uint64_t	total = 0;

	if (d == NULL)
		die("cannot open", ds->dir);
	while ((de = readdir(d)) != NULL)
	{
		char		path[1024];
		struct stat st;

		if (de->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "%s/%s", ds->dir, de->d_name);
		if (stat(path, &st) == 0 && S_ISREG(st.st_mode))
			total += (uint64_t) st.st_blocks * 512;
	}
	closedir(d);
	return total;
}

size_t
dirstore_file_count(ChunkStore *cs)
{
	DirStore   *ds = (DirStore *) cs;
	size_t		n = 0;

	for (size_t i = 0; i < ds->nfiles; i++)
		n += ds->files[i].used;
	return n;
}

static int
ent_order(const void *a, const void *b)
{
	const DEntry *x = *(const DEntry *const *) a,
			   *y = *(const DEntry *const *) b;

	if (x->file != y->file)
		return x->file < y->file ? -1 : 1;
	return (x->off > y->off) - (x->off < y->off);
}

int
dirstore_compact(ChunkStore *cs, double min_dead_fraction, DirCompactStats *st)
{
	DirStore   *ds = (DirStore *) cs;
	bool	   *victim;
	size_t		nvict = 0,
				nsmall = 0;
	DEntry	  **ents = NULL;
	size_t		n = 0,
				cap = 0,
				bytes = 0,
				start = 0,
				nf0;
	int			rc;

	memset(st, 0, sizeof(*st));
	if ((rc = dirstore_flush(cs)) != CS_OK)
		return rc;
	st->files_before = dirstore_file_count(cs);
	st->disk_before = dirstore_disk_bytes(cs);
	/* only files that exist now can be victims; writing adds new slots */
	nf0 = ds->nfiles;
	victim = calloc(nf0 + 1, sizeof(bool));
	for (size_t i = 0; i < nf0; i++)
		if (ds->files[i].used && ds->files[i].size < ds->file_target / 4)
			nsmall++;
	for (size_t i = 0; i < nf0; i++)
	{
		DFile	   *f = &ds->files[i];
		uint64_t	total = f->live_bytes + f->dead_bytes;

		if (!f->used)
			continue;
		if ((f->dead_bytes > 0 && total > 0 &&
			 (double) f->dead_bytes >= min_dead_fraction * (double) total) ||
			(nsmall > 1 && f->size < ds->file_target / 4))
		{
			victim[i] = true;
			nvict++;
		}
	}
	if (nvict == 0)
	{
		free(victim);
		st->files_after = st->files_before;
		st->disk_after = st->disk_before;
		return CS_OK;
	}
	for (size_t i = 0; i < ds->cap; i++)
	{
		DEntry	   *e = &ds->tab[i];

		if (e->state == SLOT_USED && e->file != IN_MEM && e->file < nf0 && victim[e->file])
		{
			px_grow((void **) &ents, &cap, n + 1, sizeof(DEntry *));
			ents[n++] = e;
			st->bytes_copied += e->len;
			st->chunks_copied++;
		}
	}
	/* keep the old order: chunks written together stay together */
	if (n > 1)
		qsort(ents, n, sizeof(DEntry *), ent_order);
	for (size_t i = 0; i <= n; i++)
	{
		if (i == n || (bytes > 0 && bytes + ents[i]->len > ds->file_target))
		{
			if (i > start &&
				!write_chunk_file(ds, ents + start, i - start, DS_FP_COMPACT_BEFORE_RENAME))
			{
				free(ents);
				free(victim);
				return DS_CRASHED;
			}
			start = i;
			bytes = 0;
		}
		if (i < n)
			bytes += REC_HDR + ents[i]->len;
	}
	free(ents);
	if (ds->failpoint == DS_FP_COMPACT_BEFORE_MANIFEST)
	{
		free(victim);
		return DS_CRASHED;
	}
	/* the manifest no longer names the old files; then they go */
	for (size_t i = 0; i < nf0; i++)
		if (victim[i])
			ds->files[i].used = false;
	if (!write_manifest(ds, -1))
	{
		free(victim);
		return DS_CRASHED;
	}
	if (ds->failpoint == DS_FP_COMPACT_BEFORE_UNLINK)
	{
		free(victim);
		return DS_CRASHED;
	}
	for (size_t i = 0; i < nf0; i++)
		if (victim[i])
		{
			char		name[64],
						path[1024];

			file_name(name, sizeof(name), ds->files[i].id);
			snprintf(path, sizeof(path), "%s/%s", ds->dir, name);
			ds->files[i].used = true;	/* for unmap_file's bookkeeping */
			unmap_file(&ds->files[i]);
			if (unlink(path) != 0)
				die("cannot remove", path);
		}
	fsync_dir(ds->dir);
	free(victim);
	st->files_after = dirstore_file_count(cs);
	st->disk_after = dirstore_disk_bytes(cs);
	return CS_OK;
}

/* -------------------------------------------------------------------- open */

static bool
load_file(DirStore *ds, uint64_t id, char *err, size_t errlen)
{
	size_t		slot = new_file_slot(ds);
	DFile	   *f;
	const uint8_t *foot;
	uint64_t	n,
				index_off;
	PHash		ih;
	char		name[64];

	file_name(name, sizeof(name), id);
	if (map_file(ds, slot, id) != 0)
	{
		snprintf(err, errlen, "chunk file %s is missing or unreadable", name);
		return false;
	}
	f = &ds->files[slot];
	foot = f->map + f->size - FOOTER_LEN;
	if (memcmp(foot, FILE_MAGIC, 8) != 0 || get_be32(foot + 8) != DS_FORMAT)
	{
		snprintf(err, errlen, "chunk file %s has a bad footer", name);
		return false;
	}
	n = get_be64(foot + 12);
	index_off = get_be64(foot + 20);
	if (index_off + n * INDEX_ENT + FOOTER_LEN != f->size)
	{
		snprintf(err, errlen, "chunk file %s has a bad index size", name);
		return false;
	}
	phash_compute(f->map + index_off, n * INDEX_ENT, &ih);
	if (memcmp(ih.b, foot + 36, PHASH_LEN) != 0)
	{
		snprintf(err, errlen, "chunk file %s has a corrupt index", name);
		return false;
	}
	for (uint64_t i = 0; i < n; i++)
	{
		const uint8_t *ie = f->map + index_off + i * INDEX_ENT;
		PHash		h;
		uint64_t	off = get_be64(ie + PHASH_LEN);
		uint32_t	len = get_be32(ie + PHASH_LEN + 8);
		DEntry	   *e;

		memcpy(h.b, ie, PHASH_LEN);
		if (off < REC_HDR || off + len > index_off ||
			memcmp(f->map + off - PHASH_LEN, h.b, PHASH_LEN) != 0)
		{
			snprintf(err, errlen, "chunk file %s has a bad index entry", name);
			return false;
		}
		if (ds_find(ds, &h))
		{
			f->dead_bytes += REC_HDR + len;	/* a second copy */
			continue;
		}
		e = ds_insert(ds, &h);
		e->file = (uint32_t) slot;
		e->off = off;
		e->len = len;
		f->live_bytes += REC_HDR + len;
		ds->live += len;
	}
	return true;
}

/* Deletes files the manifest does not name: crash leftovers. */
static void
remove_strays(DirStore *ds)
{
	DIR		   *d = opendir(ds->dir);
	struct dirent *de;

	if (d == NULL)
		die("cannot open", ds->dir);
	while ((de = readdir(d)) != NULL)
	{
		const char *nm = de->d_name;
		size_t		len = strlen(nm);
		bool		stray = false;

		if (len > 4 && strcmp(nm + len - 4, ".tmp") == 0)
			stray = true;
		else if (strncmp(nm, "c-", 2) == 0 && len > 7 && strcmp(nm + len - 7, ".chunks") == 0)
		{
			uint64_t	id = strtoull(nm + 2, NULL, 16);

			stray = true;
			for (size_t i = 0; i < ds->nfiles; i++)
				if (ds->files[i].used && ds->files[i].id == id)
					stray = false;
		}
		if (stray)
		{
			char		path[1024];

			snprintf(path, sizeof(path), "%s/%s", ds->dir, nm);
			if (unlink(path) != 0)
				die("cannot remove", path);
		}
	}
	closedir(d);
	fsync_dir(ds->dir);
}

typedef struct Mark
{
	DirStore   *ds;
	PHashMap   *seen;
	PHash	   *stack;
	size_t		n,
				cap;
	bool		ok;
	PHash		missing;
	bool		count;			/* second pass: count edges */
} Mark;

static void
mark_fn(void *arg, const PHash *h, bool strong)
{
	Mark	   *m = arg;
	DEntry	   *e;
	bool		created;

	if (!strong)
		return;
	e = ds_find(m->ds, h);
	if (e == NULL)
	{
		if (m->ok)
			m->missing = *h;
		m->ok = false;
		return;
	}
	if (m->count)
	{
		e->meta.refs++;
		return;
	}
	phmap_ref(m->seen, h, &created);
	if (created)
	{
		px_grow((void **) &m->stack, &m->cap, m->n + 1, sizeof(PHash));
		m->stack[m->n++] = *h;
	}
}

/*
 * The chunks the roots reach are live, with reference counts equal to the
 * strong edges among them; everything else in the files is dead.
 */
static bool
rebuild_holds(DirStore *ds, char *err, size_t errlen)
{
	PHashMap	seen;
	Mark		m;
	bool		created;
	char		hex[PHASH_HEX_LEN + 1];

	phmap_init(&seen);
	memset(&m, 0, sizeof(m));
	m.ds = ds;
	m.seen = &seen;
	m.ok = true;
	for (size_t i = 0; i < ds->roots.cap; i++)
	{
		DEntry	   *e;

		if (!ds->roots.used[i])
			continue;
		e = ds_find(ds, &ds->roots.keys[i]);
		if (e == NULL)
		{
			phash_hex(&ds->roots.keys[i], hex);
			snprintf(err, errlen, "root %s is not in any chunk file", hex);
			phmap_free(&seen);
			return false;
		}
		e->meta.roots = ds->roots.vals[i];
		phmap_ref(&seen, &e->h, &created);
		if (created)
		{
			px_grow((void **) &m.stack, &m.cap, m.n + 1, sizeof(PHash));
			m.stack[m.n++] = e->h;
		}
	}
	while (m.n > 0 && m.ok)
	{
		PHash		h = m.stack[--m.n];
		size_t		len = 0;
		const uint8_t *d = ds_get(&ds->base, &h, &len);

		if (!chunk_refs(d, len, mark_fn, &m))
		{
			phash_hex(&h, hex);
			snprintf(err, errlen, "chunk %s does not parse", hex);
			m.ok = false;
		}
	}
	if (!m.ok && err[0] == '\0')
	{
		phash_hex(&m.missing, hex);
		snprintf(err, errlen, "a live chunk refers to missing chunk %s", hex);
	}
	if (m.ok)
	{
		/* drop the unreachable, then count edges among the reachable */
		for (size_t i = 0; i < ds->cap; i++)
			if (ds->tab[i].state == SLOT_USED && !phmap_find(&seen, &ds->tab[i].h))
				ds_remove(ds, &ds->tab[i]);
		m.count = true;
		for (size_t i = 0; i < ds->cap && m.ok; i++)
			if (ds->tab[i].state == SLOT_USED)
			{
				size_t		len = 0;
				const uint8_t *d = ds_get(&ds->base, &ds->tab[i].h, &len);

				chunk_refs(d, len, mark_fn, &m);
			}
	}
	free(m.stack);
	phmap_free(&seen);
	return m.ok;
}

ChunkStore *
dirstore_open(const char *dir, char *err, size_t errlen)
{
	DirStore   *ds = px_malloc(sizeof(DirStore));
	char		path[1024];
	int			fd;
	struct stat st;

	err[0] = '\0';
	memset(ds, 0, sizeof(*ds));
	ds->base.ops = &dir_ops;
	ds->dir = px_memdup(dir, strlen(dir) + 1);
	ds->cap = 1 << 12;
	ds->tab = calloc(ds->cap, sizeof(DEntry));
	ds->file_target = 64 << 20;
	ds->next_id = 1;
	ds->failpoint = -1;
	phmap_init(&ds->roots);
	if (ds->tab == NULL)
		abort();
	if (mkdir(dir, 0700) != 0 && errno != EEXIST)
		die("cannot create", dir);

	snprintf(path, sizeof(path), "%s/MANIFEST", dir);
	fd = open(path, O_RDONLY);
	if (fd >= 0)
	{
		uint8_t    *m;
		const uint8_t *p,
				   *end;
		PHash		sum;
		uint32_t	nf;
		uint64_t	nr;

		if (fstat(fd, &st) != 0)
			die("cannot stat", path);
		m = px_malloc((size_t) st.st_size + 1);
		if (read(fd, m, (size_t) st.st_size) != st.st_size)
			die("cannot read", path);
		close(fd);
		end = m + st.st_size;
		phash_compute(m, (size_t) st.st_size - PHASH_LEN, &sum);
		if (st.st_size < 8 + 4 + 8 + 4 + 8 + 1 + PHASH_LEN ||
			memcmp(m, MANIFEST_MAGIC, 8) != 0 || get_be32(m + 8) != DS_FORMAT ||
			memcmp(sum.b, end - PHASH_LEN, PHASH_LEN) != 0)
		{
			snprintf(err, errlen, "MANIFEST is corrupt or of another format");
			free(m);
			goto fail;
		}
		ds->next_id = get_be64(m + 12);
		nf = get_be32(m + 20);
		p = m + 24;
		for (uint32_t i = 0; i < nf; i++, p += 8)
			if (!load_file(ds, get_be64(p), err, errlen))
			{
				free(m);
				goto fail;
			}
		nr = get_be64(p);
		p += 8;
		for (uint64_t i = 0; i < nr; i++, p += PHASH_LEN + 8)
		{
			PHash		h;
			bool		created;

			memcpy(h.b, p, PHASH_LEN);
			*phmap_ref(&ds->roots, &h, &created) = get_be64(p + PHASH_LEN);
		}
		ds->has_state = *p++ != 0;
		if (ds->has_state)
			memcpy(ds->state.b, p, PHASH_LEN);
		free(m);
	}
	remove_strays(ds);
	if (!rebuild_holds(ds, err, errlen))
		goto fail;
	return &ds->base;

fail:
	dirstore_crash(&ds->base);
	return NULL;
}

/* Scrub: every live chunk's bytes must hash to its name.  O(data). */
bool
dirstore_verify(ChunkStore *cs, PHash *bad)
{
	DirStore   *ds = (DirStore *) cs;

	for (size_t i = 0; i < ds->cap; i++)
		if (ds->tab[i].state == SLOT_USED)
		{
			size_t		len = 0;
			const uint8_t *d = ds_get(cs, &ds->tab[i].h, &len);
			PHash		h;

			phash_compute(d, len, &h);
			if (!phash_eq(&h, &ds->tab[i].h))
			{
				*bad = ds->tab[i].h;
				return false;
			}
		}
	return true;
}

void
dirstore_set_state(ChunkStore *cs, const PHash *state)
{
	DirStore   *ds = (DirStore *) cs;

	ds->has_state = state != NULL;
	if (state)
		ds->state = *state;
}

bool
dirstore_get_state(ChunkStore *cs, PHash *state)
{
	DirStore   *ds = (DirStore *) cs;

	if (ds->has_state)
		*state = ds->state;
	return ds->has_state;
}

void
dirstore_set_failpoint(ChunkStore *cs, int failpoint)
{
	((DirStore *) cs)->failpoint = failpoint;
}

void
dirstore_set_file_target(ChunkStore *cs, size_t bytes)
{
	((DirStore *) cs)->file_target = bytes;
}

/* Lets go of everything without writing: what a crash leaves. */
void
dirstore_crash(ChunkStore *cs)
{
	DirStore   *ds = (DirStore *) cs;

	for (size_t i = 0; i < ds->nfiles; i++)
		if (ds->files[i].map)
			munmap(ds->files[i].map, ds->files[i].size);
	for (size_t i = 0; i < ds->cap; i++)
		if (ds->tab[i].state == SLOT_USED && ds->tab[i].file == IN_MEM)
			free(ds->tab[i].mem);
	free(ds->files);
	free(ds->tab);
	free(ds->dir);
	phmap_free(&ds->roots);
	free(ds);
}

static void
ds_destroy(ChunkStore *cs)
{
	DirStore   *ds = (DirStore *) cs;

	ds->failpoint = -1;
	if (dirstore_flush(cs) != CS_OK)
		abort();
	dirstore_crash(cs);
}
