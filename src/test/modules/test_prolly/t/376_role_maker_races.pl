# Copyright (c) 2026, PostgreSQL Global Development Group

# DROP OWNED and REASSIGN OWNED find a role's branch, tag or stash by its
# maker record's id with the refs held, so a change another session makes
# meanwhile (a stash pushed onto the same list, the branch renamed) can't
# make them change another object, or one no longer there.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('role_maker_races');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;
sub sql { return $node->safe_psql('postgres', $_[0]); }

sub waits_for
{
	my ($what) = @_;
	return $node->poll_query_until('postgres', qq{
		SELECT count(*) > 0 FROM pg_stat_activity
		WHERE query LIKE '%$what%' AND wait_event_type = 'Lock'});
}

# the role each of the database's maker entries of kind classid names
sub makers
{
	my ($classid) = @_;
	return sql(qq{SELECT string_agg(pg_describe_object(classid, objid, 0)
			|| ' ' || refobjid::regrole, ', ' ORDER BY 1)
		FROM pg_shdepend WHERE classid = $classid});
}

sql(q{CREATE EXTENSION test_prolly;
	CREATE SCHEMA r VERSIONED;
	CREATE TABLE r.t (a int PRIMARY KEY);
	GRANT USAGE ON SCHEMA r TO PUBLIC;
	GRANT ALL ON r.t TO PUBLIC;
	INSERT INTO r.t VALUES (1);
	SELECT dolt_commit('-Am', 'one');
	CREATE ROLE regress_ra;
	CREATE ROLE regress_rb;
	CREATE ROLE regress_rh;
	GRANT CREATE ON DATABASE postgres TO regress_ra, regress_rb});

# A stash pushed onto the list while DROP OWNED waits for the refs: the
# dropped role's stash goes, not the one now first in the list.
sql(q{SET ROLE regress_ra;
	INSERT INTO r.t VALUES (2);
	SELECT dolt_stash('push', 'shelf')});
my $b = $node->background_psql('postgres', on_error_stop => 0);
my $a = $node->background_psql('postgres', on_error_stop => 0);
$b->query_safe(q{SET ROLE regress_rb; BEGIN; INSERT INTO r.t VALUES (3);
	SELECT dolt_stash('push', 'shelf')});
$a->query_until(qr/started/, q{
	\echo started
	DROP OWNED BY regress_ra;
	\echo done
});
ok(waits_for('DROP OWNED'), 'DROP OWNED waits for the stash push');
$b->query_safe('COMMIT');
$a->query_until(qr/done/, '');
is($a->{stderr}, '', 'and then drops');
is(makers(9909), 'stash stash@{0} of "shelf" regress_rb',
	'the other role\'s stash is left');
is(sql(q{SELECT count(*) FROM dolt_stashes}), '1', 'and only it');
sql(q{SELECT dolt_stash('clear', 'shelf')});

# A rename while REASSIGN OWNED waits for the refs: the renamed branch gets
# the new maker, and no maker is recorded for the old name.
# (DROP OWNED took the role's grants too)
sql(q{GRANT CREATE ON DATABASE postgres TO regress_ra});
sql(q{SET ROLE regress_ra; SELECT dolt_branch('old')});
$b->query_safe(q{RESET ROLE; BEGIN;
	SELECT dolt_branch('-m', 'old', 'renamed')});
$a->query_until(qr/started/, q{
	\echo started
	REASSIGN OWNED BY regress_ra TO regress_rh;
	\echo done
});
ok(waits_for('REASSIGN OWNED'), 'REASSIGN OWNED waits for the rename');
$b->query_safe('COMMIT');
$a->query_until(qr/done/, '');
is($a->{stderr}, '', 'and then reassigns');
is(makers(9907), 'branch "renamed" regress_rh',
	'the renamed branch is the new role\'s, and only it');
is(sql(q{SELECT string_agg(name || ' ' || creator, ', ' ORDER BY name)
		FROM dolt_branches WHERE name <> 'main'}),
	'renamed regress_rh', 'as dolt_branches shows');

$a->quit;
$b->quit;
$node->stop;
done_testing();
