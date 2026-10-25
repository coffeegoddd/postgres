# Copyright (c) 2026, PostgreSQL Global Development Group

# A share or a snapshot of a block-mapped relation, which logs the
# relation's own pages with its map's header held exclusively (at
# wal_level replica), never waits for a page's content lock meanwhile: a
# backend holding a page exclusively may be waiting for that header, to
# ask whether the page is shared or to prepare it for a change (a pruning
# scan, a SELECT FOR UPDATE).  It lets go of the header, waits for the
# page, and walks again.

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
$node->init;
$node->append_conf('postgresql.conf', qq{
autovacuum = off
prolly_fold_worker = off
wal_level = replica
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE EXTENSION test_prolly;
	CREATE EXTENSION injection_points;
	CREATE SCHEMA cow VERSIONED;
	CREATE TABLE cow.scratch (a int);
	INSERT INTO cow.scratch VALUES (1);
});
my $scratch = sql(q{SELECT pg_relation_filenode('cow.scratch')});

# Pages full of rows an update left dead, which a scan prunes.
sql(q{CREATE TABLE cow.p (a int, b text);
	INSERT INTO cow.p SELECT g, repeat('x', 40) FROM generate_series(1, 400) g});
sql(q{UPDATE cow.p SET b = repeat('y', 40) WHERE a <= 250});
my $p = sql(q{SELECT pg_relation_filenode('cow.p')});

# A page of a few rows, which no scan prunes, locked through an index.
sql(q{CREATE TABLE cow.u (a int PRIMARY KEY);
	INSERT INTO cow.u SELECT generate_series(1, 10)});
my $u = sql(q{SELECT pg_relation_filenode('cow.u')});

# 'op' (a share or a snapshot) stopped at the first page it logs, holding
# the header; then 'go', in a session 'prep' set up before (its scan or
# plan made, as those read the relation's length from the header), which
# locks a page exclusively and waits for the header.  The op let go, both
# are to finish within the timeout.
sub check_no_deadlock
{
	my ($name, $prep, $go, $op) = @_;
	my $reader = $node->background_psql('postgres', on_error_stop => 0);

	$reader->query_safe($prep);
	# every cached page checked again before it next changes
	sql(qq{SELECT test_bmap_share($scratch)});
	sql(q{SELECT pg_stat_reset_shared('prolly')});
	sql(q{SELECT injection_points_attach('bmap-preserve-page', 'wait')});

	my $opsess = $node->background_psql('postgres', on_error_stop => 0);
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

my $pcopy = check_no_deadlock('prune and share',
	q{BEGIN; DECLARE c CURSOR FOR SELECT a FROM cow.p; FETCH 1 FROM c},
	'FETCH 150 FROM c; COMMIT',
	"test_bmap_share($p)");
is(sql(q{SELECT count(*), count(*) FILTER (WHERE b LIKE 'y%') FROM cow.p}),
	'400|250', 'the rows');

my $lock = q{SET plan_cache_mode = force_generic_plan;
	PREPARE l AS SELECT a FROM cow.u WHERE a = 5 FOR UPDATE;
	BEGIN; EXECUTE l; COMMIT};
my $ucopy = check_no_deadlock('row lock and share', $lock,
	'BEGIN; EXECUTE l; COMMIT', "test_bmap_share($u)");
# (the rows as shared, before the next lock changes them)
my $udigest = sql(qq{SELECT test_bmap_rows_digest($u)});
check_no_deadlock('row lock and snapshot', $lock,
	'BEGIN; EXECUTE l; COMMIT', "test_bmap_snapshot_take($u)");
is(sql(q{SELECT count(*) FROM cow.u}), '10', 'the rows locked');

# The copies have the sources' rows, and every page logged again after the
# walk began again is the page then: so they do after a crash too.
my $pdigest = sql(qq{SELECT test_bmap_rows_digest($p)});
SKIP:
{
	skip 'a share deadlocked', 4 unless $pcopy && $ucopy;
	is(sql(qq{SELECT test_bmap_rows_digest($pcopy)}), $pdigest, 'the first copy has the rows');
	is(sql(qq{SELECT test_bmap_rows_digest($ucopy)}), $udigest, 'so does the second');
	$node->stop('immediate');
	$node->start;
	is(sql(qq{SELECT test_bmap_rows_digest($pcopy)}), $pdigest, 'after a crash too');
	is(sql(qq{SELECT test_bmap_rows_digest($ucopy)}), $udigest, 'both');
}

$node->stop;
done_testing();
