# Copyright (c) 2026, PostgreSQL Global Development Group

# A record names its routines and types through an entry records share,
# which a commit finding it stored already doesn't write.  The step after a
# collection forgets those no record left names, holding the refs whole:
# a commit made while it holds them finds the entry still stored, and its
# publish waits for the step, then, rebuilt on the root the step moved,
# puts back the entry its record names.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('forget_owned_concurrent');
$node->init;
# (no branch's pages kept as of a commit, which would keep the commit)
$node->append_conf('postgresql.conf', 'debug_prolly_page_roots = off');
$node->start;
$node->safe_psql('postgres', 'CREATE DATABASE fo');
$node->safe_psql('fo', 'CREATE EXTENSION test_prolly');
$node->safe_psql('fo', 'CREATE EXTENSION injection_points');

sub sql { return $node->safe_psql('fo', $_[0]); }

sub owned_entry
{
	my ($value) = @_;
	return sql(
		qq{SELECT substring(convert_from($value, 'UTF8')
			FROM '\@([0-9a-f]{64})\\n\$')});
}

sql(q{CREATE SCHEMA s VERSIONED;
	CREATE TABLE s.t (id int PRIMARY KEY, v text);
	INSERT INTO s.t VALUES (1, 'base');
	CREATE FUNCTION s.f1() RETURNS int LANGUAGE sql AS 'SELECT 1'});
sql(q{SELECT dolt_commit('-Am', 'base')});
sql(q{SELECT dolt_branch('x')});

# On x a commit with a second routine, whose record names an entry of its
# own; then x's head goes back to the base, the routine left uncommitted.
my $maker = $node->background_psql('fo');
$maker->query_safe(q{SELECT dolt_checkout('x')});
$maker->query_safe(
	q{CREATE FUNCTION s.f2() RETURNS int LANGUAGE sql AS 'SELECT 2'});
$maker->query_safe(q{SELECT dolt_commit('-Am', 'f2')});
my $f2 = sql(q{SELECT dolt_hashof('x')});
my $entry = owned_entry("test_refs_record('$f2')");
isnt($entry, '', 'the commit names its routines by an entry');
$maker->query_safe(q{SELECT dolt_reset('--soft', 'HEAD~1')});
# (x's record of its head's tables names the entry too, until x's next
# commit writes it again: deleted here, so that the collected commit's
# record is the only one naming it)
is(owned_entry(q{test_refs_value('Nx')}), $entry, "x's head record names it");
is(sql(q{SELECT test_refs_delete('Nx')}), 't', "x's head record is deleted");

# A collection takes the commit; its step forgetting records stops, the
# refs held, once it has found what to forget.
my $gc = $node->background_psql('fo');
$gc->query_safe(q{SELECT injection_points_set_local();
	SELECT injection_points_attach('prolly-forget-records-locked', 'wait')});
$gc->query_until(qr/collecting/, q{\echo collecting
	SELECT dolt_gc() IS NOT NULL;
	\echo collected
});
$node->wait_for_event('client backend', 'prolly-forget-records-locked');
is(sql(qq{SELECT test_chunk_has('$f2')}), 'f', 'the collection took the commit');

# x's next commit lists the same routines: it finds the entry stored (the
# step hasn't moved the root yet), so doesn't write it, and its publish
# waits for the step.
$maker->query_until(qr/started/, q{\echo started
	UPDATE s.t SET v = 'on x';
	SELECT dolt_commit('-Am', 'x') IS NULL;
	\echo made
});
$node->poll_query_until('fo', q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE state = 'active' AND wait_event_type = 'Lock'
	AND backend_type = 'client backend' AND pid <> pg_backend_pid()})
  or diag('the commit did not wait');
$node->safe_psql('fo', q{SELECT injection_points_wakeup('prolly-forget-records-locked')});
$gc->query_until(qr/collected/, '');
$maker->query_until(qr/made/, '');
$gc->quit;

my $x = sql(q{SELECT dolt_hashof('x')});
is(owned_entry("test_refs_record('$x')"), $entry,
	"x's commit names the entry");
is(sql(qq{SELECT test_refs_record('$f2') IS NULL}), 't',
	"the collected commit's record is forgotten");
is(sql(qq{SELECT test_refs_value('J' || '$entry') IS NOT NULL}),
	't', 'the entry survives: the publish put it back');

# A move reading the record finds every routine's owner.
$maker->query_safe(q{DROP FUNCTION s.f2()});
is($maker->query_safe(qq{SELECT dolt_reset('--hard', '$x') IS NOT NULL}),
	't', 'a reset to the commit reads its record');
is($maker->query_safe(q{SELECT to_regprocedure('s.f2()') IS NOT NULL}),
	't', 'and makes the routine');
$maker->quit;

$node->stop;
done_testing();
