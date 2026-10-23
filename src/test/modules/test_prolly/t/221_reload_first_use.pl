# Copyright (c) 2026, PostgreSQL Global Development Group

# A branch's first use that empties a table's copy and loads the head's
# rows into it (most of its rows differ): two sessions using the branch
# first at once make its copies once, and what the load wrote (the new
# storage, the rows, the indexes built after them) survives a crash, on
# the primary and on a standby replaying it.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('reload_first_use');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;
sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{CREATE EXTENSION amcheck;
	CREATE SCHEMA app VERSIONED;
	CREATE TABLE app.t (id int PRIMARY KEY, v text, w int);
	CREATE INDEX t_v ON app.t (v);
	CREATE INDEX t_w ON app.t (w);
	INSERT INTO app.t SELECT i, 'v' || i, i % 100 FROM generate_series(1, 20000) i;
	SELECT dolt_commit('-Am', 'base')});
my $head = sql(q{SELECT dolt_hashof('HEAD')});
sql(qq{SELECT dolt_branch('b', '$head');
	UPDATE app.t SET v = 'far' || id, w = w + 1;
	SELECT dolt_commit('-am', 'far')});

$node->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('reload_standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->start;

my $as_head = q{SELECT count(*) FILTER (WHERE v = 'v' || id AND w = id % 100)
	|| '/' || count(*) FROM app.t};
my $indexes = q{SELECT bool_and(bt_index_check(c.oid, true) IS NOT NULL)
	FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid
	WHERE i.indrelid = 'app.t'::regclass};

# Two first uses at once: one makes the copies, loading t; the other
# waits for it and finds them made.
sql(q{SELECT pg_stat_reset_shared('prolly')});
my $s1 = $node->background_psql('postgres');
my $s2 = $node->background_psql('postgres');
$s1->query_safe(q{BEGIN; SELECT dolt_checkout('b')});
$s2->query_until(qr/started/, q{
	\echo started
	SELECT dolt_checkout('b');
	\echo done
});
$s1->query_safe('COMMIT');
$s2->query_until(qr/done/, '');
is($s2->{stderr}, '', 'the second first use goes on once the first commits');
is($s1->query_safe($as_head), '20000/20000', 'the first session is at the head');
is($s2->query_safe($as_head), '20000/20000', 'and the second');
$s1->quit;
$s2->quit;
sql(q{SELECT pg_stat_force_next_flush()});
is(sql(q{SELECT value FROM pg_stat_prolly WHERE name = 'apply_loads'}), '1',
	't loaded once');
is($node->safe_psql('postgres/b', $indexes), 't', 'its indexes valid');

$node->wait_for_replay_catchup($standby);
is($standby->safe_psql('postgres/b', $as_head), '20000/20000',
	'the standby has the branch as its head');
is($standby->safe_psql('postgres/b', $indexes), 't', 'its indexes valid there');

$node->stop('immediate');
$node->start;
is($node->safe_psql('postgres/b', $as_head), '20000/20000', 'after a crash, b');
is($node->safe_psql('postgres/b', $indexes), 't', 'its indexes valid');
is(sql(q{SELECT count(*) FILTER (WHERE v = 'far' || id) FROM app.t}), '20000',
	'and main as it was');

done_testing();
