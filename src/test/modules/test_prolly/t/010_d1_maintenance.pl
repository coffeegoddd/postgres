# Copyright (c) 2026, PostgreSQL Global Development Group

# ANALYZE and CLUSTER on engine D1 tables: ANALYZE reads only the rows it
# samples, found by position; CLUSTER and VACUUM FULL take the table's
# trees and TIDs into the new storage, which survives a crash and which a
# standby replays.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $conf = "default_table_access_method = prolly_d1\n"
  . "wal_consistency_checking = 'Prolly,ProllyStore'\n";

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', $conf);
$primary->start;
$primary->safe_psql('postgres',
	q{SELECT pg_create_physical_replication_slot('standby')});
$primary->backup('base');

# The slot keeps the WAL the standby still needs through the primary's
# checkpoints and its crash below.
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'base', has_streaming => 1);
$standby->append_conf('postgresql.conf', $conf);
$standby->append_conf('postgresql.conf', "primary_slot_name = 'standby'\n");
$standby->start;

$primary->safe_psql('postgres', q{
	CREATE TABLE big (id int PRIMARY KEY, k int);
	INSERT INTO big SELECT g, g % 10 FROM generate_series(1, 100000) g;
	CREATE TABLE near (id int PRIMARY KEY);
	ALTER TABLE near ALTER id SET STATISTICS 10;
	INSERT INTO near SELECT generate_series(1, 5000);
	CREATE TABLE triples (a int, b text);
	ALTER TABLE triples ALTER a SET STATISTICS 10, ALTER b SET STATISTICS 10;
	INSERT INTO triples SELECT g % 40000, 'x' FROM generate_series(1, 120000) g;
});

# What ANALYZE VERBOSE says it read.
sub analyzed
{
	my ($table) = @_;
	my ($ret, $out, $err) =
	  $primary->psql('postgres', "ANALYZE VERBOSE $table");
	is($ret, 0, "ANALYZE $table");
	my ($line) = $err =~ /(read \d+ of \d+ items.*)$/m;
	return $line // $err;
}

is(analyzed('big'),
	'read 30000 of 100000 items, 30000 rows; 30000 rows in sample, 100000 estimated total rows',
	'ANALYZE reads only a sample of distinct rows');
is(analyzed('near'),
	'read 3000 of 5000 items, 3000 rows; 3000 rows in sample, 5000 estimated total rows',
	'a sample close to the table size is still exactly the target');
is(analyzed('triples'),
	'read 40000 of 40000 items, 120000 rows; 3000 rows in sample, 120000 estimated total rows',
	'a keyless table is read whole');

# CLUSTER and VACUUM FULL keep the rows' TIDs and the indexes; the copy
# survives a crash before any checkpoint, and the standby replays it.
$primary->safe_psql('postgres', q{
	CREATE INDEX big_k ON big USING prolly (k);
	CREATE INDEX big_kb ON big (k);
	DELETE FROM big WHERE id % 4 = 0;
});
my $state = q{
	SELECT count(*), sum(id), md5(string_agg(ctid::text, ',' ORDER BY id))
	FROM big;
};
my $by_index = q{
	SET enable_seqscan = off; SET enable_bitmapscan = off;
	SELECT (SELECT count(*) FROM big WHERE k = 3),
		   (SELECT count(*) FROM big WHERE k = 3 AND id < 50000);
};
my $before = $primary->safe_psql('postgres', $state);
my $index_before = $primary->safe_psql('postgres', $by_index);
$primary->safe_psql('postgres', 'CHECKPOINT');
$primary->safe_psql('postgres', 'VACUUM FULL big');
$primary->safe_psql('postgres', 'CLUSTER big USING big_kb');
is($primary->safe_psql('postgres', $state), $before,
	'VACUUM FULL and CLUSTER keep the rows and their TIDs');

$primary->stop('immediate');
$primary->start;
is($primary->safe_psql('postgres', $state), $before,
	'the copied table survives a crash');
is($primary->safe_psql('postgres', $by_index), $index_before,
	'its indexes agree after the crash');
$primary->safe_psql('postgres',
	'INSERT INTO big SELECT g, 3 FROM generate_series(200001, 200500) g');
is( $primary->safe_psql('postgres',
		'SELECT count(*) = count(DISTINCT ctid) FROM big'),
	't',
	'new rows get new TIDs after the copy and the crash');

$primary->wait_for_replay_catchup($standby);
is($standby->safe_psql('postgres', $state),
	$primary->safe_psql('postgres', $state),
	'the standby replays the copy');
is($standby->safe_psql('postgres', $by_index),
	$primary->safe_psql('postgres', $by_index),
	'the standby\'s indexes agree');

$primary->stop;
$standby->stop;
ok(slurp_file($primary->logfile) !~ /inconsistent page/
	  && slurp_file($standby->logfile) !~ /inconsistent page/,
	'no inconsistent pages in replay');

done_testing();
