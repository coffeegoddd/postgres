# Copyright (c) 2026, PostgreSQL Global Development Group

# A relation's trees are found by the relation, not by its storage: the
# catalogs (shared by every branch until they're versioned) give it a new
# relfilenumber on TRUNCATE and rewrites, and that must not empty the table
# on other branches or in commits.  TRUNCATE and rewrites on one branch
# leave the others' rows and the commits' alone, roll back with their
# savepoints, keep TIDs unique to the table, and survive a crash; DROP
# removes the relation's trees from the branch, so reclamation frees them,
# prepared transactions included.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('trees');
$node->init;
$node->append_conf('postgresql.conf', "max_prepared_transactions = 2\n");
$node->start;
$node->safe_psql('postgres', 'CREATE DATABASE v VERSIONED true');

sub sql { return $node->safe_psql($_[1] // 'v', $_[0]); }

sql(q{
	CREATE TABLE t (id int PRIMARY KEY, v text);
	CREATE INDEX t_id ON t USING prolly_d1_index (id);
	INSERT INTO t SELECT g, 'v' || g FROM generate_series(1, 5) g;
	SELECT dolt_commit('-m', 'base');
	SELECT dolt_branch('b');
});
my $tids = sql(q{SELECT string_agg(ctid::text, ',' ORDER BY id) FROM t});

# TRUNCATE on main.
sql(q{TRUNCATE t; INSERT INTO t VALUES (100, 'main')});
is(sql(q{SELECT string_agg(id::text, ',' ORDER BY id) FROM t}), '100', 'main is truncated');
is(sql(q{SELECT string_agg(id::text, ',' ORDER BY id) FROM t}, 'v/b'), '1,2,3,4,5',
	'branch b keeps its rows');
is(sql(q{SET enable_seqscan = off; SELECT string_agg(id::text, ',' ORDER BY id) FROM t WHERE id > 2},
	'v/b'), '3,4,5', 'and its prolly index');
is(sql(q{SELECT string_agg(ctid::text, ',' ORDER BY id) FROM t}, 'v/b'), $tids,
	'and its rows\' TIDs');
my $new = sql(q{INSERT INTO t VALUES (6, 'v6') RETURNING ctid}, 'v/b');
unlike(",$tids,", qr/,\Q$new\E,/, 'a new row on b doesn\'t take a TID of one of its rows');
isnt($new, sql(q{SELECT ctid FROM t WHERE id = 100}), 'nor of main\'s row');

# Rewrites on main: VACUUM FULL, and a column's type.
sql(q{VACUUM FULL t; ALTER TABLE t ALTER COLUMN v TYPE varchar(20)});
is(sql(q{SELECT string_agg(id || ':' || v, ',' ORDER BY id) FROM t}), '100:main',
	'main\'s rows after its rewrites');
is(sql(q{SELECT string_agg(id::text, ',' ORDER BY id) FROM t}, 'v/b'), '1,2,3,4,5,6',
	'branch b keeps its rows through main\'s rewrites');

# The commit from before the TRUNCATE still has its rows.
sql(q{SELECT dolt_branch('old', 'HEAD')});
is(sql(q{SELECT count(*) FROM t}, 'v/old'), '5', 'a branch from the old commit has its rows');

# Rolled back with a savepoint.
sql(q{BEGIN; SAVEPOINT s; TRUNCATE t; ROLLBACK TO s; COMMIT;});
is(sql(q{SELECT count(*) FROM t}), '1', 'a TRUNCATE rolled back to a savepoint');
sql(q{BEGIN; SAVEPOINT s; ALTER TABLE t ALTER COLUMN v TYPE text; ROLLBACK TO s; COMMIT;});
is(sql(q{SELECT string_agg(id || ':' || v, ',' ORDER BY id) FROM t}), '100:main',
	'a rewrite rolled back to a savepoint');

# Changing a table's access method moves its rows, both ways.
sql(q{
	CREATE TABLE h (x int) USING heap;
	INSERT INTO h SELECT generate_series(1, 10);
	ALTER TABLE h SET ACCESS METHOD prolly_d1;
}, 'postgres');
is(sql(q{SELECT count(*), sum(x) FROM h}, 'postgres'), '10|55', 'heap to prolly');
sql(q{ALTER TABLE h SET ACCESS METHOD heap}, 'postgres');
is(sql(q{SELECT count(*), sum(x) FROM h}, 'postgres'), '10|55', 'and back');

# Through a crash.
$node->stop('immediate');
$node->start;
is(sql(q{SELECT string_agg(id::text, ',' ORDER BY id) FROM t}, 'v/b'), '1,2,3,4,5,6',
	'branch b\'s rows survive a crash');
is(sql(q{SELECT string_agg(id::text, ',' ORDER BY id) FROM t}), '100', 'and so do main\'s');

# DROP removes the trees; reclamation frees them.
sql(q{CREATE TABLE big (id int PRIMARY KEY, pad text);
	INSERT INTO big SELECT g, repeat('x', 200) FROM generate_series(1, 5000) g;
	SELECT prolly_gc();});
sql(q{DROP TABLE big});
cmp_ok(sql(q{SELECT nodes_freed FROM prolly_gc()}), '>', 20,
	'a dropped table\'s nodes are reclaimed');
sql(q{CREATE TABLE big2 (id int PRIMARY KEY, pad text);
	INSERT INTO big2 SELECT g, repeat('x', 200) FROM generate_series(1, 5000) g;
	SELECT prolly_gc();});
sql(q{BEGIN; DROP TABLE big2; PREPARE TRANSACTION 'd'; COMMIT PREPARED 'd';});
cmp_ok(sql(q{SELECT nodes_freed FROM prolly_gc()}), '>', 20,
	'also when dropped by a prepared transaction');
sql(q{CREATE TABLE kept (id int PRIMARY KEY);
	INSERT INTO kept SELECT generate_series(1, 2000);
	BEGIN; SAVEPOINT s; DROP TABLE kept; ROLLBACK TO s; COMMIT;});
is(sql(q{SELECT count(*) FROM kept}), '2000', 'a DROP rolled back to a savepoint keeps the rows');

$node->stop;
done_testing();
