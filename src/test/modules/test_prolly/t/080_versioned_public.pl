# Copyright (c) 2026, PostgreSQL Global Development Group

# A history whose public is versioned, taken into a new database whose
# public is the default one, unversioned and empty: pushed over a libpq
# connection into a database with no history yet, the push moves the
# branch and makes its copies there, the empty public replaced by the
# history's; one with objects in it is refused, naming it.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $origin = PostgreSQL::Test::Cluster->new('pub_origin');
$origin->init;
$origin->append_conf('postgresql.conf', 'autovacuum = off');
$origin->start;
my $local = PostgreSQL::Test::Cluster->new('pub_local');
$local->init;
$local->append_conf('postgresql.conf', 'autovacuum = off');
$local->start;

sub o { return $origin->safe_psql($_[0], $_[1]); }
sub l { return $local->safe_psql($_[0], $_[1]); }
sub url
{
	return "postgresql:///$_[0]?host=" . $origin->host . '&port=' . $origin->port;
}

l('postgres', q{
	DROP SCHEMA public CASCADE;
	CREATE SCHEMA public VERSIONED;
	CREATE TABLE t (id int PRIMARY KEY, v text);
	INSERT INTO t SELECT g, 'v' || g FROM generate_series(1, 100) g;
	SELECT dolt_commit('-Am', 'one');
});

# a push into a new database: its empty public replaced by the history's
o('postgres', 'CREATE DATABASE fresh');
my $u = url('fresh');
l('postgres', qq{
	SELECT dolt_remote('add', 'fresh', '$u');
	SELECT dolt_push('fresh', 'main');
});
is(o('fresh', q{SELECT dolt_hashof('main')}), l('postgres', q{SELECT dolt_hashof('main')}),
	'pushed into a new database');
is(o('fresh', 'SELECT count(*), max(v) FROM t'), '100|v99', 'with its rows');
is(o('fresh', q{SELECT nspversioned FROM pg_namespace WHERE nspname = 'public'}), 't',
	'its public the history\'s');
is( o('fresh',
		q{SELECT nspowner::regrole, nspacl FROM pg_namespace WHERE nspname = 'public'}),
	'pg_database_owner|{pg_database_owner=UC/pg_database_owner,=U/pg_database_owner}',
	'owned and usable as the one it replaced');
is(o('fresh', q{SELECT count(*) FROM dolt_status}), '0', 'and nothing uncommitted');

# and pushed to again
l('postgres', q{
	INSERT INTO t VALUES (101, 'two');
	SELECT dolt_commit('-am', 'two');
	SELECT dolt_push('fresh', 'main');
});
is(o('fresh', 'SELECT v FROM t WHERE id = 101'), 'two', 'and pushed to again');

# a public with something in it: refused, named, and left as it was
o('postgres', 'CREATE DATABASE used');
o('used', 'CREATE TABLE public.mine (id int)');
$u = url('used');
l('postgres', qq{SELECT dolt_remote('add', 'used', '$u')});
my ($ret, $out, $err) = $local->psql('postgres', q{SELECT dolt_push('used', 'main')});
like($err, qr/schema "public" already exists and isn't versioned/,
	'a public with objects in it: refused');
like($err, qr/HINT:  Drop it/, 'with a hint');
is(o('used', q{SELECT count(*) FROM dolt_log}), '0', 'the push left nothing');
is(o('used', q{SELECT nspversioned::text || count(c.oid) FROM pg_namespace n
	LEFT JOIN pg_class c ON c.relnamespace = n.oid
	WHERE nspname = 'public' GROUP BY nspversioned}), 'false1', 'its public as it was');

# a later move there, the pushed branch checked out where main has
# commits of its own using the ordinary public: refused, public kept
o('postgres', 'CREATE DATABASE hist');
o('hist', q{SELECT dolt_commit('--allow-empty', '-m', 'own')});
$u = url('hist');
l('postgres', qq{
	SELECT dolt_remote('add', 'hist', '$u');
	SELECT dolt_branch('feature');
	SELECT dolt_push('hist', 'feature');
});
($ret, $out, $err) = $origin->psql('hist', q{SELECT dolt_checkout('feature')});
like($err, qr/branch "feature" has commits already/,
	'a later move: refused');
is(o('hist', q{SELECT nspversioned FROM pg_namespace WHERE nspname = 'public'}), 'f',
	'its public as it was');

done_testing();
