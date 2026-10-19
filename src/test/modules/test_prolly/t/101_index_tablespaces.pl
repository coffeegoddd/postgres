# Copyright (c) 2026, PostgreSQL Global Development Group

# An index's tablespace is the cluster's, not the commit's: a clone of a
# database whose indexes are in a tablespace, into a cluster without it,
# is clean, and moves there (a reset, a pull) that make an index again make
# it in the default tablespace.  On the origin, a move that makes an index
# again keeps it in its tablespace.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $origin = PostgreSQL::Test::Cluster->new('ts_origin');
$origin->init;
$origin->append_conf('postgresql.conf', "autovacuum = off\nallow_in_place_tablespaces = on");
$origin->start;
my $local = PostgreSQL::Test::Cluster->new('ts_local');
$local->init;
$local->append_conf('postgresql.conf', 'autovacuum = off');
$local->start;

my $url = 'postgresql:///postgres?host=' . $origin->host . '&port=' . $origin->port;
sub o { return $origin->safe_psql($_[0], $_[1]); }
sub l { return $local->safe_psql($_[0], $_[1]); }

my $places = q{SELECT string_agg(indexname || ':' || coalesce(tablespace, ''), ','
	ORDER BY indexname COLLATE "C") FROM pg_indexes WHERE schemaname = 'n'};

o('postgres', q{CREATE TABLESPACE fast LOCATION ''});
o('postgres', q{
	CREATE SCHEMA n VERSIONED;
	CREATE TABLE n.t (id int PRIMARY KEY USING INDEX TABLESPACE fast,
		a int, b int, UNIQUE (a) USING INDEX TABLESPACE fast);
	CREATE INDEX t_b ON n.t (b) TABLESPACE fast;
	INSERT INTO n.t SELECT g, g, g FROM generate_series(1, 100) g;
	SELECT dolt_commit('-Am', 'one');
	DROP INDEX n.t_b;
	CREATE INDEX t_b ON n.t (b, a) TABLESPACE fast;
	SELECT dolt_commit('-am', 'two');
});

# the clone, where there's no tablespace "fast"
l('postgres', 'CREATE DATABASE c');
l('c', qq{SELECT dolt_clone('$url')});
is(l('c', $places), 't_a_key:,t_b:,t_pkey:', 'cloned: the indexes in the default tablespace');
is(l('c', q{SELECT dolt_hashof('main')}), o('postgres', q{SELECT dolt_hashof('main')}),
	'at the remote\'s head');
is(l('c', 'SELECT count(*) FROM dolt_status'), '0', 'and clean');

# a move there that makes t_b again
l('c', q{SELECT dolt_reset('--hard', 'HEAD~1')});
is(l('c', q{SELECT indexdef FROM pg_indexes WHERE indexname = 't_b'}),
	'CREATE INDEX t_b ON n.t USING btree (b)', 'reset: t_b made again, as the commit has it');
is(l('c', 'SELECT count(*) FROM dolt_status'), '0', 'and clean');
l('c', q{SELECT dolt_reset('--hard', 'origin/main')});
is(l('c', 'SELECT count(*) FROM dolt_status'), '0', 'and back');

# a pull of a commit that changes the indexes the origin keeps in "fast"
o('postgres', q{
	ALTER TABLE n.t DROP CONSTRAINT t_a_key, ADD CONSTRAINT t_a_key UNIQUE (a, b)
		USING INDEX TABLESPACE fast;
	SELECT dolt_commit('-am', 'three');
});
l('c', q{SELECT dolt_pull('origin')});
is(l('c', q{SELECT pg_get_constraintdef(oid) FROM pg_constraint WHERE conname = 't_a_key'}),
	'UNIQUE (a, b)', 'pulled: the constraint made again');
is(l('c', $places), 't_a_key:,t_b:,t_pkey:', 'in the default tablespace');
is(l('c', 'SELECT count(*) FROM dolt_status'), '0', 'and clean');

# on the origin, a move that makes the indexes again keeps them in "fast"
o('postgres', q{SELECT dolt_reset('--hard', 'HEAD~2')});
is(o('postgres', q{SELECT indexdef FROM pg_indexes WHERE indexname = 't_b'}),
	'CREATE INDEX t_b ON n.t USING btree (b)', 'origin: t_b made again, as commit one has it');
is(o('postgres', $places), 't_a_key:fast,t_b:fast,t_pkey:fast', 'every index still in "fast"');
is(o('postgres', 'SELECT count(*) FROM dolt_status'), '0', 'and clean');

done_testing();
