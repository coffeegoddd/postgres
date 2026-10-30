# Copyright (c) 2026, PostgreSQL Global Development Group

# pg_upgrade accepts a cluster with block-mapped tables: their rows
# survive an upgrade to a new cluster, which goes on changing them, in
# each of the ways pg_upgrade moves files (copy, hard links and swapped
# directories).  A block-mapped table's pages are in its database's page
# store, which is moved whole, and its map fork names their blocks there.
# Sequences are block-mapped too, and moved as tables are: a sequence, a
# serial column's and an identity column's go on from where they were.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# (pg_upgrade writes its scripts and logs to the directory it runs in)
chdir PostgreSQL::Test::Utils::tempdir;

for my $mode ('--copy', '--link', '--swap')
{
	(my $suffix = $mode) =~ s/^--/_/;
	my $old = PostgreSQL::Test::Cluster->new("upgrade_old$suffix");
	$old->init;
	$old->append_conf('postgresql.conf', qq{
	debug_block_mapped_heap = on
	autovacuum = off
	});
	$old->start;

	my $rows_query = q{SELECT md5(string_agg(t::text, ',' ORDER BY a)) FROM t};
	$old->safe_psql('postgres', q{
		CREATE TABLE t (a int PRIMARY KEY, b text);
		INSERT INTO t SELECT g, repeat('x', 100) FROM generate_series(1, 20000) g;
		CREATE TABLE s (LIKE t INCLUDING ALL);
		INSERT INTO s SELECT * FROM t WHERE a % 2 = 0;
		UPDATE t SET b = 'y' WHERE a % 3 = 0;
		DELETE FROM t WHERE a % 11 = 0;
		CREATE SEQUENCE sq;
		SELECT nextval('sq') FROM generate_series(1, 42);
		CREATE TABLE ser (id serial, v int);
		CREATE TABLE ident (id int GENERATED ALWAYS AS IDENTITY, v int);
		INSERT INTO ser (v) SELECT g FROM generate_series(1, 7) g;
		INSERT INTO ident (v) SELECT g FROM generate_series(1, 9) g;
	});
	$old->safe_psql('postgres', q{CREATE DATABASE other});
	$old->safe_psql('other', q{
		CREATE TABLE t (a int, b text);
		INSERT INTO t SELECT g, 'other' FROM generate_series(1, 5000) g;
	});
	my $rows = $old->safe_psql('postgres', $rows_query);
	my $s_rows = $old->safe_psql('postgres', q{SELECT count(*), sum(a) FROM s});
	my $other_rows = $old->safe_psql('other', $rows_query);
	my $db = $old->safe_psql('postgres', q{SELECT oid FROM pg_database WHERE datname = 'postgres'});
	my $t = $old->safe_psql('postgres', q{SELECT pg_relation_filenode('t')});
	ok(-f $old->data_dir . "/base/$db/${t}_bmap", 'the old cluster\'s table is block-mapped');
	my $sq = $old->safe_psql('postgres', q{SELECT pg_relation_filenode('sq')});
	ok(-f $old->data_dir . "/base/$db/${sq}_bmap", 'and so is its sequence');
	$old->stop;

	my $new = PostgreSQL::Test::Cluster->new("upgrade_new$suffix");
	$new->init;
	$new->append_conf('postgresql.conf', qq{
	debug_block_mapped_heap = on
	autovacuum = off
	});

	my $bindir = $old->config_data('--bindir');
	command_ok(
		[ 'pg_upgrade', '--no-sync',
		  '--old-datadir' => $old->data_dir,
		  '--new-datadir' => $new->data_dir,
		  '--old-bindir' => $bindir,
		  '--new-bindir' => $bindir,
		  '--socketdir' => $new->host,
		  '--old-port' => $old->port,
		  '--new-port' => $new->port,
		  $mode ],
		"pg_upgrade $mode upgrades the cluster");

	$new->start;
	is($new->safe_psql('postgres', $rows_query), $rows, 'the table\'s rows survive');
	is($new->safe_psql('postgres', q{SELECT count(*), sum(a) FROM s}), $s_rows,
		'and the other table\'s');
	is($new->safe_psql('other', $rows_query), $other_rows,
		'and those of another database');
	is($new->safe_psql('postgres', q{SELECT b FROM t WHERE a = 3}), 'y',
		'an index scan finds a row');
	is($new->safe_psql('postgres', q{SELECT nextval('sq')}), '43',
		'the sequence goes on from where it was');
	is($new->safe_psql('postgres', q{INSERT INTO ser (v) VALUES (8) RETURNING id}), '8',
		'and a serial column');
	is($new->safe_psql('postgres', q{INSERT INTO ident (v) VALUES (10) RETURNING id}), '10',
		'and an identity column');
	$new->safe_psql('postgres', q{
		UPDATE t SET b = 'after' WHERE a % 5 = 0;
		INSERT INTO t SELECT g, 'new' FROM generate_series(20001, 25000) g;
		VACUUM t;
	});
	is($new->safe_psql('postgres', q{SELECT count(*) FROM t WHERE b = 'after' OR b = 'new'}),
		$new->safe_psql('postgres', q{SELECT count(*) FROM t WHERE a % 5 = 0 OR a > 20000}),
		'and the new cluster changes the table');
	$new->stop('immediate');
	$new->start;
	is($new->safe_psql('postgres', q{SELECT count(*) FROM t WHERE b = 'new'}), '5000',
		'which a crash keeps');
	$new->stop;
}

done_testing();
