# Copyright (c) 2026, PostgreSQL Global Development Group

# VACUUM of a prolly table removes row versions no snapshot can still
# see: their entries in other index AMs, then their TIDs, which new rows
# reuse.  A reused TID is never reached through an index entry of the
# version that had it.  A version an open snapshot can still see stays.
# The removal and the reuse survive a crash and replay on a standby.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('vacuum');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', "default_table_access_method = prolly_d1\n"
	. "wal_consistency_checking = 'Prolly,ProllyStore,Generic,Btree'\n");
$node->start;
$node->backup('b');
my $standby = PostgreSQL::Test::Cluster->new('vacuum_standby');
$standby->init_from_backup($node, 'b', has_streaming => 1);
$standby->start;

$node->safe_psql('postgres', q{
	CREATE TABLE vt (id int PRIMARY KEY, v int);
	CREATE INDEX vt_v ON vt USING btree (v);
	INSERT INTO vt SELECT x, x FROM generate_series(1, 1000) x;
});

# A snapshot from before the update keeps the versions it sees.
my $s1 = $node->background_psql('postgres');
$s1->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM vt;');
$node->safe_psql('postgres', 'UPDATE vt SET v = v + 10000');
my $max_before = $node->safe_psql('postgres', 'SELECT max(ctid) FROM vt');
$node->safe_psql('postgres', 'VACUUM vt');
is($s1->query_safe(q{SET enable_seqscan = off; SELECT count(*) FROM vt WHERE v <= 1000;}),
	'1000', 'VACUUM keeps the versions an open snapshot sees');
$s1->query_safe('COMMIT');

# Once it ends, they go, and their TIDs are reused.
$node->safe_psql('postgres', 'VACUUM vt');
$node->safe_psql('postgres', 'SELECT txid_current()') for (1 .. 2);
$node->safe_psql('postgres',
	q{INSERT INTO vt SELECT x, 50000 + x FROM generate_series(2001, 2500) x});
is($node->safe_psql('postgres',
	qq{SELECT (SELECT max(ctid) FROM vt WHERE id > 2000) < '$max_before'::tid}),
	't', 'new rows reuse the TIDs VACUUM freed');
is($node->safe_psql('postgres',
	q{SET enable_seqscan = off; SET enable_bitmapscan = off;
	  SELECT count(*) FROM vt WHERE v <= 1000;}),
	'0', 'no index entry of a removed version reaches a reused TID');
is($node->safe_psql('postgres',
	q{SET enable_seqscan = off; SELECT count(*) FROM vt WHERE v > 10000;}),
	'1500', 'every current row is found through the index');

# Through a crash, and on a standby.
my $digest = q{SELECT count(*) || ':' || sum(v) || ':' || max(ctid) FROM vt};
my $now = $node->safe_psql('postgres', $digest);
$node->stop('immediate');
$node->start;
is($node->safe_psql('postgres', $digest), $now, 'rows and TIDs survive a crash');
$node->safe_psql('postgres',
	q{INSERT INTO vt SELECT x, 60000 + x FROM generate_series(3001, 3100) x});
$node->wait_for_replay_catchup($standby);
is($standby->safe_psql('postgres', $digest),
	$node->safe_psql('postgres', $digest), 'a standby replays them');

$s1->quit;
$standby->stop;
$node->stop;
done_testing();
