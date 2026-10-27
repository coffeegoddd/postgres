# Copyright (c) 2026, PostgreSQL Global Development Group

# A standby replays a sever: its horizon is the primary's, its log stops
# there, and once the primary's full collection frees the severed history
# the standby says a severed commit is past the horizon.  A plan reads on
# the standby, holding nothing.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init(allows_streaming => 1);
$node->start;
$node->safe_psql('postgres', 'CREATE DATABASE hs');
$node->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->start;

sub sql { return $node->safe_psql('hs', $_[0]); }
sub ssql { return $standby->safe_psql('hs', $_[0]); }

sql(q{CREATE SCHEMA hs VERSIONED;
	CREATE TABLE hs.t (id int PRIMARY KEY, v text);
	INSERT INTO hs.t SELECT g, 'v' FROM generate_series(1, 200) g;});
for my $d (1 .. 6)
{
	sql(qq{UPDATE hs.t SET v = 'd$d' WHERE id % 7 = $d;
		SELECT dolt_commit('-Am', 'd$d', '--date', '2020-01-0$d')});
}
sql(q{SELECT commits FROM dolt_history_drop('2020-01-04')});
my $top = sql(q{SELECT tops[1] FROM prolly_history_horizon()});
$node->safe_psql('hs', 'SELECT * FROM prolly_gc(true)');
$node->wait_for_catchup($standby);

is(ssql(q{SELECT count(*) FROM prolly_history_horizon()}), '1',
	'the standby has the horizon');
is(ssql(q{SET client_min_messages = warning;
	SELECT count(*) FROM dolt_log('main')}),
	sql(q{SET client_min_messages = warning;
	SELECT count(*) FROM dolt_log('main')}),
	'its log stops where the primary\'s does');
cmp_ok(ssql(q{SELECT count(*) FROM prolly_history_plan('2020-01-05')}), '>', 0,
	'a plan reads on the standby');
my ($ret, $out, $err) = $standby->psql('hs', qq{SELECT prolly_root_hash('$top')});
like($err, qr/is past the history horizon/,
	'the standby says a severed commit is past the horizon');

done_testing();
