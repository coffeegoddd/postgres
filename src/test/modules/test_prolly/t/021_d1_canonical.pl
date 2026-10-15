# Copyright (c) 2026, PostgreSQL Global Development Group

# A commit's root hash depends only on the logical content of what it
# commits.  Two databases reach the same tables, types, indexes and
# sequences by different histories (other creation orders, rows inserted
# and deleted, columns dropped, a column added with a default after rows
# existed (with no other change to its table), other search paths) and get
# the same root hash; one changed
# value changes it.  Objects the canonical form doesn't cover yet make a
# commit fail, rather than give a hash that might depend on history.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('canonical');
$node->init;
$node->append_conf('postgresql.conf', "default_table_access_method = prolly_d1\n");
$node->start;
$node->safe_psql('postgres', 'CREATE DATABASE a; CREATE DATABASE b; CREATE DATABASE c;');

my $schema_a = q{
	CREATE TYPE mood AS ENUM ('sad', 'ok', 'happy');
	CREATE DOMAIN pos AS int CHECK (VALUE > 0);
	CREATE TYPE pair AS (x int, y text);
	CREATE SEQUENCE ids START 100;
	CREATE TABLE junk (x int);
	DROP TABLE junk;
	CREATE TABLE t (id int PRIMARY KEY, name text NOT NULL, m mood, extra int, p pair);
	INSERT INTO t SELECT g, 'n' || g, 'ok', g, ROW(g, 'y')::pair FROM generate_series(1, 300) g;
	DELETE FROM t WHERE id > 200;
	ALTER TABLE t DROP COLUMN extra;
	ALTER TABLE t ADD COLUMN score pos DEFAULT 5;
	CREATE INDEX t_name ON t (name);
	CREATE UNIQUE INDEX t_m ON t USING prolly (name, id);
	CREATE TABLE k (v text);
	INSERT INTO k VALUES ('x'), ('x'), ('y');
	CREATE TABLE d (id int PRIMARY KEY);
	INSERT INTO d SELECT generate_series(1, 50);
	ALTER TABLE d ADD COLUMN w int DEFAULT 9;
	SET search_path = pg_catalog, public;
	SELECT prolly_commit('a');
};
my $schema_b = q{
	SET search_path = public;
	CREATE TABLE k (v text);
	INSERT INTO k VALUES ('y'), ('x'), ('x');
	CREATE TYPE pair AS (x int, y text);
	CREATE DOMAIN pos AS int CHECK (VALUE > 0);
	CREATE TYPE mood AS ENUM ('sad', 'ok', 'happy');
	CREATE TABLE t (id int PRIMARY KEY, name text NOT NULL, m mood, p pair, score pos DEFAULT 5);
	CREATE UNIQUE INDEX t_m ON t USING prolly (name, id);
	INSERT INTO t SELECT g, 'n' || g, 'ok', ROW(g, 'y')::pair, 5 FROM generate_series(200, 1, -1) g;
	CREATE INDEX t_name ON t (name);
	CREATE SEQUENCE ids START 100;
	CREATE TABLE d (id int PRIMARY KEY, w int DEFAULT 9);
	INSERT INTO d SELECT generate_series(50, 1, -1);
	SELECT prolly_commit('b');
};
$node->safe_psql('a', $schema_a);
$node->safe_psql('b', $schema_b);
(my $schema_c = $schema_b) =~ s/'n' \|\| g, 'ok'/'n' || g, CASE WHEN g = 7 THEN 'happy' ELSE 'ok' END::mood/;
$node->safe_psql('c', $schema_c);

my $root = q{SELECT prolly_root_hash('main')};
isnt($node->safe_psql('a', q{SELECT 't'::regclass::oid}),
	$node->safe_psql('b', q{SELECT 't'::regclass::oid}), 'the OIDs differ');
is($node->safe_psql('a', $root), $node->safe_psql('b', $root),
	'the same content reached by different histories has the same root hash');
isnt($node->safe_psql('c', $root), $node->safe_psql('b', $root),
	'one value changed changes the root hash');
is($node->safe_psql('a', q{SELECT score || ':' || count(*) FROM t GROUP BY score}),
	'5:200', 'rows from before the column was added read with its default');

# Committing again without changes keeps the root.
my $before = $node->safe_psql('a', $root);
$node->safe_psql('a', q{SELECT prolly_commit('again')});
is($node->safe_psql('a', $root), $before, 'committing again keeps the root hash');

# Views, functions and triggers are part of it, by name.
for my $db ('a', 'b')
{
	$node->safe_psql($db, q{
		CREATE FUNCTION f(x int) RETURNS int LANGUAGE sql AS 'SELECT x + 1';
		CREATE VIEW v AS SELECT id, f(id) FROM t;
		CREATE FUNCTION trg() RETURNS trigger LANGUAGE plpgsql AS 'BEGIN RETURN NEW; END';
		CREATE TRIGGER t_trg BEFORE INSERT ON t FOR EACH ROW EXECUTE FUNCTION trg();
		SELECT prolly_commit('objects');});
}
is($node->safe_psql('a', $root), $node->safe_psql('b', $root),
	'views, functions and triggers are part of the canonical form');
isnt($node->safe_psql('a', $root), $before, 'and change the root hash');

# What the canonical form doesn't cover yet fails the commit.
my ($ret, $out, $err) = $node->psql('a', q{
	CREATE AGGREGATE mysum(int) (SFUNC = int4pl, STYPE = int);
	SELECT prolly_commit('aggregate');});
like($err, qr/prolly commits don't cover aggregates and window functions yet: public\.mysum\(integer\)/,
	'an aggregate makes a commit fail');
($ret, $out, $err) = $node->psql('a', q{
	DROP AGGREGATE mysum(int);
	CREATE POLICY p ON t USING (true);
	SELECT prolly_commit('policy');});
like($err, qr/prolly commits don't cover row security policies yet: p/,
	'a policy makes a commit fail');

$node->stop;
done_testing();
