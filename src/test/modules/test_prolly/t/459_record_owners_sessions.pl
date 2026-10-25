# Copyright (c) 2026, PostgreSQL Global Development Group

# A session keeps the routines and types a commit's record of relations
# lists from one commit to the next, worked out again only where the
# caches' invalidations say something changed.  Another session's changes
# are among them: a routine's owner changed, and a role renamed whose name
# a line holds, each by another session between two commits of this one,
# are in the next commit's record, so a reset making the routine from that
# commit makes it as the owner recorded then.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('record_owners');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;

$node->safe_psql('postgres', q{
	CREATE ROLE regress_ro_a;
	CREATE ROLE regress_ro_b;
	CREATE SCHEMA s VERSIONED;
	GRANT USAGE, CREATE ON SCHEMA s TO regress_ro_a, regress_ro_b;
	CREATE TABLE s.t (id int PRIMARY KEY, v int);
	INSERT INTO s.t VALUES (1, 0);
	CREATE TYPE s.ty1 AS ENUM ('a');
	CREATE FUNCTION s.f() RETURNS int LANGUAGE sql AS 'SELECT 1';
	CREATE FUNCTION s.g(s.ty1) RETURNS int LANGUAGE sql AS 'SELECT 1';
	ALTER FUNCTION s.f() OWNER TO regress_ro_a;
	ALTER FUNCTION s.g(s.ty1) OWNER TO regress_ro_a;
});

# (one session commits throughout; another changes things between)
my $committer = $node->background_psql('postgres');
$committer->query_safe(q{SELECT dolt_commit('-Am', 'base')});
$committer->query_safe(q{UPDATE s.t SET v = v + 1});
$committer->query_safe(q{SELECT dolt_commit('-am', 'warm')});

$node->safe_psql('postgres', q{
	ALTER FUNCTION s.f() OWNER TO regress_ro_b;
});
$committer->query_safe(q{UPDATE s.t SET v = v + 1});
my $c1 = $committer->query_safe(q{SELECT (dolt_commit('-am', 'changed elsewhere'))[1]});
chomp $c1;

$node->safe_psql('postgres', q{
	DROP FUNCTION s.f();
	DROP FUNCTION s.g(s.ty1);
	SELECT dolt_commit('-am', 'dropped');
});
$node->safe_psql('postgres', qq{SELECT dolt_reset('--hard', '$c1')});
is( $node->safe_psql('postgres', q{
	SELECT string_agg(p.oid::regprocedure || ' ' || p.proowner::regrole, ', ' ORDER BY 1)
	FROM pg_proc p WHERE p.pronamespace = 's'::regnamespace}),
	's.f() regress_ro_b, s.g(s.ty1) regress_ro_a',
	'an owner changed by another session is recorded');

# (a role renamed by another session: its new name recorded.  The
# committer commits first, so that it has read the routines as they are.)
$committer->query_safe(q{UPDATE s.t SET v = v + 1});
$committer->query_safe(q{SELECT dolt_commit('-am', 'settled')});
$node->safe_psql('postgres', q{ALTER ROLE regress_ro_b RENAME TO regress_ro_b2});
$committer->query_safe(q{UPDATE s.t SET v = v + 1});
my $c2 = $committer->query_safe(q{SELECT (dolt_commit('-am', 'role renamed'))[1]});
chomp $c2;
$committer->quit;
$node->safe_psql('postgres', q{
	DROP FUNCTION s.f();
	SELECT dolt_commit('-am', 'f dropped');
});
$node->safe_psql('postgres', qq{SELECT dolt_reset('--hard', '$c2')});
is( $node->safe_psql('postgres', q{
	SELECT p.proowner::regrole FROM pg_proc p WHERE p.oid = 's.f()'::regprocedure}),
	'regress_ro_b2',
	'a role renamed by another session is recorded by its new name');

$node->stop;
done_testing();
