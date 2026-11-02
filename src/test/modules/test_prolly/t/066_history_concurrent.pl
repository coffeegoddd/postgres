# Copyright (c) 2026, PostgreSQL Global Development Group

# Many backends writing the history store at once, as commits do: each
# transaction puts leaves and a parent referencing them, and moves a
# pointer of its own (its new root taken, its old let go of), the chunks
# the old held then nobody's; others free what nobody holds, and VACUUM
# (and its freer, in the background) takes out what is freed.  After,
# the audit is clean: every count the edges to its chunk, every pointer's
# chunk there with its roots, no live chunk's bytes counted dead; and once
# what nobody holds is freed, nothing is left but what the pointers hold.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('history_concurrent');
$node->init;
$node->append_conf('postgresql.conf', qq{
autovacuum = off
max_connections = 40
});
$node->start;
$node->safe_psql('postgres', q{
	CREATE EXTENSION test_prolly;
	CREATE FUNCTION h(d bytea) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS 'SELECT substr(sha256(d), 1, 20)';
	CREATE FUNCTION leaf(i int) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS $$SELECT 'L'::bytea || decode(repeat(md5('w' || i), 8), 'hex')$$;
	-- (a chunk of the test format referencing others: 'R', their count, them)
	CREATE FUNCTION node(VARIADIC kids bytea[]) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS $$SELECT 'R'::bytea || set_byte('\x00'::bytea, 0, cardinality(kids)) ||
			(SELECT string_agg(h(k), '' ORDER BY o) FROM unnest(kids) WITH ORDINALITY u(k, o))$$;
	-- a commit: leaves (some another's too) and a parent of them put, and the
	-- branch's pointer moved to it
	CREATE FUNCTION commit_to(branch text, a int, b int, c int) RETURNS void LANGUAGE plpgsql AS $$
	BEGIN
		PERFORM test_chunk_format_refs(true);
		-- (the leaves in their names' order, as a pack puts them: two
		-- commits putting two leaves new to the store each in the other's
		-- order would wait for each other's, as inserts into a unique index)
		PERFORM test_cs_put('pages', d)
			FROM (SELECT leaf(x) AS d FROM unnest(ARRAY[a, b, c]) x) l ORDER BY h(d);
		PERFORM test_cs_put('pages', node(leaf(a), leaf(b), leaf(c)));
		UPDATE pg_history.pointers SET hash = h(node(leaf(a), leaf(b), leaf(c))) WHERE name = branch;
	END$$;
	SELECT test_chunk_format_refs(true);
	SELECT test_cs_put('pages', leaf(0));
	INSERT INTO pg_history.pointers SELECT 'b' || i, h(leaf(0)) FROM generate_series(1, 16) i;
});

$node->pgbench(
	'--no-vacuum --client=16 --jobs=4 --transactions=1500',
	0,
	[qr{processed: 24000/24000}],
	[qr{^$}],
	'sixteen clients commit, free and vacuum at once',
	{
		'066_commit@10' => q{
\set branch random(1, 16)
\set a random(1, 2000)
\set b random(1, 2000)
\set c random(1, 2000)
SELECT commit_to('b' || :branch, :a, :b, :c);
},
		'066_free@1' => q{
SELECT test_chunk_format_refs(true), test_cs_free_unheld('pages');
},
		'066_vacuum@1' => q{
VACUUM pg_history.chunk_index;
},
	});

my $refs = 'SELECT test_chunk_format_refs(true);';
my $q = sub {
	my $out = $node->safe_psql('postgres', "$refs $_[0]");
	$out =~ s/^\n//;
	return $out;
};
is($q->(q{SELECT ok || ' ' || coalesce(why, '') FROM test_cs_check('pages')}), 'true ',
	'after, the audit clean');
is($q->(q{SELECT count(*) FILTER (WHERE test_cs_has('pages', hash)
	AND (SELECT roots FROM test_cs_counts('pages', hash)) >= 1) FROM pg_history.pointers}), '16',
	'every pointer\'s chunk there, with its root');

# what nobody holds freed, and taken out: what is left the pointers' alone
$q->(q{SELECT test_cs_free_unheld('pages')});
$node->safe_psql('postgres', 'VACUUM pg_history.chunk_index');
is($q->(q{SELECT ok || ' ' || unheld || ' ' || coalesce(why, '') FROM test_cs_check('pages')}), 'true 0 ',
	'nothing unheld left, the audit clean');
is($q->(q{SELECT (SELECT chunks FROM test_cs_check('pages')) =
	(SELECT count(*) FROM (SELECT DISTINCT hash FROM pg_history.pointers
		UNION SELECT substr(test_cs_get('pages', p.hash), 3 + 20 * j, 20)
		FROM pg_history.pointers p, generate_series(0, 2) j
		WHERE get_byte(test_cs_get('pages', p.hash), 0) = 82) s)}), 't',
	'the chunks left are the pointers\' and the leaves they reference');

$node->stop;
done_testing();
