# Copyright (c) 2026, PostgreSQL Global Development Group

# Durability: a streaming standby replays engine tables exactly, serves
# them to hot-standby queries, and becomes a working primary when promoted.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use ProllyTest;

my $am = $ENV{PROLLY_AM} // 'prolly';
my $conf = qq(
default_table_access_method = '$am'
wal_consistency_checking = 'all'
);

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', $conf);
$primary->start;
setup_target($primary, 'postgres', $am);
$primary->safe_psql('postgres', q(
	CREATE TABLE rep (id int PRIMARY KEY, v text);
	INSERT INTO rep SELECT g, 'v' || g FROM generate_series(1, 1000) g;
));
$primary->backup('base');

my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'base', has_streaming => 1);
$standby->append_conf('postgresql.conf', $conf);
$standby->start;

$primary->safe_psql('postgres', q(
	INSERT INTO rep SELECT g, 'more' FROM generate_series(1001, 3000) g;
	UPDATE rep SET v = 'changed' WHERE id % 3 = 0;
	DELETE FROM rep WHERE id % 11 = 0;
	CREATE TABLE rep2 AS SELECT id FROM rep WHERE id < 100;
));
$primary->wait_for_replay_catchup($standby);

my $q = 'SELECT count(*), sum(id), count(*) FILTER (WHERE v = \'changed\') FROM rep';
is($standby->safe_psql('postgres', $q), $primary->safe_psql('postgres', $q),
	'standby has the same rows');
is($standby->safe_psql('postgres', 'SELECT count(*) FROM rep2'),
	$primary->safe_psql('postgres', 'SELECT count(*) FROM rep2'),
	'standby has the table created after the base backup');

$standby->promote;
$standby->safe_psql('postgres', "INSERT INTO rep VALUES (-1, 'promoted')");
is($standby->safe_psql('postgres', 'SELECT v FROM rep WHERE id = -1'),
	'promoted', 'promoted standby accepts writes');

for my $node ($primary, $standby)
{
	unlike(slurp_file($node->logfile), qr/inconsistent page found/,
		$node->name . ': no inconsistent pages in replay');
}
$standby->stop;
$primary->stop;
done_testing();
