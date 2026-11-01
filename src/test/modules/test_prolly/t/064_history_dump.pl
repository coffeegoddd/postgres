# Copyright (c) 2026, PostgreSQL Global Development Group

# pg_dump dumps a database's version-control history store with it: its
# chunks, each after those it references, then its pointers; pg_restore
# puts the chunks back (each checked against its name) before the
# pointers, which take their roots.  Plain, custom and parallel directory
# formats all keep it; --no-history leaves it out; a role that cannot read
# it cannot dump the database whole without saying so.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('history_dump');
$node->init;
$node->append_conf('postgresql.conf', qq{
shared_preload_libraries = 'test_prolly'
test_prolly.chunk_format_refs = on
autovacuum = off
chunk_free_in_background = off
});
$node->start;
$node->safe_psql('postgres', 'CREATE DATABASE src');
$node->safe_psql('src', q{
	CREATE EXTENSION test_prolly;
	CREATE FUNCTION h(d bytea) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS 'SELECT substr(sha256(d), 1, 20)';
	CREATE FUNCTION leaf(i int) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS $$SELECT 'L'::bytea || decode(repeat(md5('d' || i), 40), 'hex')$$;
	-- (a chunk of the test format referencing others: 'R', their count, them)
	CREATE FUNCTION node(VARIADIC kids bytea[]) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS $$SELECT 'R'::bytea || set_byte('\x00'::bytea, 0, cardinality(kids)) ||
			(SELECT string_agg(h(k), '' ORDER BY o) FROM unnest(kids) WITH ORDINALITY u(k, o))$$;
	CREATE TABLE t AS SELECT i FROM generate_series(1, 10) i;
	-- 300 leaves, 30 parents of ten each, a root of the parents, and a
	-- second root sharing half of them
	SELECT count(*) FROM generate_series(1, 300) i, LATERAL test_cs_put('pages', leaf(i));
	CREATE TABLE parents AS
		SELECT p, node(VARIADIC ARRAY(SELECT leaf(p * 10 + j) FROM generate_series(1, 10) j)) AS d
		FROM generate_series(0, 29) p;
	SELECT count(*) FROM parents, LATERAL test_cs_put('pages', d);
	SELECT test_cs_put('pages', node(VARIADIC ARRAY(SELECT d FROM parents ORDER BY p)));
	SELECT test_cs_put('pages', node(VARIADIC ARRAY(SELECT d FROM parents WHERE p < 15 ORDER BY p)));
	INSERT INTO pg_history.pointers VALUES
		('main', h(node(VARIADIC ARRAY(SELECT d FROM parents ORDER BY p)))),
		('half', h(node(VARIADIC ARRAY(SELECT d FROM parents WHERE p < 15 ORDER BY p))));
	-- (a loose leaf, held by nothing)
	SELECT test_cs_put('pages', leaf(1000));
});

my $state = q{SELECT (SELECT chunks || ' ' || unheld || ' ' || ok FROM test_cs_check('pages')) || ' / ' ||
	(SELECT string_agg(name || ':' || encode(hash, 'hex'), ',' ORDER BY name) FROM pg_history.pointers) || ' / ' ||
	(SELECT string_agg(roots::text, ',' ORDER BY name) FROM pg_history.pointers p, LATERAL test_cs_counts('pages', p.hash))};
my $want = $node->safe_psql('src', $state);
like($want, qr/^333 1 true \/ half:\w+,main:\w+ \/ 1,1$/, 'the source store: 333 chunks, one unheld, two pointers');

my $dir = PostgreSQL::Test::Utils::tempdir;
my $restore = sub {
	my ($name, @restore_cmd) = @_;
	$node->safe_psql('postgres', "CREATE DATABASE $name TEMPLATE template0");
	$node->command_ok([@restore_cmd, '--dbname' => $node->connstr($name)], "restored into $name");
	return $node->safe_psql($name, $state);
};

# custom format
$node->command_ok([ 'pg_dump', '--format' => 'custom', '--file' => "$dir/src.dump", $node->connstr('src') ],
	'pg_dump, custom format');
is($restore->('dc', 'pg_restore', "$dir/src.dump"), $want, 'custom format: the store as it was');
is($node->safe_psql('dc', 'SELECT count(*) FROM t'), '10', 'and the tables');

# plain, through psql
$node->command_ok([ 'pg_dump', '--file' => "$dir/src.sql", $node->connstr('src') ], 'pg_dump, plain');
$node->safe_psql('postgres', 'CREATE DATABASE dp TEMPLATE template0');
$node->command_ok([ 'psql', '--no-psqlrc', '--set' => 'ON_ERROR_STOP=1', '--file' => "$dir/src.sql",
	'--dbname' => $node->connstr('dp') ], 'restored into dp');
is($node->safe_psql('dp', $state), $want, 'plain format: the store as it was');

# directory format, restored in parallel
$node->command_ok([ 'pg_dump', '--format' => 'directory', '--jobs' => 2, '--file' => "$dir/srcdir",
	$node->connstr('src') ], 'pg_dump, directory format');
is($restore->('dd', 'pg_restore', '--jobs' => 2, "$dir/srcdir"), $want, 'parallel restore: the store as it was');

# --no-history
$node->command_ok([ 'pg_dump', '--no-history', '--file' => "$dir/nohist.sql", $node->connstr('src') ],
	'pg_dump --no-history');
my $nohist = slurp_file("$dir/nohist.sql");
unlike($nohist, qr/pg_history/, 'no history in it');

# a role that cannot read the store
$node->safe_psql('src', 'CREATE ROLE dumper LOGIN; GRANT pg_read_all_data TO dumper');
my ($out, $err) = run_command([ 'pg_dump', '--file' => "$dir/role.sql",
	$node->connstr('src') . ' user=dumper' ]);
like($err, qr/cannot read the version-control history store.*--no-history/,
	'a role that cannot read the store may not dump the database whole');
# (one with no history: dumped all the same)
$node->safe_psql('postgres', 'CREATE DATABASE nohist');
$node->safe_psql('nohist', 'CREATE ROLE dumper2 LOGIN; GRANT pg_read_all_data TO dumper2; CREATE TABLE x (i int)');
$node->command_ok([ 'pg_dump', '--file' => "$dir/nohist_role.sql", $node->connstr('nohist') . ' user=dumper2' ],
	'a role that cannot read the store dumps a database with no history');

$node->stop;
done_testing();
