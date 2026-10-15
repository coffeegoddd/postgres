# Copyright (c) 2026, PostgreSQL Global Development Group

# Engine D1's transaction state is cleaned up on every way a transaction
# or subtransaction can end, and the same connection keeps working.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('cleanup');
$node->init;
$node->append_conf('postgresql.conf', qq(
default_table_access_method = prolly_d1
max_prepared_transactions = 10
));
$node->start;

$node->safe_psql('postgres', q{
	CREATE TABLE t (id int);
	INSERT INTO t VALUES (1);
});

# A first write that fails waiting for the right to write a row (another
# session holds a share lock on it), inside a savepoint, then a retry after
# rolling back to it.
my $holder = $node->background_psql('postgres');
$holder->query_safe('BEGIN; SELECT * FROM t FOR SHARE;');

my $writer = $node->background_psql('postgres', on_error_stop => 0);
$writer->query_safe(q{BEGIN; SAVEPOINT s; SET LOCAL lock_timeout = '100ms';});
$writer->query('UPDATE t SET id = 2 WHERE id = 1');
like($writer->{stderr}, qr/lock timeout/,
	'the first write times out on the write lock');
$writer->{stderr} = '';
$writer->query_safe('ROLLBACK TO s');
$holder->query_safe('COMMIT');
$holder->quit;
$writer->query_safe('INSERT INTO t VALUES (3)');
$writer->query_safe('COMMIT');
$writer->quit;
is($node->safe_psql('postgres', 'SELECT array_agg(id ORDER BY id) FROM t'),
	'{1,3}', 'a retry after a lock timeout in a savepoint writes');

# PREPARE after every write was rolled back to a savepoint, then more
# writes on the same connection.
is($node->safe_psql('postgres', q{
	BEGIN;
	SAVEPOINT s;
	INSERT INTO t VALUES (4);
	ROLLBACK TO s;
	PREPARE TRANSACTION 'p';
	COMMIT PREPARED 'p';
	INSERT INTO t VALUES (5);
	SELECT array_agg(id ORDER BY id) FROM t;
}), '{1,3,5}', 'a connection writes again after preparing a transaction');

# Preparing with writes is refused, and the connection keeps working.
my ($ret, $stdout, $stderr) = $node->psql('postgres', q{
	BEGIN;
	INSERT INTO t VALUES (6);
	PREPARE TRANSACTION 'q';
	INSERT INTO t VALUES (7);
	SELECT array_agg(id ORDER BY id) FROM t;
}, on_error_stop => 0);
like($stderr, qr/cannot PREPARE a transaction that has written to a prolly table/,
	'preparing with writes is refused');
is($stdout, '{1,3,5,7}', 'the refused transaction is rolled back');

ok($node->safe_psql('postgres', 'SELECT 1') eq '1', 'the server is still up');

done_testing();
