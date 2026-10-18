# Copyright (c) 2026, PostgreSQL Global Development Group

# Large values in versioned tables: committed out of line, each long value
# in a value tree of its own, they come back from history through a crash
# and on a standby, and a few bytes inserted into one commit a few nodes.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('large_values');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;
$node->backup('b');
my $standby = PostgreSQL::Test::Cluster->new('large_values_standby');
$standby->init_from_backup($node, 'b', has_streaming => 1);
$standby->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE EXTENSION test_prolly;
	CREATE SCHEMA lv VERSIONED;
	CREATE TABLE lv.big (id int PRIMARY KEY, t text, b bytea);
	INSERT INTO lv.big
	SELECT g, string_agg(md5(g || ':' || i), '' ORDER BY i),
		decode(repeat(to_hex(g * 17), 30000), 'hex')
	FROM generate_series(1, 5) g, generate_series(1, 2000 * g) i GROUP BY g;
	CREATE TABLE lv.keyless (t text);
	INSERT INTO lv.keyless SELECT repeat('k', 100000) FROM generate_series(1, 3);
});
my $digest = q{SELECT string_agg(id || ':' || length(t) || ':' || md5(t) || ':' || md5(b), ',' ORDER BY id) FROM lv.big};
my $before = sql($digest);
like($before, qr/^1:64000:/, 'rows with long values are stored');
my $c1 = sql(q{SELECT (dolt_commit('-Am', 'large'))[1]});

sql(q{UPDATE lv.big SET t = overlay(t PLACING 'XYZ' FROM 96000 FOR 0) WHERE id = 3});
my $after = sql($digest);
my $c2 = sql(q{SELECT (dolt_commit('-am', 'an insertion'))[1]});
cmp_ok(sql(qq{SELECT test_walk_new_count('$c1', '$c2')}), '<', 20,
	'three bytes inserted into a 192 KB value commit a few nodes');

# Through a crash: history is read back whole.
$node->stop('immediate');
$node->start;
sql(qq{SELECT dolt_reset('--hard', '$c1')});
is(sql($digest), $before, 'the first commit\'s values after a crash');
sql(q{DELETE FROM lv.keyless});
sql(qq{SELECT dolt_reset('--hard', '$c2')});
is(sql($digest), $after, 'and the second\'s');
is(sql(q{SELECT count(*), sum(length(t)) FROM lv.keyless}), '3|300000',
	'identical long rows of a keyless table');

# On a standby, history is read from the replayed chunk store.
$node->wait_for_replay_catchup($standby);
is( $standby->safe_psql('postgres',
		qq{SELECT diff_type || ':' || length((from_row).t) || ':' || length((to_row).t)
		   FROM dolt_diff('$c1', '$c2', NULL::lv.big)}),
	'modified:192000:192003',
	'a standby reads the values from history');

$standby->stop;
$node->stop;
done_testing();
