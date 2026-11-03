
# Copyright (c) 2026, PostgreSQL Global Development Group

# Version control's history across restarts, and on a hot standby: commits,
# branches and their rows survive the server stopping (immediately, its WAL
# replayed); a table of 20,000 rows written first, its old commits' trees,
# read back whole after restarts as more is committed; what a push, a fetch
# and a pull that merges sent is there after a restart; history discarded
# and freed is gone, and stays gone, on the primary and its standby; and a
# hot standby reads the history as the primary has it, refuses to write it,
# and follows as the primary goes on.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('vc_primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', 'autovacuum = off');
$primary->start;
$primary->safe_psql('postgres', q{
	CREATE EXTENSION test_prolly;
	SELECT test_vc_open();
	SELECT test_vc_bench_load('main', 20000, 10000);
});
$primary->safe_psql('postgres', "SELECT test_vc_commit('main', '$_')")
  for qw(base a b c);
$primary->safe_psql('postgres', q{
	SELECT test_vc_branch_at('side', 'main', 'b');
	SELECT test_vc_commit('side', 's1');
});

# what the history is: the branches' lines, their rows, a row each commit wrote
my $state = sub {
	my ($node) = @_;
	return $node->safe_psql('postgres', q{
		SELECT test_vc_log('main'), test_vc_log('side'),
			test_vc_rows('main'), test_vc_rows('side'),
			test_vc_working_has('main', 'base'), test_vc_working_has('side', 's1')});
};
like($primary->safe_psql('postgres', q{SELECT test_vc_log('main')}),
	qr/^c b a base load/, 'main\'s line');
like($primary->safe_psql('postgres', q{SELECT test_vc_log('side')}),
	qr/^s1 b a base load/, 'side\'s line, from b');
is($primary->safe_psql('postgres', q{SELECT test_vc_rows('side')}),
	'20004', 'side\'s rows');

# clones into memory, three in one session: each pushes a commit back, has
# another fetched from it, and one pulled, merged with one of main's own
my @fetched = split /\n/, $primary->safe_psql('postgres', q{
	SELECT encode(test_vc_round_trip('main', 'r' || i), 'hex') FROM generate_series(1, 3) i});
is(scalar(@fetched), 3, 'three round trips in one session');
my $fetched = $fetched[2];
like($primary->safe_psql('postgres', q{SELECT test_vc_log('main')}),
	qr/^merge r3 ours r3 pushed merge r2 ours r2 pushed merge r1 ours r1 pushed c /,
	'main\'s line: each round its push, its own commit, the merge');
my $before = $state->($primary);

# stopped at once: what was committed is there, from its WAL
$primary->stop('immediate');
$primary->start;
is($state->($primary), $before, 'the history after an immediate stop');
is( $primary->safe_psql(
		'postgres',
		qq{SELECT test_cs_has('pages', decode('$fetched', 'hex')),
			test_vc_working_has('main', 'r1 pushed'), test_vc_working_has('main', 'r3 theirs')}),
	't|t|t',
	'what the push, the fetch and the pull sent, after it');
is($primary->safe_psql('postgres', q{SELECT ok FROM test_cs_check('pages')}),
	't', 'the store checks');

# rows written first read back right after restarts, as more is committed:
# side's tree whole, all of it written before the restarts, and main's,
# most of it shared with its first commits (each restart's reads the
# disk's, not the buffers')
my $rows = $primary->safe_psql('postgres', q{SELECT test_vc_rows('main')});
for my $i (1 .. 3)
{
	$primary->safe_psql('postgres', qq{SELECT test_vc_commit('main', 'more $i')});
	$primary->restart;
	is( $primary->safe_psql(
			'postgres',
			qq{SELECT test_vc_working_has('main', 'base'), test_vc_working_has('main', 'more $i'),
				test_vc_rows('side'), test_vc_rows('main')}),
		't|t|20004|' . ($rows + $i),
		"old and new rows after restart $i");
}
like($primary->safe_psql('postgres', q{SELECT test_vc_log('main')}),
	qr/^more 3 more 2 more 1 /, 'main\'s line after the restarts');

# a hot standby, from a backup: it reads the history as the primary has it
$primary->backup('b');
my $standby = PostgreSQL::Test::Cluster->new('vc_standby');
$standby->init_from_backup($primary, 'b', has_streaming => 1);
$standby->start;
$primary->wait_for_replay_catchup($standby);
is($state->($standby), $state->($primary), 'the standby reads the history');
my ($ret, $out, $err) = $standby->psql('postgres', q{SELECT test_vc_commit('main', 'nope')});
isnt($ret, 0, 'a commit on the standby is refused');
like($err, qr/read-only transaction/, '(as read only)');
$primary->safe_psql('postgres', q{SELECT test_vc_commit('main', 'later')});
$primary->wait_for_replay_catchup($standby);
like($standby->safe_psql('postgres', q{SELECT test_vc_log('main')}),
	qr/^later more 3 /, 'the standby follows the primary');

# history discarded and freed is gone, and stays gone after a restart
my $held = sub {
	return [
		split /\n/,
		$primary->safe_psql(
			'postgres',
			q{SELECT encode(hash, 'hex') FROM pg_history.pointers
			WHERE name LIKE 'vc/commit/%' ORDER BY 1})
	];
};
my @was = @{ $held->() };
ok($primary->safe_psql('postgres', 'SELECT test_vc_sever(2, true)') > 0,
	'a window of two cuts commits, discarded');
my %now = map { $_ => 1 } @{ $held->() };
my @cut = grep { !$now{$_} } @was;
ok(scalar(@cut) > 0, scalar(@cut) . ' commits cut');
$primary->safe_psql('postgres', 'VACUUM pg_history.chunk_index');
ok( $primary->poll_query_until(
		'postgres',
		q{SELECT unheld = 0 AND test_chunk_index_marked('pg_history.chunk_index') = 0
		FROM test_cs_check('pages')}),
	'what the discard let go of freed');
my $gone = sub {
	my ($node) = @_;
	return $node->safe_psql('postgres',
		'SELECT bool_and(NOT test_cs_has(\'pages\', decode(h, \'hex\'))) FROM unnest(ARRAY['
		  . join(',', map { "'$_'" } @cut) . ']) h');
};
is($gone->($primary), 't', 'the cut commits are gone from the store');
$primary->restart;
is($gone->($primary), 't', 'and stay gone after a restart');
is( $primary->safe_psql(
		'postgres',
		q{SELECT test_vc_working_has('main', 'later'), test_vc_working_has('side', 's1'),
			(SELECT ok FROM test_cs_check('pages'))}),
	't|t|t',
	'the heads read, the store checks');
$primary->wait_for_replay_catchup($standby);
is( $standby->safe_psql(
		'postgres',
		q{SELECT test_vc_working_has('main', 'later'), test_vc_rows('side')}),
	't|20004', 'the standby reads the heads too');
is($gone->($standby), 't', 'and has the cut commits no more');

$standby->stop;
$primary->stop;
done_testing();
