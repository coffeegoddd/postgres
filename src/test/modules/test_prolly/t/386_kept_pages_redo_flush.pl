# Copyright (c) 2026, PostgreSQL Global Development Group

# Crash recovery at wal_level minimal (no images of kept pages logged)
# replaying a commit that keeps its pages, then a change that moves a kept
# page off its block.  Redo restores the page from its first change's full
# image and, with few buffers, writes it out to its block before replaying
# its later change, so the block holds an older version than the commit
# kept.  What puts the kept version back is redo of the snapshot writing
# out the relation's pages changed since its last snapshot: without it,
# the reset gives back the older page.  Redo finds those pages by walking
# the snapshot's own leaves, not the buffers, which the server log shows
# (no buffer pass) when no map page is newer than the replay.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('kept_pages_redo_flush');
$node->init;
$node->append_conf('postgresql.conf', qq{
wal_level = minimal
max_wal_senders = 0
wal_consistency_checking = all
shared_buffers = 1MB
autovacuum = off
prolly_fold_worker = off
checkpoint_timeout = 1h
bgwriter_lru_maxpages = 0
prolly.restore_min_changes = 0
});
# (roots sharing the relations' pages as they are: frozen ones share none)
$node->append_conf('postgresql.conf', 'prolly.freeze_roots = off');
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }
sub counted
{
	my ($name) = @_;
	return sql(qq{SELECT pg_stat_force_next_flush();
		SELECT value FROM pg_stat_prolly WHERE name = '$name'});
}

sql(q{
	CREATE SCHEMA app VERSIONED;
	CREATE TABLE app.t (id int PRIMARY KEY, v int, pad text) WITH (fillfactor = 10);
	INSERT INTO app.t SELECT g, 0, repeat('x', 100) FROM generate_series(1, 2000) g;
	SELECT dolt_commit('-Am', 'base');
	CREATE TABLE public.churn (a int, pad text) WITH (fillfactor = 10);
	INSERT INTO public.churn SELECT g, repeat('c', 300) FROM generate_series(1, 6000) g;
});
sql('CHECKPOINT');
my $digest = q{SELECT md5(string_agg(id || ':' || v, ',' ORDER BY id)) FROM app.t};

# the first page changed (its full image logged), then enough other pages
# that redo evicts it, then changed again and committed
sql(q{UPDATE app.t SET v = 1 WHERE id = 1});
sql(q{UPDATE public.churn SET a = a + 1});
sql(q{UPDATE app.t SET v = 2 WHERE id = 2});
sql(q{SELECT dolt_commit('-am', 'first page')});
my $at = sql($digest);

# the kept page changed: moved off the block the commit keeps
sql(q{UPDATE app.t SET v = 3 WHERE id = 3});
my $now = sql($digest);

my $log_offset = -s $node->logfile;
$node->append_conf('postgresql.conf', 'log_min_messages = debug1');
$node->stop('immediate');
$node->start;
is(sql($digest), $now, 'after a crash, the working set as it was');
ok( !$node->log_contains(
		'redo of a snapshot of .* writes out every buffer of it', $log_offset),
	"redo wrote out the snapshot's own pages, visiting no buffer");
$node->append_conf('postgresql.conf', 'log_min_messages = warning');
$node->reload;

my $restored = counted('relations_restored');
sql(q{SELECT dolt_reset('--hard')});
cmp_ok(counted('relations_restored'), '>', $restored,
	'the reset gave the kept pages back');
is(sql($digest), $at, "and they hold the commit's rows");


# The same in a relation of more pages than a leaf maps: its second
# subtree is a node over leaves, of which the commit's change copies the
# path to its second only (block 1100: a leaf maps 509).  Redo's walk goes
# down through the copied node to that leaf and passes over the first,
# still shared (by its count), and over the first subtree, shared whole.
sql(q{
	CREATE TABLE app.big (id int PRIMARY KEY, v int, pad text) WITH (fillfactor = 10);
	INSERT INTO app.big SELECT g, 0, repeat('x', 100) FROM generate_series(1, 7000) g;
	SELECT dolt_commit('-Am', 'big');
});
cmp_ok(sql(q{SELECT pg_relation_size('app.big') / 8192}), '>', 1200,
	'app.big spans leaves under a node');
sql('CHECKPOINT');
my $bigdigest = q{SELECT md5(string_agg(id || ':' || v, ',' ORDER BY id)) FROM app.big};
my @ids = split /\n/, sql(q{SELECT id FROM app.big
	WHERE (ctid::text::point)[0] = 1100 ORDER BY id LIMIT 3});
is(scalar @ids, 3, 'rows of block 1100');
sql(qq{UPDATE app.big SET v = 1 WHERE id = $ids[0]});
sql(q{UPDATE public.churn SET a = a + 1});
sql(qq{UPDATE app.big SET v = 2 WHERE id = $ids[1]});
sql(q{SELECT dolt_commit('-am', 'block 1100')});
$at = sql($bigdigest);
sql(qq{UPDATE app.big SET v = 3 WHERE id = $ids[2]});
$now = sql($bigdigest);
my $bigpath = sql(q{SELECT pg_relation_filepath('app.big')});

$log_offset = -s $node->logfile;
$node->append_conf('postgresql.conf', 'log_min_messages = debug1');
$node->stop('immediate');
$node->start;
is(sql($bigdigest), $now, 'after a crash, app.big as it was');
ok( !$node->log_contains(
		'redo of a snapshot of .* writes out every buffer of it', $log_offset),
	"redo wrote out app.big's own pages, visiting no buffer");

# the walk read the copied node and the leaf under it, and passed over the
# rest: the first subtree, and the node's first leaf, still shared
my ($pages, $nodes) = slurp_file($node->logfile, $log_offset) =~
  /redo of a snapshot of \Q$bigpath\E writes out its own pages: (\d+), found reading (\d+) nodes of its map/;
ok(defined $nodes, "redo walked app.big's snapshot");
is($nodes, 2, 'reading two of its four nodes');
cmp_ok($pages, '<', 20, 'and found a few own pages, not its 1200');
$node->append_conf('postgresql.conf', 'log_min_messages = warning');
$node->reload;
$restored = counted('relations_restored');
sql(q{SELECT dolt_reset('--hard')});
cmp_ok(counted('relations_restored'), '>', $restored,
	"the reset gave app.big's kept pages back");
is(sql($bigdigest), $at, "and they hold the commit's rows");

$node->stop;
done_testing();
