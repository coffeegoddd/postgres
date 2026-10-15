# Copyright (c) 2026, PostgreSQL Global Development Group

# VACUUM gives forgotten TIDs out again, but never ones a commit or
# another branch may still hold: a branch made before the VACUUM keeps its
# rows, and a reset to a commit made before it gets them back, so new rows
# there must not take their TIDs.  Without commits or branches, TIDs are
# reused as before.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('tid_reuse');
$node->init;
$node->start;
$node->safe_psql('postgres', 'CREATE DATABASE v VERSIONED true; CREATE DATABASE plain VERSIONED true;');

sub sql { return $node->safe_psql($_[1] // 'v', $_[0]); }

my $distinct = q{SELECT count(*) || '|' || count(DISTINCT ctid) FROM t};

# Another branch.
sql(q{
	CREATE TABLE t (id int PRIMARY KEY, v text);
	INSERT INTO t SELECT g, 'v' || g FROM generate_series(1, 10) g;
	SELECT dolt_commit('-m', 'base');
	SELECT dolt_branch('b');
	DELETE FROM t WHERE id <= 8;
});
sql('VACUUM t');
sql('VACUUM t');
sql(q{INSERT INTO t SELECT g, 'b' || g FROM generate_series(101, 110) g}, 'v/b');
is(sql($distinct, 'v/b'), '20|20', 'a branch\'s new rows don\'t take its rows\' TIDs');
is(sql(q{SET enable_seqscan = off; SELECT count(*) FROM t WHERE ctid = '(0,3)'}, 'v/b'), '1',
	'one row per TID');

# A reset to a commit made before the VACUUM.
sql(q{INSERT INTO t SELECT g, 'm' || g FROM generate_series(201, 210) g});
sql(q{SELECT dolt_reset('--hard', 'HEAD')});
sql(q{INSERT INTO t SELECT g, 'r' || g FROM generate_series(301, 310) g});
is(sql($distinct), '20|20', 'nor do new rows after a reset to an earlier commit');

# No commits or branches: TIDs are reused.
sql(q{
	CREATE TABLE t (id int PRIMARY KEY, v text);
	INSERT INTO t SELECT g, 'v' || g FROM generate_series(1, 10) g;
	DELETE FROM t WHERE id <= 8;
}, 'plain');
sql('VACUUM t', 'plain');
sql('VACUUM t', 'plain');
my $ctid = sql(q{INSERT INTO t VALUES (100, 'x') RETURNING ctid}, 'plain');
like($ctid, qr/^\(0,[1-8]\)$/, 'without commits or branches, a freed TID is reused');

$node->stop;
done_testing();
