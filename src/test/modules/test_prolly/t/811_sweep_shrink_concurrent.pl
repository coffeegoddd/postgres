# Copyright (c) 2026, PostgreSQL Global Development Group

# Two page sweeps of one store at once.  Sweep A has counted, found blocks
# at the store's end that nothing holds, and let go of its lock, but not
# yet freed them.  Sweep B frees the same blocks: it must not shorten the
# store past them while A runs, or A's frees would fall past its end, or,
# once the store has grown again into those block numbers, free blocks a
# table now holds.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('sweep_shrink_concurrent');
$node->init;
$node->append_conf('postgresql.conf', "autovacuum = off\nprolly_fold_worker = off");
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE EXTENSION test_prolly;
	CREATE EXTENSION injection_points;
	CREATE SCHEMA kv VERSIONED;
	CREATE TABLE kv.t (id int PRIMARY KEY, v text) WITH (autovacuum_enabled = off);
	INSERT INTO kv.t SELECT g, repeat('t', 100) FROM generate_series(1, 2000) g;
});
sql(q{SELECT count(*) FROM prolly_page_sweep()});

# blocks at the store's end that nothing holds, as a crash leaks them: a
# map's, extended, its files then lost
my $fn = sql(q{SELECT pg_relation_filenode('kv.t')});
my $copy = sql(qq{SELECT test_bmap_share($fn, false)});
sql(qq{SELECT test_bmap_extend($copy, 64); CHECKPOINT});
my $dir = $node->data_dir . '/base/'
  . sql(q{SELECT oid FROM pg_database WHERE datname = 'postgres'});
for my $f ("${copy}_bmap", "${copy}_chg")
{
	unlink("$dir/$f") if -e "$dir/$f";
}
ok(!-e "$dir/${copy}_bmap", 'the map holding the store\'s last blocks is gone');

# sweep A stopped after counting, before it frees
my $a = $node->background_psql('postgres', on_error_stop => 0);
$a->query_safe(q{SELECT injection_points_set_local();
	SELECT injection_points_attach('bmap-sweep-freeing', 'wait')});
$a->query_until(qr/started/, q{
	\echo started
	SELECT 'freed ' || freed FROM prolly_page_sweep();
	\echo done
});
$node->poll_query_until('postgres', q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE wait_event = 'bmap-sweep-freeing'}) or die 'sweep A did not stop';

# sweep B meanwhile frees the same blocks, and leaves the store as long
my ($freed, $truncated) = split /\|/,
  sql(q{SELECT freed, truncated FROM prolly_page_sweep()});
cmp_ok($freed, '>=', 64, 'sweep B frees the blocks nothing holds');
is($truncated, '0', 'and doesn\'t shorten the store while sweep A runs');

# a table grows meanwhile (into new blocks: none is reused while A runs)
sql(q{CREATE TABLE kv.u (id int PRIMARY KEY, v text) WITH (autovacuum_enabled = off);
	INSERT INTO kv.u SELECT g, repeat('u', 100) FROM generate_series(1, 3000) g});
my $want = sql(q{SELECT count(*) || ' ' || sum(length(v)) FROM kv.u});

# sweep A frees what it counted: the blocks B freed already, nothing more
sql(q{SELECT injection_points_wakeup('bmap-sweep-freeing')});
$a->query_until(qr/done/, '');
unlike($a->{stderr}, qr/ERROR/, 'sweep A ends with no error');
$a->quit;

# with no sweep running, free blocks are taken again: a table written now
# takes none of u's, and a later sweep finds every map whole
sql(q{CREATE TABLE kv.w (id int PRIMARY KEY, v text) WITH (autovacuum_enabled = off);
	INSERT INTO kv.w SELECT g, repeat('w', 100) FROM generate_series(1, 3000) g});
my $wwant = sql(q{SELECT count(*) || ' ' || sum(length(v)) FROM kv.w});
my ($ret, $out, $err) = $node->psql('postgres',
	q{SELECT count(*) FROM prolly_page_sweep()});
is($ret, 0, 'a later sweep finds every map whole') or diag($err);

# read back from disk
$node->restart;
is(sql(q{SELECT count(*) || ' ' || sum(length(v)) FROM kv.u}), $want,
	'the table that grew reads as it was written');
is(sql(q{SELECT count(*) || ' ' || sum(length(v)) FROM kv.w}), $wwant,
	'and so does the one written after');
is(sql(q{SELECT count(*) FROM kv.t}), '2000', 'and the first');

$node->stop;
done_testing();
