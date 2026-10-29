/*
 * test_dirstore.c
 *	  The durable chunk store: immutable chunk files and a manifest.
 *	  Round trips through reopening, files never changing once written,
 *	  corruption detected, repo state saved and reopened, every crash point
 *	  recovered from, and severed history giving its disk space back after
 *	  compaction.
 */
#define _DEFAULT_SOURCE
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "test_repo.h"

/* ---------------------------------------------------------------- helpers */

static char *
tmpdir(void)
{
	const char *base = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
	char	   *d = px_malloc(strlen(base) + 32);

	sprintf(d, "%s/prolly-ds-XXXXXX", base);
	CHECK(mkdtemp(d) != NULL);
	return d;
}

/* Removes the files the store writes, then the directory itself. */
static void
cleanup(char *dir)
{
	DIR		   *d = opendir(dir);
	struct dirent *de;

	CHECK(d != NULL);
	while ((de = readdir(d)) != NULL)
	{
		char		path[1024];

		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;
		CHECK(strncmp(de->d_name, "c-", 2) == 0 || strncmp(de->d_name, "j-", 2) == 0 ||
			  strcmp(de->d_name, "MANIFEST") == 0 || strstr(de->d_name, ".tmp") != NULL);
		snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
		CHECK(unlink(path) == 0);
	}
	closedir(d);
	CHECK(rmdir(dir) == 0);
	free(dir);
}

static ChunkStore *
reopen(const char *dir)
{
	char		err[256];
	ChunkStore *cs = dirstore_open(dir, err, sizeof(err));

	if (cs == NULL)
	{
		fprintf(stderr, "\nreopen failed: %s\n", err);
		exit(1);
	}
	return cs;
}

/* Every file in the directory, by name. */
typedef struct Listing
{
	char		name[16][256];
	PHash		sum[16];
	struct timespec mtime[16];
	int			n;
} Listing;

static void
list_files(const char *dir, Listing *l)
{
	DIR		   *d = opendir(dir);
	struct dirent *de;

	l->n = 0;
	while ((de = readdir(d)) != NULL)
	{
		char		path[1024];
		struct stat st;
		FILE	   *f;
		uint8_t    *buf;

		if (strncmp(de->d_name, "c-", 2) != 0 && strcmp(de->d_name, "MANIFEST") != 0)
			continue;
		CHECK(l->n < 16);
		snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
		CHECK(stat(path, &st) == 0);
		buf = px_malloc((size_t) st.st_size + 1);
		f = fopen(path, "rb");
		CHECK(f && fread(buf, 1, (size_t) st.st_size, f) == (size_t) st.st_size);
		fclose(f);
		snprintf(l->name[l->n], 256, "%s", de->d_name);
		phash_compute(buf, (size_t) st.st_size, &l->sum[l->n]);
		l->mtime[l->n] = st.st_mtim;
		l->n++;
		free(buf);
	}
	closedir(d);
}

static int
find_file(const Listing *l, const char *name)
{
	for (int i = 0; i < l->n; i++)
		if (strcmp(l->name[i], name) == 0)
			return i;
	return -1;
}

/* A row's version in the current branch's working set, or -1. */
static int64_t
working_row_t(VcRepo *r, uint64_t key)
{
	PHash		rv = vc_working(r);
	VcTable		t;
	uint8_t		k[8];
	const uint8_t *v;
	size_t		vl;

	CHECK(vc_table_get(&r->ctx, &rv, "t", &t));
	put_u64(k, key);
	if (!t_get(&r->ctx, &t.data, k, 8, &v, &vl))
		return -1;
	return (int64_t) get_u64(v + 8);
}

static void
verify_store(ChunkStore *cs)
{
	PHash		bad;

	CHECK(dirstore_verify(cs, &bad));
	check_store(cs, 0);
}

/* ------------------------------------------------------------------ tests */

static void
test_round_trip(void)
{
	char	   *dir = tmpdir();
	ChunkStore *cs = reopen(dir);
	ProllyCtx	ctx;
	PHash		root;
	size_t		chunks;
	uint64_t	live;

	prolly_ctx_init(&ctx, cs, &small_policy);
	root = build_even_rows(&ctx, 20000);
	cs_ref_root(cs, &root);
	prolly_release_pins(&ctx);
	chunks = cs->ops->count(cs);
	live = cs->ops->bytes(cs);
	CHECK_OK(dirstore_flush(cs));
	prolly_ctx_free(&ctx);
	cs_destroy(cs);

	cs = reopen(dir);
	prolly_ctx_init(&ctx, cs, &small_policy);
	CHECK_EQ(cs->ops->count(cs), chunks);
	CHECK_EQ(cs->ops->bytes(cs), live);
	CHECK_EQ(t_count(&ctx, &root), 20000);
	{
		PHash		again = build_even_rows(&ctx, 20000);	/* same rows, same tree */

		CHECK(phash_eq(&again, &root));
		prolly_release_pins(&ctx);
	}
	verify_store(cs);
	prolly_ctx_free(&ctx);
	cs_destroy(cs);
	cleanup(dir);
}

/* Unflushed work is gone after a crash; garbage does not come back. */
static void
test_unflushed_lost_garbage_dropped(void)
{
	char	   *dir = tmpdir();
	ChunkStore *cs = reopen(dir);
	ProllyCtx	ctx;
	PHash		kept,
				dropped,
				lost;

	prolly_ctx_init(&ctx, cs, &small_policy);
	kept = build_even_rows(&ctx, 5000);
	cs_ref_root(cs, &kept);
	dropped = build_even_rows(&ctx, 7000);
	cs_ref_root(cs, &dropped);
	prolly_release_pins(&ctx);
	CHECK_OK(dirstore_flush(cs));
	cs_unref_root(cs, &dropped);	/* freed, but its bytes are in a file */
	lost = build_even_rows(&ctx, 9000);
	cs_ref_root(cs, &lost);			/* never flushed */
	prolly_release_pins(&ctx);
	prolly_ctx_free(&ctx);
	dirstore_crash(cs);

	cs = reopen(dir);
	CHECK(cs->ops->has(cs, &kept));
	CHECK(!cs->ops->has(cs, &lost));
	/* the manifest still names dropped as a root: the free was not flushed */
	CHECK(cs->ops->has(cs, &dropped));
	cs_unref_root(cs, &dropped);
	CHECK_OK(dirstore_flush(cs));
	cs_destroy(cs);

	cs = reopen(dir);
	CHECK(!cs->ops->has(cs, &dropped));
	CHECK(cs->ops->has(cs, &kept));
	verify_store(cs);
	cs_destroy(cs);
	cleanup(dir);
}

/* A chunk file never changes after it is written; they are only added or deleted. */
static void
test_files_are_immutable(void)
{
	char	   *dir = tmpdir();
	ChunkStore *cs = reopen(dir);
	ProllyCtx	ctx;
	Listing		first,
				later;
	PHash		roots[6];
	DirCompactStats st;
	int			seen_deleted = 0;

	prolly_ctx_init(&ctx, cs, &small_policy);
	dirstore_set_file_target(cs, 64 << 10);
	for (int i = 0; i < 3; i++)
	{
		roots[i] = build_even_rows(&ctx, 3000 + 2000 * (uint64_t) i);
		cs_ref_root(cs, &roots[i]);
		prolly_release_pins(&ctx);
		CHECK_OK(dirstore_flush(cs));
	}
	/* flushes go to the journal; compaction writes the chunk files */
	CHECK_EQ(dirstore_file_count(cs), 0);
	CHECK_OK(dirstore_compact(cs, 0.0, &st));
	list_files(dir, &first);
	CHECK(first.n >= 4);
	usleep(20000);				/* so a rewrite would show in the mtime */

	for (int i = 3; i < 6; i++)
	{
		EditBatch	b;

		batch_init(&b, 300);
		for (uint64_t k = 0; k < 300; k++)
			batch_put(&b, k * 7, (uint64_t) i);
		roots[i] = t_apply(&ctx, &roots[i - 3], b.e, b.n);
		cs_ref_root(cs, &roots[i]);
		batch_free(&b);
		prolly_release_pins(&ctx);
		CHECK_OK(dirstore_flush(cs));
	}
	cs_unref_root(cs, &roots[0]);
	cs_unref_root(cs, &roots[1]);
	/* any dead space: the three roots share most chunks, packed together */
	CHECK_OK(dirstore_compact(cs, 0.0, &st));
	CHECK(st.files_after > 0);
	list_files(dir, &later);
	for (int i = 0; i < first.n; i++)
	{
		int			j;

		if (strcmp(first.name[i], "MANIFEST") == 0)
			continue;			/* replaced whole by rename, never edited */
		j = find_file(&later, first.name[i]);
		if (j < 0)
		{
			seen_deleted++;
			continue;
		}
		CHECK(phash_eq(&first.sum[i], &later.sum[j]));
		CHECK(first.mtime[i].tv_sec == later.mtime[j].tv_sec &&
			  first.mtime[i].tv_nsec == later.mtime[j].tv_nsec);
	}
	CHECK(seen_deleted > 0);	/* compaction replaced some files */
	verify_store(cs);
	prolly_ctx_free(&ctx);
	cs_destroy(cs);
	cleanup(dir);
}

static void
flip_byte(const char *path, long from_end)
{
	FILE	   *f = fopen(path, "r+b");
	int			c;

	CHECK(f != NULL);
	CHECK(fseek(f, -from_end, SEEK_END) == 0);
	c = fgetc(f);
	CHECK(fseek(f, -from_end, SEEK_END) == 0);
	fputc(c ^ 1, f);
	fclose(f);
}

static void
test_corruption_detected(void)
{
	char	   *dir = tmpdir();
	ChunkStore *cs = reopen(dir);
	ProllyCtx	ctx;
	PHash		root,
				bad;
	Listing		l;
	char		path[1024],
				err[256];
	int			chunk_file = -1;

	prolly_ctx_init(&ctx, cs, &small_policy);
	root = build_even_rows(&ctx, 4000);
	cs_ref_root(cs, &root);
	prolly_release_pins(&ctx);
	prolly_ctx_free(&ctx);
	{
		DirCompactStats st;

		CHECK_OK(dirstore_compact(cs, 0.0, &st));	/* chunk files from the journal */
	}
	cs_destroy(cs);
	list_files(dir, &l);
	for (int i = 0; i < l.n; i++)
		if (strncmp(l.name[i], "c-", 2) == 0)
			chunk_file = i;
	CHECK(chunk_file >= 0);
	snprintf(path, sizeof(path), "%s/%s", dir, l.name[chunk_file]);

	/* the index: the open fails, saying which file */
	flip_byte(path, 56 + 5);
	CHECK(dirstore_open(dir, err, sizeof(err)) == NULL);
	CHECK(strstr(err, "corrupt index") != NULL);
	flip_byte(path, 56 + 5);

	/* the manifest */
	snprintf(path, sizeof(path), "%s/MANIFEST", dir);
	flip_byte(path, 30);
	CHECK(dirstore_open(dir, err, sizeof(err)) == NULL);
	CHECK(strstr(err, "MANIFEST") != NULL);
	flip_byte(path, 30);

	/* chunk bytes: the scrub finds the chunk */
	snprintf(path, sizeof(path), "%s/%s", dir, l.name[chunk_file]);
	{
		struct stat st;
		FILE	   *f;
		int			c;

		CHECK(stat(path, &st) == 0);
		f = fopen(path, "r+b");
		CHECK(fseek(f, 30, SEEK_SET) == 0);	/* inside the first record's bytes */
		c = fgetc(f);
		CHECK(fseek(f, 30, SEEK_SET) == 0);
		fputc(c ^ 1, f);
		fclose(f);
	}
	cs = dirstore_open(dir, err, sizeof(err));
	if (cs)
	{
		CHECK(!dirstore_verify(cs, &bad));
		dirstore_crash(cs);
	}
	else
		CHECK(err[0] != '\0');	/* or the walk from the roots refused it */
	cleanup(dir);
}

static void
test_repo_save_and_reopen(void)
{
	char	   *dir = tmpdir(),
			   *adir = tmpdir();
	ChunkStore *cs = reopen(dir);
	ChunkStore *acs = reopen(adir);
	VcRepo		r;
	Linear		l;
	PHash		state,
				fh,
				heads[3],
				works[3];
	uint32_t	seg;
	LogAcc		before,
				after;
	Snap		sd;

	setup(&r, cs);
	vc_repo_set_name(&r, "db1");
	l = build_linear(&r);
	CHECK(vc_branch_create(&r, "feature", &l.c) == VC_OK);
	vc_checkout(&r, "feature");
	fh = commit_rows(&r, 15000, 15100, 9, "feature work");
	put_rows(&r, 16000, 16010, 10);	/* uncommitted */
	vc_checkout(&r, "main");
	CHECK(vc_window_track(&r, 2) == VC_OK);
	CHECK(vc_sever(&r, &depth2, &seg) == VC_OK);
	CHECK(vc_segment_export(&r, seg, &(VcSource) {"cold", acs}) == VC_OK);
	sd = snap(&r.ctx, &l.d);
	for (size_t i = 0; i < r.nbr; i++)
	{
		heads[i] = r.br[i].head;
		works[i] = r.br[i].working;
	}
	log_from(&r, &fh, 0, &before);
	state = vc_repo_save(&r);
	dirstore_set_state(cs, &state);
	CHECK_OK(dirstore_flush(cs));
	vc_repo_free(&r);
	cs_destroy(cs);
	cs_destroy(acs);

	cs = reopen(dir);
	acs = reopen(adir);
	CHECK(dirstore_get_state(cs, &state));
	CHECK(vc_repo_open(&r, cs, &small_policy, &state) == VC_OK);
	CHECK(strcmp(r.name, "db1") == 0 && r.nbr == 2);
	for (size_t i = 0; i < r.nbr; i++)
	{
		CHECK(phash_eq(&r.br[i].head, &heads[i]));
		CHECK(phash_eq(&r.br[i].working, &works[i]));
	}
	CHECK(vc_locate(&r, &l.b, NULL) == VC_LOC_EXPORTED);
	CHECK(vc_window_verify(&r));
	check_snap(&r.ctx, &l.d, &sd);
	vc_checkout(&r, "feature");
	CHECK(working_row_t(&r, 16005) == 10);	/* uncommitted work kept */
	log_from(&r, &fh, 0, &after);
	CHECK(after.n == before.n);
	for (int i = 0; i < after.n; i++)
		CHECK(phash_eq(&after.v[i], &before.v[i]));
	verify_store(cs);

	/* the exported segment comes back once its archive is named again */
	vc_checkout(&r, "main");
	vc_segment_set_archive(&r, seg, acs);
	CHECK(vc_segment_attach(&r, seg) == VC_OK);
	CHECK(vc_locate(&r, &l.b, NULL) == VC_LOC_WINDOW);
	check_store(cs, 0);

	free(sd.v);
	vc_repo_free(&r);
	cs_destroy(cs);
	cs_destroy(acs);
	cleanup(dir);
	cleanup(adir);
}

/*
 * A crash at each point of flush and compaction: reopening finds the last
 * complete manifest's state, intact, and no file it does not name.
 */
static void
test_crash_points(void)
{
	static const int points[] = {
		DS_FP_JOURNAL_BEFORE_ROOT, DS_FP_JOURNAL_BEFORE_SYNC,
		DS_FP_COMPACT_BEFORE_RENAME, DS_FP_COMPACT_BEFORE_MANIFEST,
		DS_FP_COMPACT_BEFORE_UNLINK
	};

	for (size_t p = 0; p < sizeof(points) / sizeof(points[0]); p++)
	{
		char	   *dir = tmpdir();
		ChunkStore *cs = reopen(dir);
		ProllyCtx	ctx;
		PHash		a,
					b,
					c;
		EditBatch	eb;
		DirCompactStats st;
		Listing		l;
		int			rc;
		bool		compacting = points[p] >= DS_FP_COMPACT_BEFORE_RENAME;

		prolly_ctx_init(&ctx, cs, &small_policy);
		dirstore_set_file_target(cs, 32 << 10);
		a = build_even_rows(&ctx, 6000);
		cs_ref_root(cs, &a);
		prolly_release_pins(&ctx);
		CHECK_OK(dirstore_flush(cs));
		batch_init(&eb, 500);
		for (uint64_t k = 0; k < 500; k++)
			batch_put(&eb, k * 11, 1);
		b = t_apply(&ctx, &a, eb.e, eb.n);
		cs_ref_root(cs, &b);
		prolly_release_pins(&ctx);
		if (compacting)
		{
			CHECK_OK(dirstore_flush(cs));
			cs_unref_root(cs, &a);	/* a's own chunks become garbage */
		}
		dirstore_set_failpoint(cs, points[p]);
		rc = compacting ? dirstore_compact(cs, 0.05, &st) : dirstore_flush(cs);
		CHECK(rc == DS_CRASHED);
		prolly_ctx_free(&ctx);
		dirstore_crash(cs);

		cs = reopen(dir);
		prolly_ctx_init(&ctx, cs, &small_policy);
		verify_store(cs);
		if (compacting)
		{
			/* the free was never in a manifest, but b was: b intact */
			CHECK_EQ(t_count(&ctx, &b), 6250);	/* 250 of the edits are new odd keys */
		}
		else if (points[p] == DS_FP_JOURNAL_BEFORE_ROOT)
		{
			/* no root record: back to a alone, b's chunks unreferenced */
			CHECK(!cs->ops->has(cs, &b));
			CHECK_EQ(t_count(&ctx, &a), 6000);
		}
		else
		{
			/* the root record reached the file (a process crash, not a
			 * power cut: the page cache keeps it), so b is committed */
			CHECK_EQ(t_count(&ctx, &b), 6250);
		}
		/* no file the manifest does not name survives the reopen */
		list_files(dir, &l);
		CHECK_EQ((size_t) l.n, dirstore_file_count(cs) + 1);
		/* and a compaction now completes, giving the garbage back */
		c = compacting ? b : a;
		(void) c;
		CHECK_OK(dirstore_compact(cs, 0.05, &st));
		verify_store(cs);
		batch_free(&eb);
		prolly_ctx_free(&ctx);
		cs_destroy(cs);
		cleanup(dir);
	}
}

/* ---------------------------------------------------------------- journal */

/* The journal's path in dir (there is one). */
static void
journal_path(const char *dir, char *out, size_t n)
{
	DIR		   *d = opendir(dir);
	struct dirent *de;

	out[0] = '\0';
	while ((de = readdir(d)) != NULL)
		if (strncmp(de->d_name, "j-", 2) == 0)
			snprintf(out, n, "%s/%s", dir, de->d_name);
	closedir(d);
	CHECK(out[0] != '\0');
}

static uint8_t *
read_all(const char *path, size_t *len)
{
	struct stat st;
	FILE	   *f = fopen(path, "rb");
	uint8_t    *b;

	CHECK(f != NULL && stat(path, &st) == 0);
	b = px_malloc((size_t) st.st_size + 1);
	CHECK(fread(b, 1, (size_t) st.st_size, f) == (size_t) st.st_size);
	fclose(f);
	*len = (size_t) st.st_size;
	return b;
}

/*
 * Three commits a, b, c, each flushed: their roots held and their state
 * set to their root.  Returns the journal end after each.
 */
static void
three_commits(const char *dir, PHash *roots, uint64_t *ends)
{
	ChunkStore *cs = reopen(dir);
	ProllyCtx	ctx;

	prolly_ctx_init(&ctx, cs, &small_policy);
	for (int i = 0; i < 3; i++)
	{
		EditBatch	b;

		if (i == 0)
			roots[i] = build_even_rows(&ctx, 3000);
		else
		{
			batch_init(&b, 200);
			for (uint64_t k = 0; k < 200; k++)
				batch_put(&b, k * 13 + (uint64_t) i, (uint64_t) i);
			roots[i] = t_apply(&ctx, &roots[i - 1], b.e, b.n);
			batch_free(&b);
		}
		cs_ref_root(cs, &roots[i]);
		if (i > 0)
			cs_unref_root(cs, &roots[i - 1]);
		dirstore_set_state(cs, &roots[i]);
		prolly_release_pins(&ctx);
		CHECK_OK(dirstore_flush(cs));
		ends[i] = dirstore_journal_end(cs);
	}
	prolly_ctx_free(&ctx);
	dirstore_crash(cs);			/* nothing more to write */
}

/* Flushes append to the journal and nothing else; earlier bytes stay. */
static void
test_journal_append_only(void)
{
	char	   *dir = tmpdir();
	PHash		roots[3];
	uint64_t	ends[3];
	char		jp[1024];
	size_t		len1,
				len2;
	uint8_t    *before,
			   *after;
	ChunkStore *cs;
	ProllyCtx	ctx;
	PHash		more;
	PHash		st;

	three_commits(dir, roots, ends);
	journal_path(dir, jp, sizeof(jp));
	before = read_all(jp, &len1);
	CHECK(ends[0] < ends[1] && ends[1] < ends[2] && ends[2] <= len1);
	cs = reopen(dir);
	CHECK_EQ(dirstore_file_count(cs), 0);	/* no chunk files yet */
	CHECK(dirstore_get_state(cs, &st) && phash_eq(&st, &roots[2]));
	prolly_ctx_init(&ctx, cs, &small_policy);
	more = build_even_rows(&ctx, 500);
	cs_ref_root(cs, &more);
	prolly_release_pins(&ctx);
	CHECK_OK(dirstore_flush(cs));
	after = read_all(jp, &len2);
	CHECK(memcmp(before, after, (size_t) ends[2]) == 0);
	CHECK(dirstore_journal_end(cs) > ends[2]);
	verify_store(cs);
	prolly_ctx_free(&ctx);
	cs_destroy(cs);
	free(before);
	free(after);
	cleanup(dir);
}

/*
 * A power cut loses whatever was not synced: the journal cut anywhere
 * after commit a's end opens as the last commit wholly before the cut,
 * with every chunk of it there and the store consistent.
 */
static void
test_journal_torn_tail(void)
{
	char	   *dir = tmpdir();
	PHash		roots[3];
	uint64_t	ends[3];
	char		jp[1024];
	size_t		len;
	uint8_t    *orig;
	int			cuts = 0;

	three_commits(dir, roots, ends);
	journal_path(dir, jp, sizeof(jp));
	orig = read_all(jp, &len);
	for (uint64_t cut = ends[0]; cut <= ends[2]; cut += 1 + (ends[2] - ends[0]) / 97)
	{
		uint64_t	at = cut;
		ChunkStore *cs;
		ProllyCtx	ctx;
		PHash		st;
		int			want = cut >= ends[2] ? 2 : cut >= ends[1] ? 1 : 0;
		FILE	   *f;

		for (int pass = 0; pass < 2; pass++)
		{
			/* the cut, then (second pass) the same cut torn mid-record */
			f = fopen(jp, "wb");
			CHECK(f && fwrite(orig, 1, (size_t) at, f) == (size_t) at);
			if (pass == 1)
			{
				static const uint8_t junk[7] = {0, 0, 0x40, 0, 'C', 1, 2};

				fwrite(junk, 1, sizeof(junk), f);	/* a header that runs past the end */
			}
			fclose(f);
			cs = reopen(dir);
			CHECK(dirstore_get_state(cs, &st) && phash_eq(&st, &roots[want]));
			prolly_ctx_init(&ctx, cs, &small_policy);
			CHECK_EQ(t_count(&ctx, &roots[want]), 3000 + (want >= 1 ? 100 : 0) + (want >= 2 ? 100 : 0));
			verify_store(cs);
			CHECK(dirstore_journal_end(cs) <= at);
			prolly_ctx_free(&ctx);
			dirstore_crash(cs);
			cuts++;
		}
	}
	CHECK(cuts > 100);
	free(orig);
	cleanup(dir);
}

/* Damage before a valid root record is not a torn tail: the open refuses. */
static void
test_journal_mid_log_damage(void)
{
	char	   *dir = tmpdir();
	PHash		roots[3];
	uint64_t	ends[3];
	char		jp[1024],
				err[256];
	FILE	   *f;
	int			c;
	ChunkStore *cs;
	PHash		st;

	three_commits(dir, roots, ends);
	journal_path(dir, jp, sizeof(jp));
	f = fopen(jp, "r+b");
	CHECK(fseek(f, 40, SEEK_SET) == 0);	/* inside commit a's first chunk */
	c = fgetc(f);
	CHECK(fseek(f, 40, SEEK_SET) == 0);
	fputc(c ^ 1, f);
	fclose(f);
	CHECK(dirstore_open(dir, err, sizeof(err)) == NULL);
	CHECK(strstr(err, "not a torn tail") != NULL);

	/* the same damage in the last commit's records is a torn tail */
	f = fopen(jp, "r+b");
	CHECK(fseek(f, 40, SEEK_SET) == 0);
	fputc(c, f);
	CHECK(fseek(f, (long) ends[2] - 9, SEEK_SET) == 0);	/* in c's root record */
	c = fgetc(f);
	CHECK(fseek(f, (long) ends[2] - 9, SEEK_SET) == 0);
	fputc(c ^ 1, f);
	fclose(f);
	cs = reopen(dir);
	CHECK(dirstore_get_state(cs, &st) && phash_eq(&st, &roots[1]));
	verify_store(cs);
	dirstore_crash(cs);
	cleanup(dir);
}

/*
 * Opening as of an earlier root record: what is after it is cut away, and
 * the store carries on from there.  An unknown state is refused.
 */
static void
test_journal_open_at(void)
{
	char	   *dir = tmpdir();
	PHash		roots[3],
				st,
				nowhere;
	uint64_t	ends[3];
	char		err[256];
	ChunkStore *cs;
	ProllyCtx	ctx;
	DirCompactStats cst;

	three_commits(dir, roots, ends);
	memset(nowhere.b, 7, PHASH_LEN);
	CHECK(dirstore_open_at(dir, &nowhere, err, sizeof(err)) == NULL);
	CHECK(strstr(err, "no root record") != NULL);

	cs = dirstore_open_at(dir, &roots[0], err, sizeof(err));
	CHECK(cs != NULL);
	CHECK(dirstore_get_state(cs, &st) && phash_eq(&st, &roots[0]));
	CHECK_EQ(dirstore_journal_end(cs), ends[0]);
	CHECK(!cs->ops->has(cs, &roots[2]));
	verify_store(cs);
	dirstore_crash(cs);

	/* the cut is durable: a plain open now sees a */
	cs = reopen(dir);
	CHECK(dirstore_get_state(cs, &st) && phash_eq(&st, &roots[0]));
	/* and a later commit, then compaction, then opening as of it */
	prolly_ctx_init(&ctx, cs, &small_policy);
	{
		PHash		d = build_even_rows(&ctx, 1234);

		cs_ref_root(cs, &d);
		dirstore_set_state(cs, &d);
		prolly_release_pins(&ctx);
		CHECK_OK(dirstore_compact(cs, 0.0, &cst));
		CHECK(dirstore_file_count(cs) > 0 && dirstore_journal_end(cs) == 0);
		prolly_ctx_free(&ctx);
		dirstore_crash(cs);
		cs = dirstore_open_at(dir, &d, err, sizeof(err));
		CHECK(cs != NULL);
		prolly_ctx_init(&ctx, cs, &small_policy);
		CHECK_EQ(t_count(&ctx, &d), 1234);
		CHECK_EQ(t_count(&ctx, &roots[0]), 3000);
		verify_store(cs);
		prolly_ctx_free(&ctx);
		cs_destroy(cs);
	}
	cleanup(dir);
}

/* ------------------------------------------------- severing gives disk back */

typedef struct DiskRun
{
	uint64_t	disk[128];
	uint64_t	live[128];
	uint64_t	copied,
				reclaimed;
	int			n;
} DiskRun;

/*
 * Commits on a table with the window severing behind them.  With compaction
 * every few commits, disk stays bounded by what the window holds; without
 * it, the files keep every byte ever written.
 */
static void
disk_run(bool compact, bool export, DiskRun *out, uint64_t *archive_disk)
{
	char	   *dir = tmpdir(),
			   *adir = tmpdir();
	ChunkStore *cs = reopen(dir);
	ChunkStore *acs = reopen(adir);
	VcRepo		r;
	const int	commits = 60;

	memset(out, 0, sizeof(*out));
	dirstore_set_file_target(cs, 1 << 20);
	setup(&r, cs);
	commit_rows(&r, 0, 30000, 0, "load");
	CHECK(vc_window_track(&r, 5) == VC_OK);
	rng_seed(4711);
	for (int i = 1; i <= commits; i++)
	{
		EditBatch	b;
		PHash		rv = vc_working(&r),
					o;
		uint32_t	seg;

		batch_init(&b, 400);
		for (int j = 0; j < 400; j++)
			batch_put(&b, rnd_below(30000), (uint64_t) i);
		CHECK(vc_table_edit(&r.ctx, &rv, "t", b.e, b.n, &o) == 0);
		vc_set_working(&r, &o);
		batch_free(&b);
		vc_commit(&r, "step");
		CHECK(vc_sever(&r, &(VcWindow) {5, NULL, 0}, &seg) == VC_OK);
		if (seg != VC_NO_SEGMENT && export)
			CHECK(vc_segment_export(&r, seg, &(VcSource) {"cold", acs}) == VC_OK);
		else if (seg != VC_NO_SEGMENT)
			CHECK(vc_segment_discard(&r, seg) == VC_OK);
		{
			PHash		st = vc_repo_save(&r);

			dirstore_set_state(cs, &st);
		}
		CHECK_OK(dirstore_flush(cs));
		if (compact && i % 10 == 0)
		{
			DirCompactStats s;

			CHECK_OK(dirstore_compact(cs, 0.3, &s));
			out->copied += s.bytes_copied;
			out->reclaimed += s.disk_before > s.disk_after ? s.disk_before - s.disk_after : 0;
		}
		out->disk[out->n] = dirstore_disk_bytes(cs);
		out->live[out->n] = cs->ops->bytes(cs);
		out->n++;
	}
	if (archive_disk)
	{
		CHECK_OK(dirstore_flush(acs));
		*archive_disk = dirstore_disk_bytes(acs);
	}
	/* reopened, the window's commits read back whole */
	{
		PHash		head = vc_head(&r)->head,
					state;
		Snap		sh = snap(&r.ctx, &head);

		vc_repo_free(&r);
		cs_destroy(cs);
		cs = reopen(dir);
		CHECK(dirstore_get_state(cs, &state));
		CHECK(vc_repo_open(&r, cs, &small_policy, &state) == VC_OK);
		check_snap(&r.ctx, &head, &sh);
		verify_store(cs);
		free(sh.v);
	}
	vc_repo_free(&r);
	cs_destroy(cs);
	cs_destroy(acs);
	cleanup(dir);
	cleanup(adir);
}

static void
test_severing_gives_disk_back(void)
{
	DiskRun		none,
				gc,
				exp;
	uint64_t	adisk = 0;
	int			mid = 29,
				last = 59;

	disk_run(false, false, &none, NULL);
	disk_run(true, false, &gc, NULL);
	disk_run(true, true, &exp, &adisk);
	printf("\n  %-26s %12s %12s %12s\n", "", "disk @30", "disk @60", "live @60");
	printf("  %-26s %10.1f MB %10.1f MB %10.1f MB\n", "sever, no compaction",
		   none.disk[mid] / 1048576.0, none.disk[last] / 1048576.0, none.live[last] / 1048576.0);
	printf("  %-26s %10.1f MB %10.1f MB %10.1f MB\n", "sever + discard + compact",
		   gc.disk[mid] / 1048576.0, gc.disk[last] / 1048576.0, gc.live[last] / 1048576.0);
	printf("  %-26s %10.1f MB %10.1f MB %10.1f MB\n", "sever + export + compact",
		   exp.disk[mid] / 1048576.0, exp.disk[last] / 1048576.0, exp.live[last] / 1048576.0);
	printf("  archive after export: %.1f MB; compaction copied %.2f bytes per byte reclaimed\n%-44s",
		   adisk / 1048576.0, gc.reclaimed ? (double) gc.copied / (double) gc.reclaimed : 0, "");

	/* without compaction, disk keeps everything ever written: it grows */
	CHECK(none.disk[last] > none.disk[mid] * 3 / 2);
	/* with it, disk follows what the window holds, not history's length */
	CHECK_LE(gc.disk[last], gc.disk[mid] * 13 / 10);
	CHECK_LE(gc.disk[last], none.disk[last] / 2);
	/*
	 * Just after a compaction, disk is close to the live bytes, plus the
	 * zeros the journal keeps allocated ahead of its end (4 MB).
	 */
	CHECK_LE(gc.disk[last], gc.live[last] * 3 / 2 + (1 << 20) + (4 << 20));
	/* exported history leaves this host the same way */
	CHECK_LE(exp.disk[last], gc.disk[last] * 11 / 10 + (1 << 20));
	CHECK(adisk > 0);
}

int
main(void)
{
	RUN(test_round_trip);
	RUN(test_unflushed_lost_garbage_dropped);
	RUN(test_files_are_immutable);
	RUN(test_journal_append_only);
	RUN(test_journal_torn_tail);
	RUN(test_journal_mid_log_damage);
	RUN(test_journal_open_at);
	RUN(test_corruption_detected);
	RUN(test_repo_save_and_reopen);
	RUN(test_crash_points);
	RUN(test_severing_gives_disk_back);
	printf("test_dirstore: all tests passed\n");
	return 0;
}
