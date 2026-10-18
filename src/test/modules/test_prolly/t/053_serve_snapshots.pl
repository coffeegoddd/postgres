# Copyright (c) 2026, PostgreSQL Global Development Group

# A push's move reads the branch it moves as of its head: a transaction
# whose snapshot predates the head the refs show can't move it from a
# working set that lacks that head's rows.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('serve_snapshots');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;
$node->safe_psql('postgres', q{
	CREATE SCHEMA ss VERSIONED;
	CREATE TABLE ss.t (id int PRIMARY KEY);
	SELECT dolt_commit('-Am', 'one');
	SELECT dolt_branch('other');
	SELECT dolt_checkout('other');
	INSERT INTO ss.t VALUES (1);
	SELECT dolt_commit('-am', 'two');
});
my $target = $node->safe_psql('postgres', q{SELECT dolt_hashof('main')});

# a pusher whose snapshot is taken
my $pusher = $node->background_psql('postgres', on_error_stop => 0);
$pusher->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
$pusher->query_safe('SELECT 1');

# the branch moves after it
$node->safe_psql('postgres', q{
	SELECT dolt_checkout('other');
	INSERT INTO ss.t VALUES (2);
	SELECT dolt_commit('-am', 'three');
});
my $head = $node->safe_psql('postgres', q{SELECT dolt_hashof('other')});
my $fmt = $node->safe_psql('postgres', q{SELECT dolt_serve_format()});

my ($out, $ret) = $pusher->query(qq{
	SELECT dolt_serve_move_head('other', decode('$head', 'hex'), decode('$target', 'hex'),
								'$fmt', false)});
like($pusher->{stderr}, qr/could not serialize access/,
	'a snapshot older than the head refuses the move');
$pusher->query('ROLLBACK');
$pusher->quit;

is($node->safe_psql('postgres', q{SELECT dolt_hashof('other')}), $head,
	'and the branch stays at its head');
is($node->safe_psql('postgres', q{
	SELECT dolt_checkout('other');
	SELECT string_agg(id::text, ',' ORDER BY id) FROM ss.t}), "{0,\"Switched to branch 'other'\"}\n1,2",
	'with its rows');

done_testing();
