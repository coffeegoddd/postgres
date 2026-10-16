# Copyright (c) 2026, PostgreSQL Global Development Group

# The chunk store: chunks are seen by their transaction at once and by
# others once it commits; the root moves with its transaction, one mover at
# a time.  Recovery stopped at every record boundary has the root of the
# last commit before it, a standby's journal is the primary's byte for
# byte, and a base backup restores the store.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('chunks');
$node->init(allows_streaming => 1, has_archiving => 1);
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_prolly; CREATE EXTENSION pg_walinspect');
$node->backup('base');

my $standby = PostgreSQL::Test::Cluster->new('chunks_standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

my $start = sql('SELECT pg_current_wal_insert_lsn()');
my @moves;		# the roots committed, in order

# A chunk is its transaction's until it commits; an aborted one's is gone.
my $a = $node->background_psql('postgres');
my $h1 = $a->query_safe(q{BEGIN; SELECT test_chunk_put('one')});
is($a->query_safe("SELECT convert_from(test_chunk_get('$h1'), 'UTF8')"), 'one',
	'its transaction reads a chunk at once');
is(sql("SELECT test_chunk_has('$h1')"), 'f', 'others don\'t, before it commits');
$a->query_safe('COMMIT');
is(sql("SELECT convert_from(test_chunk_get('$h1'), 'UTF8')"), 'one', 'and do after');
sql(q{BEGIN; SELECT test_chunk_put('aborted'); ROLLBACK});
is(sql(q{SELECT test_chunk_has(encode(sha256('aborted'), 'hex'))}), 'f',
	'an aborted transaction\'s chunk is gone');
is(sql("SELECT test_chunk_put('one')"), $h1, 'putting a chunk again names it the same');

# The root.
is(sql('SELECT test_chunk_root() IS NULL'), 't', 'a new store has no root');
is(sql("SELECT test_chunk_move_root(NULL, '$h1')"), 't', 'the root moves');
push @moves, $h1;
is(sql('SELECT test_chunk_root()'), $h1, 'to the chunk');
my $h2 = sql(q{SELECT test_chunk_put('two')});
my $h3 = sql(q{SELECT test_chunk_put('three')});
is(sql("SELECT test_chunk_move_root('$h2', '$h3')"), 'f',
	'a move expecting another root fails');

$a->query_safe("BEGIN; SELECT test_chunk_move_root('$h1', '$h2')");
is($a->query_safe('SELECT test_chunk_root()'), $h2, 'the mover sees its move');
is(sql('SELECT test_chunk_root()'), $h1, 'others don\'t, before it commits');
my ($ret, $out, $err) = $node->psql('postgres',
	"SET lock_timeout = '1s'; SELECT test_chunk_move_root('$h1', '$h3')");
like($err, qr/lock timeout/, 'another mover waits for it');
$a->query_safe('ROLLBACK');
is(sql('SELECT test_chunk_root()'), $h1, 'a rolled back move is gone');
is(sql("SELECT test_chunk_move_root('$h1', '$h3')"), 't', 'the next mover moves it');
push @moves, $h3;
$a->query_safe("BEGIN; SELECT test_chunk_move_root('$h3', '$h2'); COMMIT");
push @moves, $h2;
is(sql('SELECT test_chunk_root()'), $h2, 'moves follow each other');

# Many chunks, more than one append's worth, with a move.
my $big = sql(q{
	BEGIN;
	SELECT count(test_chunk_put(convert_to(repeat(g::text, 20000), 'UTF8')))
	  FROM generate_series(1, 120) g;
	SELECT test_chunk_move_root(test_chunk_root(),
		test_chunk_put(convert_to(repeat('120', 20000), 'UTF8')));
	COMMIT;
});
my $bigroot = sql('SELECT test_chunk_root()');
push @moves, $bigroot;
my $all = q{SELECT count(*) FROM generate_series(1, 120) g
	WHERE convert_from(test_chunk_get(encode(sha256(convert_to(repeat(g::text, 20000),
		'UTF8')), 'hex')), 'UTF8') = repeat(g::text, 20000)};
is(sql($all), '120', 'every chunk reads back');
sql('CHECKPOINT');
my $after_ckpt = sql(q{SELECT test_chunk_put('after checkpoint')});
my $end = sql('SELECT pg_current_wal_insert_lsn()');

# A crash keeps what was committed.
$node->stop('immediate');
$node->start;
is(sql('SELECT test_chunk_root()'), $bigroot, 'the root survives a crash');
is(sql($all), '120', 'and the chunks');
is(sql("SELECT test_chunk_has('$after_ckpt')"), 't', 'appended after the checkpoint too');

# The standby's journal is the primary's.
$node->wait_for_replay_catchup($standby);
my $file = q{SELECT md5(pg_read_binary_file('base/' ||
	(SELECT oid FROM pg_database WHERE datname = 'postgres') || '/prolly_journal'))};
is($standby->safe_psql('postgres', $file), sql($file),
	'a standby\'s journal is the primary\'s, byte for byte');
is($standby->safe_psql('postgres', 'SELECT test_chunk_root()'), $bigroot,
	'and so is its root');

# (every segment archived, for recovery from the archive)
my $seg = sql('SELECT pg_walfile_name(pg_current_wal_lsn())');
sql('SELECT pg_switch_wal()');
$node->poll_query_until('postgres',
	"SELECT last_archived_wal >= '$seg' FROM pg_stat_archiver")
  or die 'the WAL was not archived';

# Recovery stopped at each record boundary: the root is the last committed
# one's.  The commit records carrying the moves are matched to them in
# order.
my @boundaries = split /\n/, sql(qq{
	SELECT end_lsn FROM pg_get_wal_records_info('$start', '$end')
	WHERE resource_manager IN ('ProllyChunks', 'Transaction') ORDER BY end_lsn});
my @move_ends = split /\n/, sql(qq{
	SELECT end_lsn FROM pg_get_wal_records_info('$start', '$end')
	WHERE resource_manager = 'Transaction'
	  AND description LIKE '%chunk store root move%' ORDER BY end_lsn});
is(scalar(@move_ends), scalar(@moves), 'each move\'s commit record carries it');
cmp_ok(scalar(@boundaries), '>', 10, 'records to stop at');
my $i = 0;
my $bad = 0;
for my $lsn (@boundaries)
{
	my $want = '';
	for my $m (0 .. $#move_ends)
	{
		$want = $moves[$m]
		  if sql("SELECT '$move_ends[$m]'::pg_lsn <= '$lsn'::pg_lsn") eq 't';
	}
	# (not inclusive: replay stops before the record starting at the target,
	# the boundary, so just after the one ending there)
	my $pitr = PostgreSQL::Test::Cluster->new('chunks_pitr' . ++$i);
	$pitr->init_from_backup($node, 'base', has_restoring => 1);
	$pitr->append_conf('postgresql.conf', qq{
recovery_target_lsn = '$lsn'
recovery_target_inclusive = off
recovery_target_action = 'promote'
recovery_target_timeline = 'current'
archive_mode = off
});
	$pitr->start;
	$pitr->poll_query_until('postgres', 'SELECT NOT pg_is_in_recovery()')
	  or die "recovery to $lsn did not end";
	my $got = $pitr->safe_psql('postgres', 'SELECT coalesce(test_chunk_root(), \'\')');
	if ($got ne $want)
	{
		diag("stopped at $lsn: root <$got>, expected <$want>");
		$bad++;
	}
	elsif ($want ne '')
	{
		# the root's chunk is there
		$bad++
		  unless $pitr->safe_psql('postgres', "SELECT test_chunk_has('$want')") eq 't';
	}
	$pitr->stop;
}
is($bad, 0, 'recovery to every record boundary has the last committed root');

# A base backup restores the store.
$node->backup('later');
my $restored = PostgreSQL::Test::Cluster->new('chunks_restored');
$restored->init_from_backup($node, 'later');
$restored->start;
is($restored->safe_psql('postgres', 'SELECT test_chunk_root()'), $bigroot,
	'a base backup has the root');
is($restored->safe_psql('postgres', $all), '120', 'and the chunks');
$restored->stop;

$standby->stop;
$node->stop;
done_testing();
