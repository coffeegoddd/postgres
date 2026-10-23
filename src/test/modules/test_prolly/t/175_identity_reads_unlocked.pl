# Copyright (c) 2026, PostgreSQL Global Development Group

# A historical read of one table works out the working set's identities
# (which renames keep) from the catalogs alone: it doesn't open, so doesn't
# wait for the locks of, any other table, nor fail on one dropped meanwhile.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('identity_reads_unlocked');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;
sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{CREATE SCHEMA app VERSIONED;
	CREATE TABLE app.x (id int PRIMARY KEY, v text);
	CREATE TABLE app.y (id int PRIMARY KEY, w text);
	INSERT INTO app.x VALUES (1, 'one');
	INSERT INTO app.y VALUES (1, 'one');
	SELECT dolt_commit('-Am', 'c1');
	ALTER TABLE app.x RENAME COLUMN v TO renamed;
	SELECT dolt_commit('-Am', 'c2')});

my $locker = $node->background_psql('postgres');
$locker->query_safe(q{BEGIN; LOCK TABLE app.y IN ACCESS EXCLUSIVE MODE});

my ($ret, $out, $err) = $node->psql('postgres', q{
	SET lock_timeout = '2s';
	SELECT string_agg(id || ':' || renamed, ',') FROM dolt_at('HEAD~1', NULL::app.x)});
is($ret, 0, 'a read of app.x does not wait for app.y\'s lock');
is($out, '1:one', 'and reads the renamed column\'s stored value');
is($err, '', 'without an error');

($ret, $out, $err) = $node->psql('postgres', q{
	SET lock_timeout = '2s';
	SELECT count(*) FROM dolt_diff_table(NULL::app.x)});
is($ret, 0, 'nor does its history');

$locker->query_safe('COMMIT');
$locker->quit;
$node->stop;
done_testing();
