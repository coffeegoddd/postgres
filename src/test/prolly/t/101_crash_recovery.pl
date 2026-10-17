# Copyright (c) 2026, PostgreSQL Global Development Group

# Durability: after a crash, committed rows on engine tables are there,
# uncommitted ones aren't, indexes agree with the table, and a prepared
# transaction survives to be committed.  Every page replayed is checked
# against the primary's image (wal_consistency_checking).

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use ProllyTest;

my $am = $ENV{PROLLY_AM} // 'prolly';

my $node = PostgreSQL::Test::Cluster->new('crash');
$node->init;
$node->append_conf('postgresql.conf', qq(
default_table_access_method = '$am'
max_prepared_transactions = 10
wal_consistency_checking = 'all'
));
$node->start;
setup_target($node, 'postgres', $am);

$node->safe_psql('postgres', q(
	CREATE TABLE dur (id int PRIMARY KEY, v text);
	CREATE INDEX dur_v ON dur (v);
	INSERT INTO dur SELECT g, 'v' || g FROM generate_series(1, 5000) g;
	CHECKPOINT;
	UPDATE dur SET v = 'updated' WHERE id % 10 = 0;
	DELETE FROM dur WHERE id % 7 = 0;
	INSERT INTO dur SELECT g, 'late' FROM generate_series(5001, 6000) g;
	BEGIN;
	INSERT INTO dur VALUES (-1, 'prepared');
	PREPARE TRANSACTION 'survivor';
));

# An uncommitted transaction open at the crash.
my $open = $node->background_psql('postgres');
$open->query_safe('BEGIN; INSERT INTO dur VALUES (-2, \'uncommitted\');');

my $expect = $node->safe_psql('postgres',
	'SELECT count(*), sum(id), count(*) FILTER (WHERE v = \'updated\') FROM dur');

$node->stop('immediate');
$open->quit;
$node->start;

is($node->safe_psql('postgres',
	'SELECT count(*), sum(id), count(*) FILTER (WHERE v = \'updated\') FROM dur'),
	$expect, 'committed rows survive a crash; uncommitted ones are gone');
is($node->safe_psql('postgres', 'SELECT count(*) FROM dur WHERE id = -2'),
	'0', 'the uncommitted insert is gone');
is($node->safe_psql('postgres', q(
	SET enable_seqscan = off;
	SELECT count(*) FROM dur WHERE v = 'updated')),
	$node->safe_psql('postgres', q(
	SET enable_indexscan = off; SET enable_bitmapscan = off;
	SELECT count(*) FROM dur WHERE v = 'updated')),
	'index and table agree after recovery');
is($node->safe_psql('postgres', 'SELECT gid FROM pg_prepared_xacts'),
	'survivor', 'the prepared transaction survives');
$node->safe_psql('postgres', "COMMIT PREPARED 'survivor'");
is($node->safe_psql('postgres', 'SELECT v FROM dur WHERE id = -1'),
	'prepared', 'the prepared transaction commits after recovery');

# Writes after recovery work, and a second crash recovers them too.
$node->safe_psql('postgres', "INSERT INTO dur VALUES (-3, 'after')");
$node->stop('immediate');
$node->start;
is($node->safe_psql('postgres', 'SELECT v FROM dur WHERE id = -3'),
	'after', 'writes after recovery survive the next crash');

unlike(slurp_file($node->logfile), qr/inconsistent page found/,
	'no inconsistent pages in replay');
$node->stop;
done_testing();
