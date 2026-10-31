# Copyright (c) 2026, PostgreSQL Global Development Group

# pg_upgrade accepts a cluster with block-mapped tables: their rows
# survive an upgrade to a new cluster, which goes on changing them, in
# each of the ways pg_upgrade moves files (copy, hard links and swapped
# directories).  A block-mapped table's pages are in its database's page
# store, which is moved whole, and its map fork names their blocks there.
# Sequences are block-mapped too, and moved as tables are: a sequence, a
# serial column's and an identity column's go on from where they were.
# A page store is moved even when no relation names it: one holding only
# a captured root keeps it, in the default tablespace and in another where
# the database has nothing else.  One run moves files with two jobs.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Digest::MD5;

# (pg_upgrade writes its scripts and logs to the directory it runs in)
chdir PostgreSQL::Test::Utils::tempdir;

# A database's directory in a tablespace (the server running).
sub db_dir
{
	my ($node, $dbname, $spcname) = @_;
	my $db = $node->safe_psql($dbname, qq{SELECT oid FROM pg_database WHERE datname = '$dbname'});
	my $spc = $node->safe_psql($dbname, qq{SELECT oid FROM pg_tablespace WHERE spcname = '$spcname'});
	my ($version) = glob($node->data_dir . "/pg_tblspc/$spc/PG_*");
	return "$version/$db";
}

# The page store's files in a database's directory, by file number, with
# an MD5 of each (the server stopped).
sub store_files
{
	my ($dir) = @_;
	my @out;
	for my $f (9442, 9441, 9421, 9420, 9419)
	{
		my $path = "$dir/$f";
		next unless -f $path;
		open my $fh, '<', $path or die "$path: $!";
		binmode $fh;
		push @out, "$f:" . Digest::MD5->new->addfile($fh)->hexdigest;
		close $fh;
	}
	return join(',', @out);
}

for my $mode ('--copy', '--link', '--swap')
{
	(my $suffix = $mode) =~ s/^--/_/;
	my $old = PostgreSQL::Test::Cluster->new("upgrade_old$suffix");
	$old->init;
	$old->append_conf('postgresql.conf', qq{
	debug_block_mapped_heap = on
	autovacuum = off
	allow_in_place_tablespaces = on
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
	# A database whose page store holds only a captured root: the table
	# it was captured of is dropped.
	$old->safe_psql('postgres', q{CREATE DATABASE roots});
	$old->safe_psql('roots', q{
		CREATE EXTENSION test_prolly;
		CREATE TABLE r (a int, b text);
		INSERT INTO r SELECT g, repeat('r', 100) FROM generate_series(1, 3000) g;
	});
	my $r = $old->safe_psql('roots', q{SELECT pg_relation_filenode('r')});
	my $root = $old->safe_psql('roots', qq{SELECT test_bmap_root_capture($r)});
	my $r_rows = $old->safe_psql('roots', qq{SELECT test_bmap_rows_digest($r)});
	$old->safe_psql('roots', q{DROP TABLE r});

	# And a store in a tablespace where the database then has nothing else
	# (its table dropped, its blocks freed or queued to be).
	$old->safe_psql('postgres', q{CREATE TABLESPACE ts LOCATION ''});
	$old->safe_psql('roots', q{
		CREATE TABLE r2 (a int, b text) TABLESPACE ts;
		INSERT INTO r2 SELECT g, repeat('s', 100) FROM generate_series(1, 2000) g;
	});
	$old->safe_psql('roots', q{DROP TABLE r2; CHECKPOINT;});
	is($old->safe_psql('roots', q{SELECT count(*) FROM pg_class WHERE reltablespace =
		(SELECT oid FROM pg_tablespace WHERE spcname = 'ts')}),
		'0', 'the roots database has no relation in the tablespace');
	is($old->safe_psql('roots', q{
		SELECT count(*) FROM pg_class c
		WHERE c.relkind IN ('r', 'm', 't', 'S', 'i') AND c.oid >= 16384}),
		'0', 'the roots database has no relation of its own');
	my $rows = $old->safe_psql('postgres', $rows_query);
	my $old_ts_dir = db_dir($old, 'roots', 'ts');
	my $s_rows = $old->safe_psql('postgres', q{SELECT count(*), sum(a) FROM s});
	my $other_rows = $old->safe_psql('other', $rows_query);
	my $db = $old->safe_psql('postgres', q{SELECT oid FROM pg_database WHERE datname = 'postgres'});
	my $t = $old->safe_psql('postgres', q{SELECT pg_relation_filenode('t')});
	ok(-f $old->data_dir . "/base/$db/${t}_bmap", 'the old cluster\'s table is block-mapped');
	my $sq = $old->safe_psql('postgres', q{SELECT pg_relation_filenode('sq')});
	ok(-f $old->data_dir . "/base/$db/${sq}_bmap", 'and so is its sequence');
	$old->stop;
	my $ts_store = store_files($old_ts_dir);
	like($ts_store, qr/9421:[0-9a-f]{32}/,
		'the tablespace has the database\'s store');

	my $new = PostgreSQL::Test::Cluster->new("upgrade_new$suffix");
	$new->init;
	$new->append_conf('postgresql.conf', qq{
	debug_block_mapped_heap = on
	autovacuum = off
	allow_in_place_tablespaces = on
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
		  ($mode eq '--link' ? ('--jobs' => 2) : ()),
		  $mode ],
		"pg_upgrade $mode upgrades the cluster");

	# (tablespace and database OIDs are kept)
	(my $new_ts_dir = $old_ts_dir) =~ s/^\Q@{[$old->data_dir]}\E/@{[$new->data_dir]}/;
	is(store_files($new_ts_dir), $ts_store,
		'the store in a tablespace where the database had nothing else is moved');

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
	is($new->safe_psql('roots', qq{SELECT test_bmap_root_held($root) IS NOT NULL}),
		't', 'a page store holding only a captured root keeps it');
	my $made = $new->safe_psql('roots', qq{SELECT test_bmap_storage_from_root($root)});
	is($new->safe_psql('roots', qq{SELECT test_bmap_rows_digest($made)}), $r_rows,
		'and storage made of it has the rows captured');
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

# Where the old cluster has no page store, the new cluster's is left: from
# a cluster whose sequences' files aren't transferred, pg_restore's
# sequences are in it.  (An old cluster on md.c alone, a new one making
# block-mapped storage.)
{
	my $old = PostgreSQL::Test::Cluster->new('upgrade_old_md');
	$old->init;
	$old->start;
	$old->safe_psql('postgres', q{
		CREATE TABLE m (a int);
		INSERT INTO m SELECT generate_series(1, 100);
		CREATE SEQUENCE ms;
		SELECT nextval('ms');
	});
	my $db = $old->safe_psql('postgres', q{SELECT oid FROM pg_database WHERE datname = 'postgres'});
	$old->stop;
	ok(!-f $old->data_dir . "/base/$db/9421", 'the old cluster has no page store');

	my $new = PostgreSQL::Test::Cluster->new('upgrade_new_bmap');
	$new->init;
	$new->append_conf('postgresql.conf', 'debug_block_mapped_heap = on');
	# (no autovacuum, which would run the sweep due after a start first)
	$new->append_conf('postgresql.conf', 'autovacuum = off');
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
		  '--copy' ],
		'pg_upgrade from a cluster on md.c alone');
	ok(-f $new->data_dir . "/base/$db/9421", 'the new cluster\'s page store is left');
	$new->start;
	is($new->safe_psql('postgres', q{SELECT count(*) FROM m}), '100', 'and the rows survive');
	is($new->safe_psql('postgres', q{SELECT nextval('ms')}), '2', 'and the sequence goes on');
	# pg_restore made the sequence block-mapped in the new cluster's store,
	# its page in it, and the upgrade dropped its map for the old file: the
	# page's block is counted and held by nothing, which the sweep due after
	# the start frees.
	$new->safe_psql('postgres', q{CREATE EXTENSION test_prolly});
	is($new->safe_psql('postgres', q{SELECT test_bmap_sweep_due()}), 't',
		'a sweep is due after the start');
	cmp_ok($new->safe_psql('postgres', q{SELECT freed FROM test_bmap_sweep()}), '>', 0,
		'it frees the blocks of the maps the upgrade dropped');
	is($new->safe_psql('postgres', q{SELECT freed FROM test_bmap_sweep()}), '0',
		'and nothing is left held by nothing');
	$new->stop;
}

done_testing();
