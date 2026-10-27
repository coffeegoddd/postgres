# Copyright (c) 2026, PostgreSQL Global Development Group

# A share or a capture of a block-mapped relation, which logs the
# relation's own pages with its map's header held exclusively (at
# wal_level replica), never waits for a page's content lock meanwhile: a
# backend holding a page exclusively may be waiting for that header, to
# ask whether the page is shared or to prepare it for a change (a pruning
# scan, a SELECT FOR UPDATE).  It lets go of the header, waits for the
# page, and walks again, logging only the pages it hasn't logged.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('preserve_waits');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', qq{
autovacuum = off
prolly_fold_worker = off
});
$node->start;
$node->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('preserve_waits_standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sub counter
{
	return sql(qq{SELECT value FROM pg_stat_prolly WHERE name = '$_[0]'});
}

sql(q{
	CREATE EXTENSION test_prolly;
	CREATE EXTENSION injection_points;
	CREATE SCHEMA cow VERSIONED;
	CREATE TABLE cow.scratch (a int);
	INSERT INTO cow.scratch VALUES (1);
});
my $scratch = sql(q{SELECT pg_relation_filenode('cow.scratch')});

# 'op' (a share or a capture) stopped at the first page it logs, holding
# the header; then 'go', in a session 'prep' set up before (its scan or
# plan made, as those read the relation's length from the header), which
# locks a page exclusively and waits for the header.  The op let go, both
# are to finish within the timeout, the op having walked again.  With
# 'released', that is run while the op has let go of the header, before it
# waits for the page.  The op's result, or undef if it deadlocked.
sub check_no_deadlock
{
	my ($name, $prep, $go, $op, $released) = @_;
	my $reader = $node->background_psql('postgres', on_error_stop => 0);

	$reader->query_safe($prep);
	# every cached page checked again before it next changes
	sql(qq{SELECT test_bmap_share($scratch)});
	sql(q{SELECT pg_stat_reset_shared('prolly')});

	my $opsess = $node->background_psql('postgres', on_error_stop => 0);
	$opsess->query_safe(q{SELECT injection_points_set_local();
		SELECT injection_points_attach('bmap-preserve-page', 'wait')});
	$opsess->query_safe(q{SELECT injection_points_attach('bmap-preserve-restart', 'wait')})
	  if $released;
	$opsess->query_until(qr/started/,
		qq{\\echo started\nSELECT $op AS made \\gset\nSELECT pg_stat_force_next_flush();\n});
	$node->wait_for_event('client backend', 'bmap-preserve-page');

	$reader->query_until(qr/started/, qq{\\echo started\n$go;\n});
	$node->poll_query_until('postgres', q{SELECT count(*) > 0 FROM pg_stat_activity
		WHERE wait_event_type = 'Buffer' AND wait_event = 'BufferShared'})
	  or die "$name: the reader didn't wait for the header";

	# (detached first, so the walk run again doesn't stop there)
	sql(q{SELECT injection_points_detach('bmap-preserve-page')});
	sql(q{SELECT injection_points_wakeup('bmap-preserve-page')});
	if ($released)
	{
		$node->wait_for_event('client backend', 'bmap-preserve-restart');
		$released->();
		sql(q{SELECT injection_points_detach('bmap-preserve-restart')});
		sql(q{SELECT injection_points_wakeup('bmap-preserve-restart')});
	}

	my $done = 0;
	for (my $i = 0; $i < 30 * 10; $i++)
	{
		if (sql(q{SELECT count(*) FROM pg_stat_activity
			WHERE backend_type = 'client backend' AND state = 'active'
			AND pid <> pg_backend_pid()}) eq '0')
		{
			$done = 1;
			last;
		}
		usleep(100_000);
	}
	ok($done, "$name: the op and the reader finish");
	if (!$done)
	{
		# deadlocked: only stopping the server ends it
		$node->stop('immediate');
		$node->start;
		return undef;
	}
	my $made = $opsess->query_safe(q{SELECT :'made'});
	$opsess->quit;
	$reader->quit;
	ok($node->poll_query_until('postgres', q{SELECT value > 0 FROM pg_stat_prolly
		WHERE name = 'bmap_preserve_waits'}),
		"$name: the op let go of the header and walked again");
	return $made;
}

# Pages full of rows an update left dead, which a scan prunes.
sql(q{CREATE TABLE cow.p (a int, b text);
	INSERT INTO cow.p SELECT g, repeat('x', 40) FROM generate_series(1, 400) g});
sql(q{UPDATE cow.p SET b = repeat('y', 40) WHERE a <= 250});
my $p = sql(q{SELECT pg_relation_filenode('cow.p')});
my $pcopy = check_no_deadlock('prune and share',
	q{BEGIN; DECLARE c CURSOR FOR SELECT a FROM cow.p; FETCH 1 FROM c},
	'FETCH 150 FROM c; COMMIT',
	"test_bmap_share($p)");
is(sql(q{SELECT count(*), count(*) FILTER (WHERE b LIKE 'y%') FROM cow.p}),
	'400|250', 'the rows');
my $pdigest = sql(qq{SELECT test_bmap_rows_digest($p)});

# Three pages, a row of the last locked through an index: the walk logs the
# first two, gives up on the third, and logs only that one when it walks
# again, the page the reader locked held meanwhile.
sql(q{CREATE TABLE cow.m (a int PRIMARY KEY, b text);
	INSERT INTO cow.m SELECT g, repeat('m', 200) FROM generate_series(1, 100) g});
my $m = sql(q{SELECT pg_relation_filenode('cow.m')});
my $mpages = sql(qq{SELECT count(*) FROM test_bmap_map($m)});
is($mpages, '3', 'three pages');
is(sql(q{SELECT (ctid::text::point)[0] FROM cow.m WHERE a = 100}), '2',
	'the row locked is on the last');
my $lock = sub {
	my ($table, $a) = @_;
	return qq{SET plan_cache_mode = force_generic_plan;
		PREPARE l AS SELECT a FROM $table WHERE a = $a FOR UPDATE;
		BEGIN; EXECUTE l; COMMIT};
};
my $mcopy = check_no_deadlock('row lock and share', $lock->('cow.m', 100),
	'BEGIN; EXECUTE l; COMMIT', "test_bmap_share($m)");
is(counter('bmap_pages_preserved'), $mpages,
	'each of its pages logged once, though the walk began again');
# (the rows as shared, before a later lock changes them)
my $mdigest = sql(qq{SELECT test_bmap_rows_digest($m)});

# A standby replays the images logged by both walks.
$node->wait_for_replay_catchup($standby);
SKIP:
{
	skip 'the share deadlocked', 1 unless $mcopy;
	is($standby->safe_psql('postgres', qq{SELECT test_bmap_rows_digest($mcopy)}),
		$mdigest, 'a standby\'s copy has the rows');
}

# The same for a capture, of a page of a few rows.
sql(q{CREATE TABLE cow.u (a int PRIMARY KEY);
	INSERT INTO cow.u SELECT generate_series(1, 10)});
my $u = sql(q{SELECT pg_relation_filenode('cow.u')});
check_no_deadlock('row lock and capture', $lock->('cow.u', 5),
	'BEGIN; EXECUTE l; COMMIT', "test_bmap_root_capture($u)");
is(sql(q{SELECT count(*) FROM cow.u}), '10', 'the rows locked');

# A page copied while the header is let go: as of a fold, the row lock's
# preparing it copies it to another block, which the walk logs.
sql(q{CREATE TABLE cow.c (a int PRIMARY KEY, b text);
	INSERT INTO cow.c SELECT g, repeat('c', 200) FROM generate_series(1, 100) g});
my $c = sql(q{SELECT pg_relation_filenode('cow.c')});
sql(qq{SELECT test_bmap_fold_begin($c)});
my $cmap = sql(qq{SELECT string_agg(phys::text, ',' ORDER BY blkno) FROM test_bmap_map($c)});
my $ccopy = check_no_deadlock('row lock copying a page and share',
	$lock->('cow.c', 100), 'BEGIN; EXECUTE l; COMMIT', "test_bmap_share($c)");
isnt(sql(qq{SELECT string_agg(phys::text, ',' ORDER BY blkno) FROM test_bmap_map($c)}),
	$cmap, 'the lock copied the page');
is(counter('bmap_pages_preserved'), '3', 'which is logged at its new block');
my $cdigest = sql(qq{SELECT test_bmap_rows_digest($c)});

# The copies have the sources' rows as shared, after a crash too.
SKIP:
{
	skip 'a share deadlocked', 6 unless $pcopy && $mcopy && $ccopy;
	is(sql(qq{SELECT test_bmap_rows_digest($pcopy)}), $pdigest, 'the first copy has the rows');
	is(sql(qq{SELECT test_bmap_rows_digest($mcopy)}), $mdigest, 'so does the second');
	is(sql(qq{SELECT test_bmap_rows_digest($ccopy)}), $cdigest, 'and the third');
	$node->stop('immediate');
	$node->start;
	is(sql(qq{SELECT test_bmap_rows_digest($pcopy)}), $pdigest, 'after a crash too');
	is(sql(qq{SELECT test_bmap_rows_digest($mcopy)}), $mdigest, 'the second');
	is(sql(qq{SELECT test_bmap_rows_digest($ccopy)}), $cdigest, 'the third');
}

$standby->stop;
$node->stop;
done_testing();
