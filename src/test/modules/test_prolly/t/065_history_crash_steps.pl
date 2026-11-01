# Copyright (c) 2026, PostgreSQL Global Development Group

# A crash between the records of each step the history store makes of
# several: a chunk's bytes appended a page at a time; a put's references
# counted one at a time; a free's chunk marked, then its references let
# go of; a pointer moved, its new root taken, then its old let go of.
# Each step is its transaction's: after the crash, the store is as before
# it (the audit clean, counts and roots as they were), and so after
# VACUUM takes out what the crash cut short.  Replay is checked page for
# page (wal_consistency_checking).

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('crash_steps');
$node->init;
$node->append_conf('postgresql.conf', qq{
autovacuum = off
chunk_free_in_background = off
wal_consistency_checking = 'ChunkLog, ChunkIndex'
});
$node->start;
$node->safe_psql('postgres', q{
	CREATE EXTENSION test_prolly;
	CREATE EXTENSION injection_points;
	CREATE FUNCTION h(d bytea) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS 'SELECT substr(sha256(d), 1, 20)';
	CREATE FUNCTION leaf(i int) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS $$SELECT 'L'::bytea || decode(repeat(md5('c' || i), 30), 'hex')$$;
	-- (longer than a region: its bytes many pages, a record each)
	CREATE FUNCTION huge(i int) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS $$SELECT 'L'::bytea || decode(repeat(md5('c' || i), 20000), 'hex')$$;
	-- (a chunk of the test format referencing others: 'R', their count, them)
	CREATE FUNCTION node(VARIADIC kids bytea[]) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS $$SELECT 'R'::bytea || set_byte('\x00'::bytea, 0, cardinality(kids)) ||
			(SELECT string_agg(h(k), '' ORDER BY o) FROM unnest(kids) WITH ORDINALITY u(k, o))$$;
	CREATE FUNCTION p1() RETURNS bytea LANGUAGE sql IMMUTABLE AS 'SELECT node(leaf(1), leaf(2), leaf(3))';
	CREATE FUNCTION p2() RETURNS bytea LANGUAGE sql IMMUTABLE AS 'SELECT node(leaf(1))';
	SELECT count(*) FROM generate_series(1, 3) i, LATERAL test_cs_put('pages', leaf(i));
});

# (every session of the test format, its references counted)
my $refs = 'SELECT test_chunk_format_refs(true);';
# (the last statement's output: the format's call prints a line of its own)
my $q = sub {
	my $out = $node->safe_psql('postgres', "$refs $_[0]");
	$out =~ s/^\n//;
	return $out;
};
my $audit = sub {
	return $q->(q{SELECT ok || ' ' || coalesce(why, '') FROM test_cs_check('pages')});
};

# a session stopped at point in what it runs, the WAL before flushed, and
# a crash
my $crash_at = sub {
	my ($point, $sql) = @_;
	my $s = $node->background_psql('postgres', on_error_stop => 0);
	$s->query_safe($refs);
	$s->query_safe(q{SELECT injection_points_set_local()});
	$s->query_safe("SELECT injection_points_attach('$point', 'wait')");
	$s->query_until(qr/stepping/, "\\echo stepping\n$sql;\n");
	$node->wait_for_event('client backend', $point);
	# (a commit of another's that wrote, flushing the WAL of the step's
	# records before it)
	$node->safe_psql('postgres', "CREATE TABLE flushed_" . ($point =~ tr/-/_/r) . " ()");
	$node->stop('immediate');
	$s->{run}->finish;
	$node->start;
};
my $after = sub {
	my ($what) = @_;
	is($audit->(), 'true ', "$what: after the crash, the audit clean");
	$node->safe_psql('postgres', 'VACUUM pg_history.chunk_index');
	is($audit->(), 'true ', "$what: and after VACUUM");
};

# 1. a chunk's bytes, a page at a time: the crash after the first
$crash_at->('chunk-log-append-page', q{SELECT test_cs_put('pages', huge(1))});
is($q->(q{SELECT test_cs_has('pages', h(huge(1)))}), 'f', 'append: the chunk is not there');
$after->('append');
is($q->(q{SELECT (test_cs_put('pages', huge(1))).stored}), 't', 'append: put again after');

# 2. a put's references, a count at a time: the crash after the first
$crash_at->('chunk-put-ref-counted', q{SELECT test_cs_put('pages', p1())});
is($q->(q{SELECT test_cs_has('pages', h(p1()))}), 'f', 'counts: the chunk is not there');
is($q->(q{SELECT string_agg(refs::text, ',' ORDER BY i) FROM generate_series(1, 3) i,
	LATERAL test_cs_counts('pages', h(leaf(i)))}), '0,0,0', 'counts: no reference counted');
$after->('counts');

# 3. a free: the chunk marked, then its references let go of
$q->(q{SELECT (test_cs_put('pages', p1())).stored});
$crash_at->('chunk-free-marked', q{SELECT test_cs_free('pages', ARRAY[h(p1())])});
is($q->(q{SELECT test_cs_has('pages', h(p1()))}), 't', 'free: the chunk still there');
is($q->(q{SELECT string_agg(refs::text, ',' ORDER BY i) FROM generate_series(1, 3) i,
	LATERAL test_cs_counts('pages', h(leaf(i)))}), '1,1,1', 'free: its references still counted');
$after->('free');

# 4. a pointer moved: its new root taken, then its old let go of
$q->(q{SELECT (test_cs_put('pages', p2())).stored;
	INSERT INTO pg_history.pointers VALUES ('main', h(p1()))});
$crash_at->('history-pointer-moving',
	q{UPDATE pg_history.pointers SET hash = h(p2()) WHERE name = 'main'});
is($q->(q{SELECT hash = h(p1()) FROM pg_history.pointers WHERE name = 'main'}), 't',
	'pointer move: the pointer where it was');
is($q->(q{SELECT (SELECT roots FROM test_cs_counts('pages', h(p1()))) || ',' ||
	(SELECT roots FROM test_cs_counts('pages', h(p2())))}), '1,0', 'pointer move: the roots as they were');
$after->('pointer move');

$node->stop;
done_testing();
