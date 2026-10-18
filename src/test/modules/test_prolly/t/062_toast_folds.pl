# Copyright (c) 2026, PostgreSQL Global Development Group

# A fold doesn't read a value stored by content whose tree holds the
# column's canonical bytes: bytea, text in a UTF-8 database or converted
# in another, jsonb.  It writes the tree's reference from the pointer.  It
# reads one short enough to be inline, and one stored in another form.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('folds');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;
$node->safe_psql('postgres', q{CREATE DATABASE latin1 ENCODING 'LATIN1'
	LC_COLLATE 'C' LC_CTYPE 'C' TEMPLATE template0});

# Commit with every fetch of a value by content failing.
sub commit_unread
{
	my ($db, $msg) = @_;
	my ($ret, $out, $err) = $node->psql($db, qq{
		SELECT injection_points_set_local();
		SELECT injection_points_attach('prolly-toast-fetch', 'error');
		SELECT length(dolt_commit('-Am', '$msg')::text) > 0;
	});
	return $ret == 0 ? 'committed' : ($err =~ /prolly-toast-fetch/ ? 'read' : $err);
}

for my $db ('postgres', 'latin1')
{
	$node->safe_psql($db, q{
		CREATE EXTENSION test_prolly;
		CREATE EXTENSION injection_points;
		CREATE SCHEMA f VERSIONED;
		CREATE TABLE f.docs (id int PRIMARY KEY, t text, b bytea, j jsonb, v varchar);
		INSERT INTO f.docs
		SELECT g, string_agg(md5(g || '/' || i), '' ORDER BY i),
			   decode(string_agg(md5(g || '|' || i), '' ORDER BY i), 'hex'),
			   jsonb_build_object('g', g, 'parts', jsonb_agg(md5(g || '-' || i))),
			   string_agg(md5(g || ':' || i), '' ORDER BY i)
		FROM generate_series(1, 3) g, generate_series(1, 3000) i GROUP BY g;
	});
	is(commit_unread($db, 'docs'), 'committed',
		"$db: a commit doesn't read values stored by content");

	# a short value moved out (its row was long) is inline in history
	$node->safe_psql($db, q{
		CREATE TABLE f.short (id int PRIMARY KEY, a text, b text, c text);
		INSERT INTO f.short
		SELECT 1, string_agg(md5(i::text), '') FILTER (WHERE i <= 60),
			   string_agg(md5(i::text || 'b'), ''), string_agg(md5(i::text || 'c'), '')
		FROM generate_series(1, 120) i;
	});
	is(commit_unread($db, 'short'), 'read', "$db: a commit reads a short value");
	$node->safe_psql($db, q{SELECT length(dolt_commit('-Am', 'short')::text)});

	# a type whose canonical bytes aren't what the tree holds is read
	$node->safe_psql($db, q{
		CREATE TABLE f.arrays (id int PRIMARY KEY, a text[]);
		INSERT INTO f.arrays
		SELECT 1, array_agg(md5(i::text)) FROM generate_series(1, 3000) i;
	});
	is(commit_unread($db, 'arrays'), 'read', "$db: a commit reads an array");
	$node->safe_psql($db, q{SELECT length(dolt_commit('-Am', 'arrays')::text)});

	# what was committed reads back
	is($node->safe_psql($db, q{
		UPDATE f.docs SET t = 'x', b = 'x', j = '{}', v = 'x';
		SELECT FROM dolt_reset('--hard');
		SELECT count(*) FROM f.docs
		WHERE md5(t) = md5((SELECT string_agg(md5(id || '/' || i), '' ORDER BY i)
							FROM generate_series(1, 3000) i))}), '3',
		"$db: and reads back from history");
}

$node->stop;
done_testing();
