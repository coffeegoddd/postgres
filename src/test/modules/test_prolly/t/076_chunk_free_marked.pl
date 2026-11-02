
# Copyright (c) 2026, PostgreSQL Global Development Group

# Freeing costs what is freed.  A transaction letting go of a chunk's last
# hold (a pointer moved or dropped, a root let go of) marks its bucket of
# the chunk index, and its commit starts a freer, which frees what nothing
# holds in the buckets marked, and what they alone held, and unmarks them:
# no VACUUM, and no walk of the index.  Two letting go of a chunk's last
# references at once, each seeing the other's left, mark it too.  The
# marks survive a crash, and follow their chunks as buckets split.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('free_marked');
$node->init;
$node->append_conf('postgresql.conf', qq{
shared_preload_libraries = 'test_prolly'
test_prolly.chunk_format_refs = on
autovacuum = off
log_min_messages = debug1
});
$node->start;
$node->safe_psql('postgres', q{
	CREATE EXTENSION test_prolly;
	CREATE FUNCTION h(d bytea) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS 'SELECT substr(sha256(d), 1, 20)';
	CREATE FUNCTION leaf(i int) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS $$SELECT 'L'::bytea || decode(md5('m' || i), 'hex')$$;
	CREATE FUNCTION node(VARIADIC kids bytea[]) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS $$SELECT 'R'::bytea || set_byte('\x00'::bytea, 0, cardinality(kids)) ||
			(SELECT string_agg(h(k), '' ORDER BY o) FROM unnest(kids) WITH ORDINALITY u(k, o))$$;
	CREATE FUNCTION tree(a int, b int) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS 'SELECT node(VARIADIC ARRAY(SELECT leaf(i) FROM generate_series(a, b) i))';
	CREATE FUNCTION marked() RETURNS int8 LANGUAGE sql
		AS $$SELECT test_chunk_index_marked('pg_history.chunk_index')$$;
});
my $gone = sub {
	my ($hash_sql, $name) = @_;
	ok($node->poll_query_until('postgres',
		"SELECT NOT test_cs_has('pages', $hash_sql)"), $name);
};

# a store of 20,000 chunks a pointer holds, and a small tree another does
$node->safe_psql('postgres', q{
	SELECT count(*) FROM generate_series(1, 20000) i, LATERAL test_cs_put('pages', leaf(i));
	CREATE TABLE parents AS SELECT p, tree(p * 100 + 1, p * 100 + 100) AS d FROM generate_series(0, 199) p;
	SELECT count(*) FROM parents, LATERAL test_cs_put('pages', d);
	SELECT test_cs_put('pages', node(VARIADIC ARRAY(SELECT d FROM parents ORDER BY p)));
	INSERT INTO pg_history.pointers VALUES ('big', h(node(VARIADIC ARRAY(SELECT d FROM parents ORDER BY p))));
	SELECT count(*) FROM generate_series(30001, 30005) i, LATERAL test_cs_put('pages', leaf(i));
	SELECT test_cs_put('pages', tree(30001, 30005));
	INSERT INTO pg_history.pointers VALUES ('small', h(tree(30001, 30005)));
});
is($node->safe_psql('postgres', 'SELECT marked()'), '0', 'no bucket marked');

# the small tree's pointer dropped: freed by the freer its commit starts,
# looking in the buckets marked alone
my $offset = -s $node->logfile;
$node->safe_psql('postgres', q{DELETE FROM pg_history.pointers WHERE name = 'small'});
$gone->(q{h(tree(30001, 30005))}, 'the dropped pointer\'s root freed, no VACUUM');
$gone->(q{h(leaf(30003))}, 'and its leaves');
ok($node->poll_query_until('postgres', 'SELECT marked() = 0'), 'its bucket unmarked');
$node->wait_for_log(qr/chunk freer freed 6 chunks of chunk index \d+, looking in (\d+) buckets marked/, $offset);
my ($looked) = slurp_file($node->logfile, $offset) =~
	/chunk freer freed 6 chunks of chunk index \d+, looking in (\d+) buckets marked/;
# (a bucket a chunk freed, at most: the root's, and those of the leaves
# its freeing let go of)
cmp_ok($looked, '<=', 6, "the freer looked in $looked buckets");
cmp_ok($node->safe_psql('postgres', q{SELECT test_chunk_index_buckets('pg_history.chunk_index')}), '>', 200,
	'in an index of more than 200 buckets');
is($node->safe_psql('postgres', q{SELECT test_cs_get('pages', h(leaf(12345))) = leaf(12345)}), 't',
	'the big tree\'s chunks there');

# two letting go of a child's last two references at once: each sees the
# other's left, and the one after marks it
$node->safe_psql('postgres', q{
	SELECT count(*) FROM generate_series(40000, 40002) i, LATERAL test_cs_put('pages', leaf(i));
	SELECT test_cs_put('pages', node(leaf(40000), leaf(40001)));
	SELECT test_cs_put('pages', node(leaf(40000), leaf(40002)));
});
my $a = $node->background_psql('postgres');
my $b = $node->background_psql('postgres');
$a->query_safe(q{BEGIN; SELECT test_cs_free('pages', ARRAY[h(node(leaf(40000), leaf(40001)))])});
$b->query_safe(q{BEGIN; SELECT test_cs_free('pages', ARRAY[h(node(leaf(40000), leaf(40002)))])});
$a->query_safe('COMMIT');
$b->query_safe('COMMIT');
$a->quit;
$b->quit;
$gone->(q{h(leaf(40000))}, 'the child both let go of freed, no VACUUM');

# marks survive a crash: a pointer dropped, no freer started, the server
# stopped at once
ok($node->poll_query_until('postgres', 'SELECT marked() = 0'), 'every bucket unmarked');
$node->safe_psql('postgres', q{
	SELECT count(*) FROM generate_series(50001, 50003) i, LATERAL test_cs_put('pages', leaf(i));
	SELECT test_cs_put('pages', tree(50001, 50003));
	INSERT INTO pg_history.pointers VALUES ('crash', h(tree(50001, 50003)));
});
$node->safe_psql('postgres', q{SET chunk_free_in_background = off;
	DELETE FROM pg_history.pointers WHERE name = 'crash'});
is($node->safe_psql('postgres', 'SELECT marked()'), '1', 'a bucket marked, no freer started');
$node->stop('immediate');
$node->start;
is($node->safe_psql('postgres', 'SELECT marked()'), '1', 'marked still after the crash');
# (a freer started by the next commit that marks frees it too)
$node->safe_psql('postgres', q{
	SELECT test_cs_put('pages', leaf(50100));
	INSERT INTO pg_history.pointers VALUES ('kick', h(leaf(50100)));
	DELETE FROM pg_history.pointers WHERE name = 'kick';
});
$gone->(q{h(tree(50001, 50003))}, 'and freed by the next freer');

# marks follow their chunks as buckets split: 100 roots let go of, no
# freer started, then enough put to split many buckets
$node->safe_psql('postgres', q{
	SELECT count(*) FROM generate_series(60001, 60100) i, LATERAL test_cs_put('pages', leaf(i));
	INSERT INTO pg_history.pointers SELECT 's' || i, h(leaf(i)) FROM generate_series(60001, 60100) i;
});
$node->safe_psql('postgres', q{SET chunk_free_in_background = off;
	DELETE FROM pg_history.pointers WHERE name LIKE 's%'});
$node->safe_psql('postgres', q{SET chunk_free_in_background = off;
	SELECT count(*) FROM generate_series(70001, 110000) i, LATERAL test_cs_put('pages', leaf(i));
	INSERT INTO pg_history.pointers VALUES ('more', h(leaf(70001)));});
$node->safe_psql('postgres', q{
	SELECT test_cs_put('pages', leaf(60200));
	INSERT INTO pg_history.pointers VALUES ('kick2', h(leaf(60200)));
	DELETE FROM pg_history.pointers WHERE name = 'kick2';
});
ok($node->poll_query_until('postgres',
	q{SELECT count(*) = 0 FROM generate_series(60001, 60100) i WHERE test_cs_has('pages', h(leaf(i)))}),
	'every root let go of before the splits freed');
is($node->safe_psql('postgres', q{SELECT ok FROM test_cs_check('pages')}), 't', 'the audit clean');

$node->stop;
done_testing();
