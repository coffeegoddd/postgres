# Copyright (c) 2026, PostgreSQL Global Development Group

# A merge whose foreign key check fails leaves nothing of its recording
# behind: the session's later resets and merges write and record as they
# should.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('violations');
$node->init;
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{CREATE EXTENSION test_prolly;
	CREATE EXTENSION injection_points;
	CREATE SCHEMA cv VERSIONED;
	CREATE TABLE cv.parent (id int PRIMARY KEY);
	CREATE TABLE cv.child (id int PRIMARY KEY, pid int REFERENCES cv.parent);
	INSERT INTO cv.parent VALUES (1), (2);
	INSERT INTO cv.child VALUES (1, 1);
	SELECT dolt_commit('-Am', 'base');
	SELECT dolt_branch('feature');
	SELECT dolt_checkout('feature');
	INSERT INTO cv.child VALUES (2, 2);
	SELECT dolt_commit('-am', 'a child of 2');
	SELECT dolt_checkout('main');
	DELETE FROM cv.parent WHERE id = 2;
	SELECT dolt_commit('-am', 'no parent 2');
	SELECT dolt_branch('ahead');
	SELECT dolt_checkout('ahead');
	INSERT INTO cv.parent VALUES (3);
	SELECT dolt_commit('-am', 'a parent ahead');
	SELECT dolt_checkout('main')});

my $s = $node->background_psql('postgres', on_error_stop => 0);
$s->query_safe(q{SELECT injection_points_set_local();
	SELECT injection_points_attach('prolly-merge-foreign-keys', 'error')});
$s->query(q{BEGIN; SELECT dolt_merge('feature')});
like($s->{stderr}, qr/error triggered for injection point prolly-merge-foreign-keys/,
	'the merge\'s foreign key check fails');
$s->{stderr} = '';
$s->query_safe(q{ROLLBACK;
	SELECT injection_points_detach('prolly-merge-foreign-keys')});

# The same session writes rows as merges do (a fast-forward over
# uncommitted changes), and merges again.
$s->query_safe(q{INSERT INTO cv.child VALUES (3, 1)});
is($s->query_safe(q{SELECT (dolt_merge('ahead'))[2:4]}), '{1,0,"merge successful"}',
	'a fast-forward after it works');
is($s->query_safe(q{SELECT count(*) FROM cv.parent WHERE id = 3}), '1', 'and writes its rows');
is($s->query_safe(q{SELECT dolt_reset('--hard')}), '{0}', 'a reset too');
is($s->query_safe(q{BEGIN; SELECT (dolt_merge('feature'))[3:4]}),
	'{1,"conflicts found"}', 'and a merge records its violations');
is($s->query_safe(q{SELECT violation_type || ' ' || violating_row
	FROM dolt_constraint_violations('cv.child'); ROLLBACK}),
	'foreign key {"id": 2, "pid": 2}', 'just the one it left');
$s->quit;
is(sql('SELECT 1'), '1', 'the server is up');

$node->stop;
done_testing();
