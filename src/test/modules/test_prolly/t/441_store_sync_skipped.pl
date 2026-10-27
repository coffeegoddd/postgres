# Copyright (c) 2026, PostgreSQL Global Development Group

# At wal_level minimal, as at the levels for backups, a share or capture
# of block-mapped storage logs its own blocks as images (PRESERVE), and its
# record says so; one holding no block of its own logs none.  None syncs
# the page store, a whole-file fsync that wrote back every page the kernel
# held for the store (a hard reset's discarded working set, the dirty
# working set a first checkout leaves behind; #947): WAL makes the blocks
# durable.  Their images are counted in bmap_pages_preserved.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('store_sync_skipped');
$node->init;
$node->append_conf('postgresql.conf', "autovacuum = off");
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

is(sql('SHOW wal_level'), 'minimal', 'at wal_level minimal');
sql(q{
	CREATE EXTENSION test_prolly;
	CREATE EXTENSION pg_walinspect;
	CREATE SCHEMA app VERSIONED;
	CREATE TABLE app.t (id int PRIMARY KEY, v int, pad text);
	INSERT INTO app.t SELECT i, 0, repeat('x', 500) FROM generate_series(1, 5000) i;
	SELECT dolt_commit('-Am', 'base');
});

# What a session's statement 'op' logged, after 'setup': its PRESERVE
# images (and the pages it counted as preserved), and its records saying
# images were logged.
sub logged_by
{
	my ($setup, $op) = @_;
	my $start = sql(qq{$setup; SELECT pg_current_wal_insert_lsn()}) =~ s/^.*\n//sr;
	my $preserved = sql(qq{
		CREATE TEMP TABLE before AS SELECT * FROM test_prolly_session_stats();
		$op;
		SELECT s.value - b.value FROM test_prolly_session_stats() s
		JOIN before b USING (name) WHERE s.name = 'bmap_pages_preserved'})
	  =~ s/^.*\n//sr;
	my $end = sql(q{SELECT pg_current_wal_insert_lsn()});
	my $records = sql(qq{
		SELECT count(*) FILTER (WHERE record_type = 'PRESERVE') || ' ' ||
			count(*) FILTER (WHERE description LIKE '%(images logged)%')
		FROM pg_get_wal_records_info('$start', '$end')
		WHERE resource_manager = 'BlockMap'});
	return "$preserved $records";
}

my $dirty = q{UPDATE app.t SET v = v + 1 WHERE id % 5 = 0};

# A commit keeping pages changed since logs their images.
like(logged_by($dirty, q{SELECT dolt_commit('-am', 'c1')}),
	qr/^([1-9]\d*) \1 [1-9]\d*$/,
	'a commit\'s kept roots log their own blocks as images');

# A hard reset of a dirty working set: its new storage holds no block of
# its own; the roots it shares were logged when captured.
like(logged_by($dirty, q{SELECT dolt_reset('--hard')}),
	qr/^0 0 /, 'a hard reset logs no image');
is(sql(q{SELECT count(*), sum(v) FROM app.t}), '5000|1000',
	'the hard reset discarded the dirty rows');

# A first checkout with a dirty source: its copies are made of the pages
# kept as of the commit, logged when they were kept.
sql(q{SELECT dolt_branch('fresh')});
like(logged_by($dirty, q{SELECT dolt_checkout('fresh')}),
	qr/^0 0 [1-9]\d*$/,
	'a first checkout of a branch with a dirty source logs no image');
is(sql(q{SELECT count(*), sum(v) FROM app.t}), '5000|2000',
	'main keeps its dirty rows');
like(sql(q{SELECT dolt_checkout('fresh'); SELECT count(*), sum(v) FROM app.t}),
	qr/^5000\|1000$/m, 'the branch has the commit\'s rows');
sql(q{SELECT dolt_reset('--hard')});

# A capture holding blocks of its own logs them; one holding none logs
# none, but its record says so all the same.
sql(q{SET debug_block_mapped_heap = on;
	CREATE TABLE plain (a int, b text);
	INSERT INTO plain SELECT g, repeat('p', 200) FROM generate_series(1, 2000) g});
my $plain = sql(q{SELECT pg_relation_filenode('plain')});
like(logged_by(q{UPDATE plain SET a = a + 1 WHERE a % 10 = 0},
		qq{SELECT test_bmap_root_capture($plain)}),
	qr/^([1-9]\d*) \1 1$/, 'a capture with blocks of its own logs them');
is(logged_by(q{SELECT 1}, qq{SELECT test_bmap_root_capture($plain)}),
	'0 0 1', 'a capture with none logs none');

$node->stop;
done_testing();
