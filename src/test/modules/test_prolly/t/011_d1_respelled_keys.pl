# Copyright (c) 2026, PostgreSQL Global Development Group

# A primary key respelled (equal under a nondeterministic collation, written
# differently) and its collation dropped later in the same transaction: the
# write still names the committed key by its bytes, so the table keeps one
# row for it, before and after the commit, and later writes work.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('node');
$node->init;
$node->append_conf('postgresql.conf',
	"default_table_access_method = prolly_d1\n");
$node->start;

if ($node->safe_psql('postgres',
		"SELECT count(*) > 0 FROM pg_collation WHERE collprovider = 'i'") ne 't')
{
	plan skip_all => 'ICU not supported by this build';
}

# ICU collations need a UTF-8 database.
$node->safe_psql('postgres',
	"CREATE DATABASE keys ENCODING 'UTF8' LOCALE 'C' TEMPLATE template0");
$node->safe_psql('keys', q{
	CREATE EXTENSION test_prolly;
	CREATE COLLATION ci (provider = icu, locale = 'und-u-ks-level2',
						 deterministic = false);
	CREATE TABLE respelled (k text COLLATE ci PRIMARY KEY, v text);
	INSERT INTO respelled VALUES ('a', 'old'), ('b', 'keep');
});

my ($ret, $out, $err) = $node->psql('keys', q{
	BEGIN;
	UPDATE respelled SET k = 'A', v = 'new' WHERE k = 'a';
	DROP COLLATION ci CASCADE;
	SELECT string_agg(v, ',' ORDER BY v) FROM respelled;
	COMMIT;
});
is($ret, 0, 'the transaction commits') or diag($err);
is($out, 'keep,new', 'one row for the respelled key once its collation is gone');

is($node->safe_psql('keys', "SELECT string_agg(v, ',' ORDER BY v) FROM respelled"),
	'keep,new', 'one row for it after the commit');
$node->safe_psql('keys', "INSERT INTO respelled VALUES ('x')");
is($node->safe_psql('keys', "SELECT count(*) FROM respelled"),
	'3', 'later writes work');

# The tree is the one a table written with those rows directly has.
$node->safe_psql('keys', q{
	CREATE TABLE fresh (v text);
	INSERT INTO fresh VALUES ('new'), ('keep'), ('x');
});
is( $node->safe_psql('keys',
		"SELECT prolly_tree_digest('respelled') = prolly_tree_digest('fresh')"),
	't',
	'the same tree as the rows written directly');

$node->stop;
done_testing();
