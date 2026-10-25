# Copyright (c) 2026, PostgreSQL Global Development Group

# A hot standby's transaction holding pointers read from history (long
# values) is protected from the primary's collections: the replay of a
# swap cancels it as a recovery conflict before the store stops naming
# what it holds, never leaving it to find a chunk missing.  One begun on
# the standby and still running when it is promoted is waited for by the
# new primary's collection, and reads every value.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('standby_gc');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;
sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{CREATE EXTENSION test_prolly; CREATE EXTENSION injection_points;
	CREATE SCHEMA sg VERSIONED;
	CREATE TABLE sg.k (id int PRIMARY KEY, j jsonb);
	SELECT dolt_commit('-Am', 'empty')});

# a branch whose history alone names its long values: they are dropped by
# a full collection once the branch is
sub side_branch
{
	my ($name) = @_;
	sql(qq{SELECT dolt_branch('$name');
		SELECT dolt_checkout('$name');
		INSERT INTO sg.k SELECT g, jsonb_build_object('a',
				(SELECT string_agg(md5(g || '$name' || i), '') FROM generate_series(1, 200) i),
				'g', g)
			FROM generate_series(1, 5) g;
		SELECT dolt_commit('-am', 'values');
		DELETE FROM sg.k;
		SELECT dolt_commit('-am', 'gone');
		SELECT dolt_checkout('main')});
}
my $read = q{SELECT id, md5(j::text) FROM
	(SELECT id, j FROM dolt_at('%s~1', NULL::sg.k) ORDER BY id) s};

side_branch('side');
is(sql(q{SELECT count(*) FROM dolt_at('side~1', NULL::sg.k)
	WHERE prolly_jsonb_tree_root(j) IS NOT NULL}), '5', 'the values are JSON trees');
my $want = sql(sprintf($read, 'side'));

$node->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('standby_gc_standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->append_conf('postgresql.conf', qq{
hot_standby_feedback = on
max_standby_streaming_delay = 1s
log_error_verbosity = verbose
});
$standby->start;
$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));

# a standby cursor holding the values' pointers (the sort has them all)
my $offset = -s $standby->logfile;
my $c = $standby->background_psql('postgres', on_error_stop => 0);
$c->query_safe(q{\set VERBOSITY verbose});
$c->query_safe(sprintf(q{BEGIN ISOLATION LEVEL REPEATABLE READ;
	DECLARE c CURSOR FOR %s; FETCH 1 FROM c}, sprintf($read, 'side')));

# the primary drops them, its collection held after its swap
my $old = sql(q{SELECT prolly_jsonb_tree_root(j) FROM dolt_at('side~1', NULL::sg.k)
	ORDER BY id DESC LIMIT 1});
sql(q{SELECT dolt_branch('-D', 'side')});
sql('VACUUM');	# (the branch's pages let go of)
my $g = $node->background_psql('postgres');
$g->query_safe(q{SELECT injection_points_set_local();
	SELECT injection_points_attach('prolly-gc-retire', 'wait')});
$g->query_until(qr/started/, q{
	\echo started
	SELECT dolt_gc('--full') IS NOT NULL;
	\echo done
});
$node->poll_query_until('postgres', q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE wait_event = 'prolly-gc-retire'}) or die 'the collection did not stop at its retire';
$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));
# (another session's look for what was dropped: the store forgets it)
is($standby->safe_psql('postgres', qq{SELECT test_chunk_has('$old')}), 'f',
	'the swap replayed, the store no longer has a value');

# (cancelled idle in its transaction, its session ends)
eval { $c->query(q{FETCH ALL FROM c}); };
my $log = slurp_file($standby->logfile, $offset);
like($log, qr/FATAL:  40001: terminating connection due to conflict with recovery/,
	'the standby cursor is cancelled as a recovery conflict at the swap');
unlike($c->{stderr}, qr/tree node missing/, 'not left to find its chunks missing');
eval { $c->quit; };
unlike(slurp_file($standby->logfile), qr/tree node missing/,
	'no standby read found a chunk missing');

sql(q{SELECT injection_points_wakeup('prolly-gc-retire');
	SELECT injection_points_detach('prolly-gc-retire')});
$g->query_until(qr/done/, '');
$g->quit;

# begun on the standby, running at its promotion: the new primary's
# collection waits for it, and it reads every value
side_branch('side2');
$want = sql(sprintf($read, 'side2'));
$old = sql(q{SELECT prolly_jsonb_tree_root(j) FROM dolt_at('side2~1', NULL::sg.k)
	ORDER BY id DESC LIMIT 1});
$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));
$c = $standby->background_psql('postgres', on_error_stop => 0);
$c->query_safe(sprintf(q{BEGIN ISOLATION LEVEL REPEATABLE READ;
	DECLARE c CURSOR FOR %s; FETCH 1 FROM c}, sprintf($read, 'side2')));
$node->stop;
$standby->promote;
$standby->poll_query_until('postgres', 'SELECT NOT pg_is_in_recovery()')
  or die 'the standby was not promoted';
$standby->safe_psql('postgres', q{SELECT dolt_branch('-D', 'side2')});
$standby->safe_psql('postgres', 'VACUUM');
$g = $standby->background_psql('postgres');
$g->query_until(qr/started/, q{
	\echo started
	SELECT dolt_gc('--full') IS NOT NULL;
	\echo done
});
$standby->poll_query_until('postgres', q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE wait_event = 'virtualxid'})
  or die 'the collection did not wait for the transaction begun on the standby';
is($standby->safe_psql('postgres', qq{SELECT test_chunk_has('$old')}), 't',
	'the collection has dropped nothing yet');
my $rest = $c->query(q{FETCH ALL FROM c});
is($c->{stderr}, '', 'the transaction begun on the standby reads on');
is((split /\n/, $want, 2)[1], $rest, 'every value, as the primary had it');
$c->query_safe('COMMIT');
$c->quit;
$g->query_until(qr/done/, '');
$g->quit;
is($standby->safe_psql('postgres', q{SELECT count(*) FROM dolt_branches WHERE name = 'side2'}),
	'0', 'and the collection finished once it ended');

$standby->stop;
done_testing();
