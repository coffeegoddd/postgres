
# Copyright (c) 2026, PostgreSQL Global Development Group

# A VACUUM after a reset sets the bits of the pages a frozen root's restore
# cleared with XLOG_HEAP2_VM_SET records, of the visibility map page alone.
# Logical decoding passes over them as it does over any physical heap2
# record: a slot reading past them decodes the changes after.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('vm_set_logical_decoding');
$node->init(allows_streaming => 'logical');
$node->append_conf('postgresql.conf', q{
autovacuum = off
prolly_fold_worker = off
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{CREATE EXTENSION pg_visibility;
	CREATE EXTENSION pg_walinspect;
	CREATE TABLE plain (id int PRIMARY KEY);
	CREATE SCHEMA vd VERSIONED;
	CREATE TABLE vd.t (id int PRIMARY KEY, v int NOT NULL, w text NOT NULL)
	  WITH (fillfactor = 50);
	INSERT INTO vd.t SELECT g, 0, repeat('w', 40) FROM generate_series(1, 5000) g;
	VACUUM (FREEZE) vd.t;
	SELECT dolt_commit('-Am', 'c1')});
sql(q{UPDATE vd.t SET v = 1 WHERE id <= 2500; SELECT dolt_commit('-am', 'c2')});

sql(q{SELECT pg_create_logical_replication_slot('s', 'test_decoding')});
sql(q{SELECT dolt_reset('--hard', 'HEAD~1')});
ok(sql(q{SELECT count(*) FROM pg_visibility_map('vd.t')
	WHERE NOT (all_visible AND all_frozen)}) > 0,
	'the reset cleared the changed pages\' bits');

my $lsn0 = sql('SELECT pg_current_wal_insert_lsn()');
sql('VACUUM vd.t');
my $lsn1 = sql('SELECT pg_current_wal_insert_lsn()');
ok(sql(qq{SELECT count(*) FROM pg_get_wal_records_info('$lsn0', '$lsn1')
	WHERE resource_manager = 'Heap2' AND record_type = 'VM_SET'}) > 0,
	'the VACUUM wrote VM_SET records');
sql(q{INSERT INTO plain VALUES (42)});

my ($ret, $out, $err) = $node->psql('postgres',
	q{SELECT data FROM pg_logical_slot_get_changes('s', NULL, NULL,
	  'include-xids', '0', 'skip-empty-xacts', '1')});
is($ret, 0, 'the slot decodes past the VM_SET records') or diag($err);
like($out, qr/table public\.plain: INSERT: id\[integer\]:42/,
	'and the insert after them');

$node->stop;
done_testing();
