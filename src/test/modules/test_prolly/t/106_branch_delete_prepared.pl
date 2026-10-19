# Copyright (c) 2026, PostgreSQL Global Development Group

# A branch isn't deleted under a prepared transaction that was on it: the
# transaction's rows there would outlive the branch, on no branch at all.
# The deletion is refused, naming the transaction, until it is committed or
# rolled back, across a restart too.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('branch_delete_prepared');
$node->init;
$node->append_conf('postgresql.conf', qq{
autovacuum = off
max_prepared_transactions = 5
default_table_access_method = prolly
});
$node->start;

sub sql { return $node->safe_psql($_[1] // 'postgres', $_[0]); }

sub delete_error
{
	my ($branch) = @_;
	my ($ret, $out, $err) =
	  $node->psql('postgres', "SELECT dolt_branch('-D', '$branch')");
	return $ret == 0 ? 'deleted' : $err;
}

# live pg_class rows naming a relation, on any branch (a deleted one too)
sub live_class_rows
{
	my ($relname) = @_;
	my $hex = unpack('H*', $relname);
	return sql(qq{
		SELECT count(*)
		FROM generate_series(0, pg_relation_size('pg_class') / 8192 - 1) b,
			ext.heap_page_items(ext.get_raw_page('pg_class', b::int)) h
		WHERE position('\\x$hex'::bytea IN h.t_data) > 0 AND h.t_xmax = '0'});
}

sql(q{
	CREATE SCHEMA ext;
	CREATE EXTENSION pageinspect SCHEMA ext;
	DROP SCHEMA public CASCADE;
	CREATE SCHEMA public VERSIONED;
	CREATE TABLE t (id int PRIMARY KEY);
	SELECT dolt_commit('-Am', 'c1');
	SELECT dolt_branch('pb1');
	SELECT dolt_branch('pb2');
	SELECT dolt_branch('pb3');
	SELECT dolt_branch('pb4');
});

# The issue's case: the session that prepared leaves the branch, and the
# deletion that followed found nothing in its way.
sql(q{
	SELECT dolt_checkout('pb1');
	BEGIN;
	CREATE TABLE ghost (id int PRIMARY KEY);
	INSERT INTO ghost VALUES (1);
	PREPARE TRANSACTION 'py';
	SELECT dolt_checkout('main');
});
like(delete_error('pb1'),
	qr/branch "pb1" has a prepared transaction on it.*\n.*DETAIL:  Prepared transaction "py" was on it\./,
	'a branch a prepared transaction was on isn\'t deleted');
sql(q{COMMIT PREPARED 'py'});
is(sql(q{SELECT count(*) FROM ghost}, 'postgres/pb1'), '1',
	'the transaction committed to the branch');
is(delete_error('pb1'), 'deleted', 'and once it is committed, the branch is deleted');
is(live_class_rows('ghost'), '0', 'with the rows the transaction made');

# A session gone, its prepared transaction left: the branch is no one's
# session's, but still the transaction's.  Rolled back, it holds nothing.
sql(q{BEGIN; INSERT INTO t VALUES (2); CREATE TABLE ghost2 (id int);
	PREPARE TRANSACTION 'pz'}, 'postgres/pb2');
like(delete_error('pb2'), qr/Prepared transaction "pz" was on it/,
	'nor one a gone session\'s prepared transaction was on');
like(delete_error('pb2'), qr/HINT:  Finish it with COMMIT PREPARED or ROLLBACK PREPARED first\./,
	'the error says what to do');
sql(q{ROLLBACK PREPARED 'pz'});
is(delete_error('pb2'), 'deleted', 'rolled back, it holds the branch no longer');

# A branch the transaction left, writing elsewhere, is held still; one it
# was never on isn't.
sql(q{
	SELECT dolt_checkout('pb3');
	BEGIN;
	SELECT dolt_checkout('main');
	INSERT INTO t VALUES (3);
	PREPARE TRANSACTION 'pm';
});
like(delete_error('pb3'), qr/Prepared transaction "pm" was on it/,
	'a branch the transaction left is still held');
is(delete_error('pb4'), 'deleted', 'a branch it was never on isn\'t');
sql(q{COMMIT PREPARED 'pm'});
is(delete_error('pb3'), 'deleted', 'until it ends');

# The prepared transaction's hold on its branch survives a restart.
sql(q{SELECT dolt_branch('pb5')});
sql(q{BEGIN; CREATE TABLE ghost5 (id int); PREPARE TRANSACTION 'pr'},
	'postgres/pb5');
$node->restart;
like(delete_error('pb5'), qr/Prepared transaction "pr" was on it/,
	'a recovered prepared transaction holds its branch');
sql(q{COMMIT PREPARED 'pr'});
is(sql(q{SELECT count(*) FROM ghost5}, 'postgres/pb5'), '0',
	'it committed to the branch');
is(delete_error('pb5'), 'deleted', 'which is then deleted');
is(live_class_rows('ghost5'), '0', 'with its rows');

$node->stop;
done_testing();
