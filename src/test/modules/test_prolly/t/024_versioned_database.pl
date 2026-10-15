# Copyright (c) 2026, PostgreSQL Global Development Group

# Version-controlled databases are opt-in: CREATE DATABASE ... VERSIONED,
# or a versioned template.  In one, prolly is the default table access
# method (below any explicit setting), and permanent tables stored by any
# other access method can't be made; temporary and unlogged tables can.
# Other databases are Postgres as it is.  Prolly relations ignore
# tablespaces, with a NOTICE.  pg_dump keeps the setting.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('versioned');
$node->init;
$node->start;

$node->safe_psql('postgres', q{
	CREATE DATABASE v WITH VERSIONED = true;
	CREATE DATABASE v2 TEMPLATE v;
	CREATE DATABASE v3 TEMPLATE v VERSIONED false;
	CREATE DATABASE plain;
});
is($node->safe_psql('postgres', q{SELECT string_agg(datname || ':' || datversioned, ',' ORDER BY datname)
	FROM pg_database WHERE datname IN ('v', 'v2', 'v3', 'plain', 'postgres')}),
	'plain:false,postgres:false,v:true,v2:true,v3:false',
	'VERSIONED, or the template\'s setting');

sub fails
{
	my ($db, $query, $re, $name) = @_;
	my ($ret, $out, $err) = $node->psql($db, $query);
	like($err, $re, $name);
}

# In a versioned database.
is($node->safe_psql('v', q{SHOW default_table_access_method}), 'prolly_d1',
	'prolly is the default table access method');
is($node->safe_psql('v', q{CREATE TABLE t (id int PRIMARY KEY, v text);
	SELECT amname FROM pg_class c JOIN pg_am a ON a.oid = c.relam WHERE relname = 't'}),
	'prolly_d1', 'so tables are prolly tables');
is($node->safe_psql('v', q{SET default_table_access_method = prolly_d2; SHOW default_table_access_method}),
	'prolly_d2', 'an explicit setting still wins');
my $refused = qr/a versioned database can't have permanent tables stored by "heap"/;
fails('v', q{CREATE TABLE h (x int) USING heap}, $refused, 'a permanent heap table is refused');
fails('v', q{SET default_table_access_method = heap; CREATE TABLE h (x int)}, $refused,
	'also by default');
fails('v', q{CREATE TABLE h USING heap AS SELECT 1 AS x}, $refused, 'and by CREATE TABLE AS');
fails('v', q{CREATE MATERIALIZED VIEW mv USING heap AS SELECT 1 AS x}, $refused,
	'and as a materialized view');
fails('v', q{ALTER TABLE t SET ACCESS METHOD heap}, $refused, 'and by SET ACCESS METHOD');
$node->safe_psql('v', q{
	CREATE TEMP TABLE tt (x int) USING heap;
	CREATE UNLOGGED TABLE u (x int);
});
pass('temporary and unlogged tables can be made');
fails('v', q{ALTER TABLE u SET LOGGED}, $refused, 'but an unlogged heap table can\'t become permanent');
$node->safe_psql('v', q{INSERT INTO t VALUES (1, 'a'); SELECT dolt_commit('-m', 'one')});
is($node->safe_psql('v', q{SELECT count(*) FROM dolt_log}), '1', 'and its tables are committed');

# A plain database is Postgres as it is.
is($node->safe_psql('plain', q{SHOW default_table_access_method}), 'heap',
	'a plain database\'s default is heap');
$node->safe_psql('plain', q{CREATE TABLE h (x int)});
pass('and it has heap tables');

# Tablespaces.
my ($ret, $out, $err) = $node->psql('v', q{
	CREATE TABLE ts (x int) TABLESPACE pg_default;
	CREATE INDEX ts_x ON ts USING prolly_d1_index (x) TABLESPACE pg_default;
	ALTER TABLE ts SET TABLESPACE pg_default;});
is(() = $err =~ /NOTICE:  prolly relations ignore tablespaces; tablespace "pg_default" is not used/g, 3,
	'prolly tables and indexes ignore tablespaces, with a NOTICE');
($ret, $out, $err) = $node->psql('plain', q{CREATE TABLE ts (x int) TABLESPACE pg_default});
is($err, '', 'heap tables use them as always');

# pg_dump keeps the setting.
my $dump = `pg_dump -C -s -d "@{[ $node->connstr('v') ]}"`;
like($dump, qr/CREATE DATABASE v WITH .*VERSIONED = true;/, 'pg_dump keeps VERSIONED');
$dump = `pg_dump -C -s -d "@{[ $node->connstr('plain') ]}"`;
unlike($dump, qr/VERSIONED/, 'and leaves it out for other databases');

$node->stop;
done_testing();
