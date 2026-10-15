# Copyright (c) 2026, PostgreSQL Global Development Group

# prolly_gc() reclaims the node store's unreachable nodes: those of
# replaced and dropped trees, large values included.  It keeps whatever a
# live root reaches, the root an older REPEATABLE READ snapshot reads among
# them; it waits for transactions writing nodes, prepared ones too; freed
# blocks are reused once no older snapshot remains; and the store stays
# consistent through a crash and on a standby.  Only roles with the
# privileges of pg_maintain may run it.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('gc');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', qq{
default_table_access_method = prolly_d1
max_prepared_transactions = 10
});
$node->start;
$node->backup('b');
my $standby = PostgreSQL::Test::Cluster->new('gc_standby');
$standby->init_from_backup($node, 'b', has_streaming => 1);
$standby->start;

my $store_size = q{SELECT size FROM pg_stat_file('global/9393')};
my $digest = q{SELECT count(*) || ':' || md5(string_agg(id || v, ',' ORDER BY id)) FROM g};

$node->safe_psql('postgres', q{
	CREATE TABLE g (id int PRIMARY KEY, v text);
	INSERT INTO g SELECT x, repeat('v', 100) FROM generate_series(1, 20000) x;
	CREATE TABLE big (id int PRIMARY KEY, t text);
	INSERT INTO big SELECT x, repeat(md5(x::text), 3000) FROM generate_series(1, 20) x;
});

# A snapshot from before the changes keeps reading its root.
my $s1 = $node->background_psql('postgres', on_error_stop => 0);
$s1->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM g;');
my $old = $s1->query_safe($digest);

$node->safe_psql('postgres', q{
	UPDATE g SET v = 'w' || v;
	DELETE FROM g WHERE id > 10000;
	DROP TABLE big;
});
my $now = $node->safe_psql('postgres', $digest);
my ($kept, $freed) = split /\|/, $node->safe_psql('postgres', 'SELECT * FROM prolly_gc()');
cmp_ok($freed, '>', 0, 'prolly_gc frees the nodes of replaced and dropped trees');
is($node->safe_psql('postgres', $digest), $now, 'the current rows are intact');
is($s1->query_safe($digest), $old,
	'an older snapshot still reads the root it saw');
$s1->query_safe('COMMIT');

# With that snapshot gone, its root's nodes are garbage too.
($kept, $freed) = split /\|/, $node->safe_psql('postgres', 'SELECT * FROM prolly_gc()');
cmp_ok($freed, '>', 1000, 'the old snapshot\'s nodes go once it ends');

# Freed blocks are reused once no older snapshot remains.
$node->safe_psql('postgres', 'SELECT txid_current()') for (1 .. 2);
my $size = $node->safe_psql('postgres', $store_size);
$node->safe_psql('postgres',
	q{INSERT INTO g SELECT x, 'z' FROM generate_series(30001, 34000) x});
is($node->safe_psql('postgres', $store_size), $size,
	'new nodes reuse freed blocks instead of growing the store');
is($node->safe_psql('postgres', 'SELECT count(*) FROM g'), '14000',
	'and read back');

# prolly_gc waits for a transaction that has written nodes (it spilled
# its writes), and for a prepared one.
my $s2 = $node->background_psql('postgres', on_error_stop => 0);
$s2->query_safe(q{SET prolly_write_set_memory = '64kB'; BEGIN;
	INSERT INTO g VALUES (-1, 'writer');
	INSERT INTO g SELECT x, repeat('s', 100) FROM generate_series(50001, 60000) x;});
my ($ret, $out, $err) = $node->psql('postgres',
	q{SET lock_timeout = '200ms'; SELECT * FROM prolly_gc();});
like($err, qr/lock timeout/, 'prolly_gc waits for a writer with nodes written');
$s2->query_safe(q{PREPARE TRANSACTION 'p';});
($ret, $out, $err) = $node->psql('postgres',
	q{SET lock_timeout = '200ms'; SELECT * FROM prolly_gc();});
like($err, qr/lock timeout/, 'and for a prepared transaction with writes');
$node->safe_psql('postgres', q{COMMIT PREPARED 'p'});
$node->safe_psql('postgres', 'SELECT * FROM prolly_gc()');
is($node->safe_psql('postgres', q{SELECT v || ':' || (SELECT count(*) FROM g WHERE id > 50000) FROM g WHERE id = -1}),
	'writer:10000', 'their writes survive the collection that waited for them');

# Through a crash, and on a standby.
$now = $node->safe_psql('postgres', $digest);
$node->stop('immediate');
$node->start;
is($node->safe_psql('postgres', $digest), $now, 'rows are intact after a crash');
($kept, $freed) = split /\|/, $node->safe_psql('postgres', 'SELECT * FROM prolly_gc()');
is($freed, '0', 'nothing is left to free after a collection survived a crash');
$node->wait_for_replay_catchup($standby);
is($standby->safe_psql('postgres', $digest), $now,
	'a standby reads the same rows');

# Privileges.
$node->safe_psql('postgres', 'CREATE ROLE nobody LOGIN');
($ret, $out, $err) = $node->psql('postgres', 'SELECT * FROM prolly_gc()',
	extra_params => [ '-U', 'nobody' ]);
like($err, qr/permission denied to reclaim prolly storage/,
	'prolly_gc needs pg_maintain');

$s1->quit;
$s2->quit;
$standby->stop;
$node->stop;
done_testing();
