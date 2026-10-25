# Copyright (c) 2026, PostgreSQL Global Development Group

# The kept checkouts' budget with other sessions: it counts the idle ones
# alone (one a session is on isn't counted, nor let go of); two sweeps at
# once let go of no more than the budget asks (one finding the branch the
# other deleted counts it gone, not taking the next); and DROP ROLE lets
# go of its database's idle kept checkouts, while another database's are
# named in a hint.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('kept_checkout_budget');
$node->init;
$node->append_conf('postgresql.conf', qq{
autovacuum = off
prolly_fold_worker = off
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE EXTENSION test_prolly;
	SET dolt.author = 'Ada Lovelace <ada@example.com>';
	CREATE SCHEMA s VERSIONED;
	CREATE TABLE s.t (id int PRIMARY KEY, v int);
	INSERT INTO s.t SELECT g, g FROM generate_series(1, 100) g;
	SELECT dolt_commit('-Am', 'c1');
	UPDATE s.t SET v = v + 1;
	SELECT dolt_commit('-am', 'c2');
	UPDATE s.t SET v = v + 1;
	SELECT dolt_commit('-am', 'c3');
	UPDATE s.t SET v = v + 1;
	SELECT dolt_commit('-am', 'c4');
});
my %c = map { $_ => sql(qq{SELECT commit_hash FROM dolt_log WHERE message = '$_'}) }
  qw(c1 c2 c3);
my $kept = q{SELECT string_agg(substr(brname, 10, 6), ',' ORDER BY brname)
	FROM pg_branch WHERE brname LIKE 'detached/%'};
sub short { return join(',', sort map { substr($c{$_}, 0, 6) } @_); }

# Keep the named commits, checked out in that order (the first the least
# recently used), nothing else kept.
sub keep
{
	sql(q{SET prolly.kept_checkouts = 0;
		DO $$BEGIN PERFORM dolt_checkout('main'); END$$});
	sql(join('', map { qq{SELECT dolt_checkout('$c{$_}');} } @_)
		  . q{SELECT dolt_checkout('main')});
	is(sql($kept), short(@_), 'kept ' . join(',', @_));
}

# A session on c1; c2 idle.  With a budget of 1, a sweep keeps c2: c1
# isn't idle.
keep('c1', 'c2');
my $a = $node->background_psql('postgres');
$a->query_safe(qq{SELECT dolt_checkout('$c{c1}')});
sql(q{SET prolly.kept_checkouts = 1;
	DO $$BEGIN PERFORM dolt_checkout('main'); END$$});
is(sql($kept), short('c1', 'c2'), 'a budget of 1 keeps the idle one beside the one in use');
sql(q{SET prolly.kept_checkouts = 0;
	DO $$BEGIN PERFORM dolt_checkout('main'); END$$});
is(sql($kept), short('c1'), 'and with none, keeps only the one in use');
$a->query_safe(q{SELECT dolt_checkout('main')});

SKIP:
{
	skip 'injection points not supported by this build', 2
	  unless $ENV{enable_injection_points} eq 'yes';

	# c1, c2, c3 kept, c1 least recently used; a budget of 2.  Y's sweep
	# waits after choosing c1; X's sweep deletes c1 and commits; Y then
	# finds c1 gone and stops there, leaving c2 and c3.
	keep('c1', 'c2', 'c3');
	sql('CREATE EXTENSION injection_points');
	my $y = $node->background_psql('postgres');
	$y->query_safe(q{SELECT injection_points_set_local();
		SELECT injection_points_attach('prolly-kept-evict', 'wait')});
	$y->query_until(qr/started/, q{
		\echo started
		SET prolly.kept_checkouts = 2;
		SELECT dolt_checkout('main');
		\echo done
	});
	$node->poll_query_until('postgres', q{SELECT count(*) = 1 FROM pg_stat_activity
		WHERE wait_event = 'prolly-kept-evict'});
	sql(q{SET prolly.kept_checkouts = 2;
		DO $$BEGIN PERFORM dolt_checkout('main'); END$$});
	is(sql($kept), short('c2', 'c3'), 'one sweep lets c1 go');
	sql(q{SELECT injection_points_wakeup('prolly-kept-evict')});
	$y->query_until(qr/done/, '');
	is(sql($kept), short('c2', 'c3'), 'the other, finding it gone, lets no more go');
	$y->quit;
}

# DROP ROLE lets go of this database's idle kept checkouts whose copies
# name the role, and only when something depends on it.
keep('c1');
sql(q{CREATE ROLE nobody_depends});
sql(q{DROP ROLE nobody_depends; DROP ROLE IF EXISTS nobody_at_all});
is(sql($kept), short('c1'), 'a role nothing depends on keeps the cache');
sql(q{CREATE ROLE reader; GRANT USAGE ON SCHEMA s TO reader;
	GRANT SELECT ON s.t TO reader});
keep('c2');
sql(q{REVOKE SELECT ON s.t FROM reader; REVOKE USAGE ON SCHEMA s FROM reader});
sql(q{DROP ROLE reader});
is(sql($kept), '', 'one its kept checkout named is dropped, the cache let go');

# Another database's kept checkout naming a role: refused, with a hint,
# until that database lets go of it.
sql(q{CREATE DATABASE other});
$node->safe_psql('other', q{
	SET dolt.author = 'Ada Lovelace <ada@example.com>';
	CREATE SCHEMA o VERSIONED;
	CREATE TABLE o.t (id int PRIMARY KEY);
	SELECT dolt_commit('-Am', 'o1');
	INSERT INTO o.t VALUES (1);
	SELECT dolt_commit('-am', 'o2');
});
my $o1 = $node->safe_psql('other', q{SELECT commit_hash FROM dolt_log WHERE message = 'o1'});
sql(q{CREATE ROLE reader2});
$node->safe_psql('other', qq{GRANT USAGE ON SCHEMA o TO reader2;
	SELECT dolt_checkout('$o1'); SELECT dolt_checkout('main');
	REVOKE USAGE ON SCHEMA o FROM reader2});
my ($ret, $stdout, $stderr) = $node->psql('postgres', q{DROP ROLE reader2});
like($stderr, qr/1 object in database other/, 'another database\'s kept checkout keeps the role');
like($stderr, qr/HINT:  In a database named, .*kept detached checkouts \(prolly.kept_checkouts\) may hold copies/, 'with a hint');
$node->safe_psql('other', q{SET prolly.kept_checkouts = 0;
	DO $$BEGIN PERFORM dolt_checkout('main'); END$$});
sql(q{DROP ROLE reader2});
pass('dropped once that database lets it go');

$a->quit;
$node->stop;
done_testing();
