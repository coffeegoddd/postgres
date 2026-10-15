# Copyright (c) 2026, PostgreSQL Global Development Group

# Branches made with dolt_branch survive crashes and reach standbys, and
# "dbname/branch" connections use them; a branch some session is on can't
# be deleted.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('branches_primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', qq{
autovacuum = off
checkpoint_timeout = 1h
});
$primary->start;
$primary->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('branches_standby');
$standby->init_from_backup($primary, 'base', has_streaming => 1);
$standby->append_conf('postgresql.conf', qq{
shared_buffers = 32MB
bgwriter_lru_maxpages = 0
});
$standby->start;

sub sql { return $primary->safe_psql($_[1] // 'postgres', $_[0]); }
sub ssql { return $standby->safe_psql($_[1] // 'postgres', $_[0]); }

my $rows = q{SELECT count(*) || ':' || sum(id) || ':' || sum(length(v)) FROM app.t};

sql(q{
	CREATE SCHEMA app VERSIONED;
	CREATE TABLE app.t (id int PRIMARY KEY, v text);
	INSERT INTO app.t SELECT g, repeat('x', g) FROM generate_series(1, 5000) g;
	SELECT dolt_branch('feature');
});
my $main_rows = sql($rows);
sql(q{UPDATE app.t SET v = 'feature' WHERE id <= 100; DELETE FROM app.t WHERE id > 4900},
	'postgres/feature');
my $feature_rows = sql($rows, 'postgres/feature');
isnt($feature_rows, $main_rows, 'the branch changed');
is(sql($rows), $main_rows, 'main did not');

$primary->wait_for_replay_catchup($standby);
is(ssql($rows), $main_rows, 'a standby reads main');
is(ssql($rows, 'postgres/feature'), $feature_rows, 'and the branch');

$primary->stop('immediate');
$primary->start;
is(sql($rows), $main_rows, 'main survives a crash');
is(sql($rows, 'postgres/feature'), $feature_rows, 'and so does the branch');

# A session on a branch keeps it from being deleted.
my $session = $primary->background_psql('postgres/feature');
$session->query_safe('SELECT 1');
my ($ret, $stdout, $stderr) = $primary->psql('postgres',
	q{SELECT dolt_branch('-d', 'feature')});
like($stderr, qr/branch "feature" is checked out in another session/,
	'a branch in use can\'t be deleted');
$session->quit;
sql(q{SELECT dolt_branch('-d', 'feature')});
is(sql(q{SELECT string_agg(name, ',') FROM dolt_branches()}), 'main',
	'once no session is on it, it can');
is(sql($rows), $main_rows, 'main keeps its rows');
$primary->wait_for_replay_catchup($standby);
is(ssql($rows), $main_rows, 'on the standby too');

$standby->stop;
$primary->stop;
done_testing();
