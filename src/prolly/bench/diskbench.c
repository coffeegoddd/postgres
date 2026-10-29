/*
 * diskbench.c
 *	  Disk used by a durable store while the window severs history behind
 *	  a stream of commits, with and without compaction.
 *
 * A table of --rows rows; --commits commits of --changes random row
 * updates; the window keeps --window commits and severed history is
 * discarded (or exported, with --export, to an archive store beside it).
 * Every commit saves the repo and flushes; every --compact-every commits
 * the store compacts files with at least a third dead.  Prints disk
 * and live bytes every --every commits, then what compaction copied per
 * byte it gave back.
 */
#define _DEFAULT_SOURCE
#include <getopt.h>
#include <sys/stat.h>

#include "../test/test_util.h"

int
main(int argc, char **argv)
{
	uint64_t	rows = 1000000;
	int			commits = 200,
				changes = 2000,
				window = 20,
				compact_every = 20,
				every = 20;
	bool		export = false;
	const char *dir = "/home/dustin/pgv2-bench/diskbench";
	static struct option opts[] = {
		{"rows", 1, 0, 'r'}, {"commits", 1, 0, 'c'}, {"changes", 1, 0, 'n'},
		{"window", 1, 0, 'w'}, {"compact-every", 1, 0, 'k'}, {"every", 1, 0, 'e'},
		{"export", 0, 0, 'x'}, {"dir", 1, 0, 'd'}, {0, 0, 0, 0}
	};
	int			ch;
	char		err[256],
				adir[600];
	ChunkStore *cs,
			   *acs = NULL;
	VcRepo		r;
	PHash		rv,
				data;
	uint64_t	copied = 0,
				reclaimed = 0;
	double		compact_s = 0;
	const char *ddl = "CREATE TABLE t (id int8 PRIMARY KEY, v int8)";

	while ((ch = getopt_long(argc, argv, "", opts, NULL)) != -1)
		switch (ch)
		{
			case 'r': rows = strtoull(optarg, NULL, 10); break;
			case 'c': commits = atoi(optarg); break;
			case 'n': changes = atoi(optarg); break;
			case 'w': window = atoi(optarg); break;
			case 'k': compact_every = atoi(optarg); break;
			case 'e': every = atoi(optarg); break;
			case 'x': export = true; break;
			case 'd': dir = optarg; break;
			default:
				fprintf(stderr, "diskbench [--rows N] [--commits N] [--changes N] [--window W]\n"
						"          [--compact-every K (0: never)] [--every N] [--export] [--dir D]\n");
				return 2;
		}
	mkdir(dir, 0700);
	cs = dirstore_open(dir, err, sizeof(err));
	CHECK(cs != NULL);
	CHECK(cs->ops->count(cs) == 0);	/* start from an empty directory */
	if (export)
	{
		snprintf(adir, sizeof(adir), "%s.archive", dir);
		acs = dirstore_open(adir, err, sizeof(err));
		CHECK(acs != NULL);
	}
	rng_seed(99);
	vc_repo_init(&r, cs, &prolly_default_policy);
	rv = vc_working(&r);
	CHECK(vc_table_create(&r.ctx, &rv, "t", ddl, strlen(ddl), &rv) == 0);
	data = build_even_rows(&r.ctx, rows);
	CHECK(vc_table_set_data(&r.ctx, &rv, "t", &data, &rv) == 0);
	vc_set_working(&r, &rv);
	vc_commit(&r, "load");
	CHECK(vc_window_track(&r, (uint32_t) window) == VC_OK);

	printf("%8s %12s %12s %8s\n", "commits", "disk_mb", "live_mb", "files");
	for (int i = 1; i <= commits; i++)
	{
		EditBatch	b;
		PHash		w = vc_working(&r),
					o,
					st;
		uint32_t	seg;

		batch_init(&b, (size_t) changes);
		for (int j = 0; j < changes; j++)
			batch_put(&b, rnd_below(rows) * 2, (uint64_t) i);
		CHECK(vc_table_edit(&r.ctx, &w, "t", b.e, b.n, &o) == 0);
		vc_set_working(&r, &o);
		batch_free(&b);
		vc_commit(&r, "step");
		CHECK(vc_sever(&r, &(VcWindow) {.depth = (uint32_t) window}, &seg) == VC_OK);
		if (seg != VC_NO_SEGMENT && acs)
			CHECK(vc_segment_export(&r, seg, &(VcSource) {"archive", acs}) == VC_OK);
		else if (seg != VC_NO_SEGMENT)
			CHECK(vc_segment_discard(&r, seg) == VC_OK);
		st = vc_repo_save(&r);
		dirstore_set_state(cs, &st);
		CHECK_OK(dirstore_flush(cs));
		if (compact_every > 0 && i % compact_every == 0)
		{
			DirCompactStats s;
			double		t = now_sec();

			CHECK_OK(dirstore_compact(cs, 1.0 / 3, &s));
			compact_s += now_sec() - t;
			copied += s.bytes_copied;
			if (s.disk_before > s.disk_after)
				reclaimed += s.disk_before - s.disk_after;
		}
		if (i % every == 0)
			printf("%8d %12.1f %12.1f %8zu\n", i, dirstore_disk_bytes(cs) / 1048576.0,
				   cs->ops->bytes(cs) / 1048576.0, dirstore_file_count(cs));
		fflush(stdout);
	}
	printf("# compaction: copied %.0f MB to give back %.0f MB (%.2f bytes per byte), %.1f s\n",
		   copied / 1048576.0, reclaimed / 1048576.0,
		   reclaimed ? (double) copied / (double) reclaimed : 0, compact_s);
	if (acs)
	{
		CHECK_OK(dirstore_flush(acs));
		printf("# archive: %.1f MB on disk\n", dirstore_disk_bytes(acs) / 1048576.0);
	}
	{
		PHash		bad;

		CHECK(dirstore_verify(cs, &bad));
		check_store(cs, 0);
	}
	vc_repo_free(&r);
	cs_destroy(cs);
	if (acs)
		cs_destroy(acs);
	return 0;
}
