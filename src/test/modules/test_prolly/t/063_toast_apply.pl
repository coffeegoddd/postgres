# Copyright (c) 2026, PostgreSQL Global Development Group

# A reset or merge writes the hash pointers history's value trees make,
# unread: the rows hold the trees history has, whether the applier updates
# a row in place or inserts it (a table with another unique index), and
# the chunk store doesn't grow for values that didn't change.  In another
# encoding text is read (its size is only known converted back); bytea
# isn't.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('apply');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;
$node->safe_psql('postgres', q{CREATE DATABASE latin1 ENCODING 'LATIN1'
	LC_COLLATE 'C' LC_CTYPE 'C' TEMPLATE template0});

# Run sql with every read of a value tree by a decoder failing.
sub unread
{
	my ($db, $sql) = @_;
	my ($ret, $out, $err) = $node->psql($db, qq{
		SELECT injection_points_set_local();
		SELECT injection_points_attach('prolly-value-read', 'error');
		$sql;
	});
	return $ret == 0 ? 'unread' : ($err =~ /prolly-value-read/ ? 'read' : $err);
}

my $roots = q{SELECT string_agg(id || ':' || coalesce((test_toast_pointer(t)).root, '-')
		|| ':' || coalesce((test_toast_pointer(b)).root, '-')
		|| ':' || coalesce((test_toast_pointer(j)).root, '-'), ',' ORDER BY id)
	FROM %s};
my $journal = q{SELECT sum(chunks) FROM test_chunk_segments()};

for my $db ('postgres', 'latin1')
{
	my $sql = sub { return $node->safe_psql($db, $_[0]); };

	$sql->(q{
		CREATE EXTENSION test_prolly;
		CREATE EXTENSION injection_points;
		CREATE SCHEMA a VERSIONED;
		CREATE TABLE a.docs (id int PRIMARY KEY, n int, t text, b bytea, j jsonb);
		CREATE TABLE a.unique2 (id int PRIMARY KEY, n int UNIQUE, t text, b bytea, j jsonb);
		INSERT INTO a.docs
		SELECT g, g, string_agg(md5(g || '/' || i), '' ORDER BY i),
			   decode(string_agg(md5(g || '|' || i), '' ORDER BY i), 'hex'),
			   jsonb_build_object('g', g, 'parts', jsonb_agg(md5(g || '-' || i)))
		FROM generate_series(1, 3) g, generate_series(1, 3000) i GROUP BY g;
		INSERT INTO a.unique2 SELECT * FROM a.docs;
		SELECT length(dolt_commit('-Am', 'docs')::text);
	});
	my $docs = $sql->(sprintf($roots, 'a.docs'));
	my $unique2 = $sql->(sprintf($roots, 'a.unique2'));

	# A reset to it writes its trees back, unread.
	$sql->(q{
		UPDATE a.docs SET n = n + 10, t = t || 'x', b = b || 'x', j = j || '{"x": 1}';
		UPDATE a.unique2 SET n = n + 10, t = t || 'x', b = b || 'x', j = j || '{"x": 1}';
		SELECT length(dolt_commit('-am', 'changed')::text);
	});
	my $before = $sql->($journal);
	my $result = unread($db, q{SELECT FROM dolt_reset('--hard', 'HEAD~1')});
	if ($db eq 'postgres')
	{
		is($result, 'unread', "$db: a reset reads no value");
		is($sql->(sprintf($roots, 'a.docs')), $docs,
			"$db: the rows it updates hold history's trees");
		is($sql->(sprintf($roots, 'a.unique2')), $unique2,
			"$db: and those it inserts");
		ok($sql->($journal) - $before < 10,
			"$db: the chunk store didn't grow for them");
	}
	else
	{
		is($result, 'read', "$db: a reset reads text, to know its size");
		$sql->(q{SELECT FROM dolt_reset('--hard', 'HEAD~1')});
	}
	is($sql->(q{SELECT count(*) FROM a.docs
		WHERE md5(t) = md5((SELECT string_agg(md5(id || '/' || i), '' ORDER BY i)
							FROM generate_series(1, 3000) i)) AND n = id}), '3',
		"$db: the values read back");

	# A merge of another branch's counters keeps the rows' values, unread.
	$sql->(q{SELECT dolt_branch('counting')});
	my $s = $node->background_psql($db);
	$s->query_safe(q{SELECT dolt_checkout('counting')});
	$s->query_safe(q{UPDATE a.docs SET n = n + 100});
	$s->query_safe(q{SELECT length(dolt_commit('-am', 'counted')::text)});
	$s->quit;
	$sql->(q{
		UPDATE a.unique2 SET n = n + 1000;
		SELECT length(dolt_commit('-am', 'main')::text);
	});
	$result = unread($db, q{SELECT FROM dolt_merge('counting')});
	is($result, 'unread', "$db: a merge reads no value");
	$sql->(q{SELECT FROM dolt_merge('counting')}) if $result eq 'read';
	is($sql->(q{SELECT string_agg(n::text, ',' ORDER BY id) FROM a.docs}), '101,102,103',
		"$db: merged");
}

# bytea alone, in LATIN1: unread.
$node->safe_psql('latin1', q{
	CREATE TABLE a.bytes (id int PRIMARY KEY, b bytea);
	INSERT INTO a.bytes SELECT 1, decode(string_agg(md5(i::text), ''), 'hex')
	FROM generate_series(1, 3000) i;
	SELECT length(dolt_commit('-Am', 'bytes')::text);
	UPDATE a.bytes SET b = 'x';
});
is(unread('latin1', q{SELECT FROM dolt_reset('--hard')}), 'unread',
	'latin1: bytea is written from history unread');

$node->stop;
done_testing();
