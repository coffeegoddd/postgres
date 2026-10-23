# Copyright (c) 2026, PostgreSQL Global Development Group

# The blocks past a truncated relation's length are let go of once the
# truncation is done (#638), freed if no other map holds them.  A crash
# between the truncation and that leaves them mapped past the length:
# nothing frees a block replay still maps, and the next truncation lets go
# of them.  A crash after it replays the frees, and records from before
# the truncation for a page past the length write nothing onto a freed
# block another relation was given.  A cancel as the blocks are let go of
# waits until they are freed.  A standby replays the same.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('truncated_blocks');
$node->init(allows_streaming => 1);
$node->append_conf(
	'postgresql.conf', q{
debug_block_mapped_heap = on
autovacuum = off
shared_buffers = 400kB
wal_consistency_checking = 'all'
});
$node->start;
$node->safe_psql('postgres',
	'CREATE EXTENSION test_prolly; CREATE EXTENSION injection_points');
$node->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('truncated_blocks_standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sub nblocks
{
	my $rel = shift;
	return sql(
		qq{SELECT pg_relation_size('$rel') / current_setting('block_size')::int});
}

sub past_end
{
	my ($n, $rel) = @_;
	return $n->safe_psql('postgres',
		qq{SELECT count(*) FROM test_bmap_past_end(pg_relation_filenode('$rel'))});
}

sub free_blocks { return $_[0]->safe_psql('postgres', 'SELECT test_bmap_free_blocks()'); }

# The standby replays what the primary has, and has its rows and counts.
sub standby_agrees
{
	my ($what, @rels) = @_;
	$node->wait_for_catchup($standby, 'replay', $node->lsn('flush'));
	is(free_blocks($standby), free_blocks($node),
		"$what: the standby's page store frees what the primary's does");
	foreach my $rel (@rels)
	{
		my $q = qq{SELECT count(*), sum(a), md5(string_agg(b, '' ORDER BY a)) FROM $rel};
		is($standby->safe_psql('postgres', $q), sql($q),
			"$what: the standby reads $rel as the primary does");
		is(past_end($standby, $rel), past_end($node, $rel),
			"$what: and maps as much of $rel past its length");
	}
}

# A crash once the truncation is done, before the blocks past the length
# are let go of.
sql(q{CREATE TABLE h (a int, b text);
	INSERT INTO h SELECT g, repeat('x', 500) FROM generate_series(1, 1500) g});
is(nblocks('h'), '100', 'the table has 100 blocks');
sql(q{CHECKPOINT; DELETE FROM h WHERE a > 150});
my $free_before = free_blocks($node);

sql(q{SELECT injection_points_attach('bmap-truncated', 'wait')});
my $vacuum = $node->background_psql('postgres');
$vacuum->query_until(qr/vacuuming/, q{\echo vacuuming
VACUUM h;
});
$node->wait_for_event('client backend', 'bmap-truncated');
$node->stop('immediate');
$vacuum->{run}->finish;
$node->start;

is(nblocks('h'), '10', 'after the crash, the table has the length it was cut to');
is(sql(q{SELECT count(*), sum(a) FROM h}), '150|11325', 'and its rows');
is(past_end($node, 'h'), '90', 'the blocks past its length stay mapped');
is(free_blocks($node), $free_before, 'and none is freed');
sql('CHECKPOINT');
is(sql(q{SELECT freed FROM prolly_page_sweep()}), '0',
	'a sweep frees none of them either');
is(past_end($node, 'h'), '90', 'they stay mapped after it');
standby_agrees('crash before letting go', 'h');

# The next truncation lets go of them too.
sql(q{DELETE FROM h WHERE a > 60; VACUUM h});
is(nblocks('h'), '4', 'the next truncation cuts the table to 4 blocks');
is(past_end($node, 'h'), '0', 'and lets go of every block past it');
is(free_blocks($node) - $free_before, 96, 'the earlier truncation\'s too');
standby_agrees('the next truncation', 'h');

# A crash after the blocks are freed and given to another relation.
# Recovery begins before the pages past the length changed (their images
# logged, maybe written out), and replays them, the truncation, the frees,
# and the other relation's use of the blocks.
sql(q{CREATE TABLE k (a int, b text);
	INSERT INTO k SELECT g, repeat('x', 500) FROM generate_series(1, 1500) g});
sql('CHECKPOINT');
sql(q{UPDATE k SET b = repeat('y', 500) WHERE a > 150});
sql(q{DELETE FROM k WHERE a > 150});
$free_before = free_blocks($node);
sql(q{VACUUM k});
is(nblocks('k'), '10', 'a second table is truncated to 10 blocks');
my $freed = free_blocks($node);
cmp_ok($freed - $free_before, '>=', 90, 'freeing its blocks past the length');
sql(q{CREATE TABLE g (a int, b text);
	INSERT INTO g SELECT g, repeat('z', 500) FROM generate_series(1, 1500) g});
# (and its TOAST index's metapage, and the leaves of both maps)
is($freed - free_blocks($node), nblocks('g') + 3, 'another table is given them');
my $g = sql(q{SELECT count(*), sum(a), md5(string_agg(b, '' ORDER BY a)) FROM g});
my $k = sql(q{SELECT count(*), sum(a), md5(string_agg(b, '' ORDER BY a)) FROM k});
my $free_now = free_blocks($node);

$node->stop('immediate');
$node->start;
is(sql(q{SELECT count(*), sum(a), md5(string_agg(b, '' ORDER BY a)) FROM g}), $g,
	'after the crash, the other table reads back');
is(sql(q{SELECT count(*), sum(a), md5(string_agg(b, '' ORDER BY a)) FROM k}), $k,
	'and the truncated one');
is(past_end($node, 'k'), '0', 'which maps nothing past its length');
is(free_blocks($node), $free_now, 'the page store as it was');
sql('CHECKPOINT');
is(sql(q{SELECT freed FROM prolly_page_sweep()}), '0',
	'a sweep finds nothing left to free');
$free_now = free_blocks($node);
standby_agrees('crash after the frees', 'k', 'g');

# And a crash after the sweep.
$node->stop('immediate');
$node->start;
is(sql(q{SELECT count(*), sum(a), md5(string_agg(b, '' ORDER BY a)) FROM g}), $g,
	'after a crash after the sweep, the other table reads back');
is(free_blocks($node), $free_now, 'and the page store is as the sweep left it');
standby_agrees('crash after the sweep', 'k', 'g');

# A truncation cancelled as it lets go of the blocks past the length (an
# autovacuum's, waited on by a query) still frees those it let go of: the
# cancel waits until they are freed.
sql(q{CREATE TABLE c (a int, b text);
	INSERT INTO c SELECT g, repeat('x', 500) FROM generate_series(1, 1500) g});
sql(q{DELETE FROM c WHERE a > 150});
$free_before = free_blocks($node);
sql(q{SELECT injection_points_attach('bmap-truncation-let-go', 'wait')});
my $cancelled = $node->background_psql('postgres', on_error_stop => 0);
my $pid = $cancelled->query_safe('SELECT pg_backend_pid()');
$cancelled->query_until(qr/vacuuming/, q{\echo vacuuming
VACUUM c;
});
$node->wait_for_event('client backend', 'bmap-truncation-let-go');
sql(qq{SELECT pg_cancel_backend($pid)});
sql(q{SELECT injection_points_detach('bmap-truncation-let-go')});
# (fails if the cancel ended the wait)
$node->psql('postgres', q{SELECT injection_points_wakeup('bmap-truncation-let-go')});
$cancelled->quit;
is(nblocks('c'), '10', 'a cancelled truncation cuts the table');
is(past_end($node, 'c'), '0', 'and lets go of every block past it');
cmp_ok(free_blocks($node) - $free_before, '>=', 90, 'freeing them');
standby_agrees('a cancelled truncation', 'c');

$standby->stop;
$node->stop;
done_testing();
