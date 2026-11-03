# Copyright (c) 2026, PostgreSQL Global Development Group

# A chunk log's lanes: the number of a lane whose making aborted given
# again once a VACUUM pass of the log's index has ended (not burnt); a lane
# made in the log's tablespace, whatever the session's default; a log with
# lanes, or a lane, not truncated.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('lane_numbers');
$node->init;
$node->append_conf('postgresql.conf', "autovacuum = off\nmax_prepared_transactions = 2");
$node->start;
my $ts_dir = PostgreSQL::Test::Utils::tempdir_short();
$node->safe_psql('postgres', qq{
	CREATE EXTENSION test_prolly;
	CREATE TABLESPACE ts LOCATION '$ts_dir';
	CREATE FUNCTION d(i int8) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS \$\$SELECT convert_to('lane number chunk ' || i, 'UTF8')\$\$;
	CREATE FUNCTION h(b bytea) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS 'SELECT substr(sha256(b), 1, 20)';
});
my $lanes = q{SELECT string_agg(relname, ' ' ORDER BY relname) FROM pg_class
	WHERE relnamespace = 'pg_history'::regnamespace AND relname LIKE 'chunks_lane_%'};

# (each a backend holding the lane it put into)
my @held;
sub put_held
{
	my ($i, $pre) = @_;
	my $s = $node->background_psql('postgres');
	$s->query_safe($pre) if $pre;
	$s->query_safe("SELECT (test_cs_put('pages', d($i))).stored");
	push @held, $s;
}

put_held(1);
put_held(2, 'SET default_tablespace = ts');
is($node->safe_psql('postgres', $lanes), 'chunks_lane_1', 'the second writer made lane 1');
is( $node->safe_psql('postgres',
		q{SELECT reltablespace FROM pg_class WHERE oid = 'pg_history.chunks_lane_1'::regclass}),
	'0', "lane 1 is in the database's tablespace, as the log is, not the session's default");

# a making aborted: lane 2 a hole, not given before VACUUM has passed
my $c = $node->background_psql('postgres');
$c->query_safe("BEGIN; SELECT (test_cs_put('pages', d(3))).stored; ROLLBACK");
$c->quit;
is($node->safe_psql('postgres', $lanes), 'chunks_lane_1', 'the aborted making left no lane 2');
put_held(4);
is($node->safe_psql('postgres', $lanes), 'chunks_lane_1 chunks_lane_3',
	'before VACUUM, the next making took a new number');

is( $node->safe_psql('postgres',
		q{SELECT string_agg((test_cs_get('pages', h(d(i))) IS NOT DISTINCT FROM d(i))::text, ' ' ORDER BY i)
		  FROM generate_series(1, 4) i}),
	'true true false true', 'the chunks put read back, the aborted one\'s not');

# VACUUM, taking out the entries the aborted making left: lane 2 given again
$node->safe_psql('postgres', 'VACUUM pg_history.chunk_index');
put_held(5);
is($node->safe_psql('postgres', $lanes), 'chunks_lane_1 chunks_lane_2 chunks_lane_3',
	'after VACUUM, the hole\'s number was given again');
is( $node->safe_psql('postgres',
		q{SELECT string_agg((test_cs_get('pages', h(d(i))) IS NOT DISTINCT FROM d(i))::text, ' ' ORDER BY i)
		  FROM generate_series(5, 5) i}),
	'true', 'the chunk put into the lane given again reads back');
is($node->safe_psql('postgres', q{SELECT ok FROM test_cs_check('pages')}),
	't', "the store's audit passes");
$node->safe_psql('postgres', 'VACUUM pg_history.chunk_index');
is($node->safe_psql('postgres', q{SELECT ok FROM test_cs_check('pages')}),
	't', "the audit passes after VACUUM: nothing the aborted making left counted in the reused lane");

# a making prepared: its number not a hole while the transaction is
# prepared (another making aborted, VACUUM passing, the next making takes
# neither number); the backend that prepared it does not wait on it
my $p = $node->background_psql('postgres');
$p->query_safe("BEGIN; SELECT (test_cs_put('pages', d(6))).stored; PREPARE TRANSACTION 'made'");
$p->query_safe("SET statement_timeout = '5s'; SELECT (test_cs_put('pages', d(7))).stored");
is($node->safe_psql('postgres', q{SELECT test_cs_get('pages', h(d(7))) = d(7)}),
	't', 'the backend that prepared the making put again without waiting on it');
push @held, $p;
$c = $node->background_psql('postgres');
$c->query_safe("BEGIN; SELECT (test_cs_put('pages', d(8))).stored; ROLLBACK");
$c->quit;
$node->safe_psql('postgres', 'VACUUM pg_history.chunk_index');
put_held(9);
is($node->safe_psql('postgres', $lanes),
	'chunks_lane_1 chunks_lane_2 chunks_lane_3 chunks_lane_5 chunks_lane_7',
	'the prepared making\'s number (4) not given again, nor the hole seen once (6)');
$node->safe_psql('postgres', "COMMIT PREPARED 'made'");
is($node->safe_psql('postgres', q{SELECT reltablespace FROM pg_class WHERE oid = 'pg_history.chunks_lane_4'::regclass}),
	'0', 'committed, the prepared making\'s lane is there');
is( $node->safe_psql('postgres',
		q{SELECT string_agg((test_cs_get('pages', h(d(i))) IS NOT DISTINCT FROM d(i))::text, ' ' ORDER BY i)
		  FROM unnest(ARRAY[6, 9]) i}),
	'true true', 'the chunks put in lanes 4 and 7 read back');
$node->safe_psql('postgres', 'VACUUM pg_history.chunk_index');
is($node->safe_psql('postgres', q{SELECT ok FROM test_cs_check('pages')}),
	't', "the audit passes after VACUUM: no lane's live chunk counted dead");

# a making prepared and rolled back: its number held while prepared (a
# making meanwhile takes a new one), a hole once rolled back, given again
# after a VACUUM pass; nothing of the reused lane counted dead
put_held(11);
my $q = $node->background_psql('postgres');
$q->query_safe("BEGIN; SELECT (test_cs_put('pages', d(12))).stored; PREPARE TRANSACTION 'gone'");
$q->quit;
put_held(13);
$node->safe_psql('postgres', 'VACUUM pg_history.chunk_index');
put_held(14);
is($node->safe_psql('postgres', $lanes),
	'chunks_lane_1 chunks_lane_2 chunks_lane_3 chunks_lane_4 chunks_lane_5 chunks_lane_7 chunks_lane_8 chunks_lane_9',
	'while prepared, its number (6) not given, a VACUUM pass between makings (8 and 9 new)');
$node->safe_psql('postgres', "ROLLBACK PREPARED 'gone'");
put_held(16);
$node->safe_psql('postgres', 'VACUUM pg_history.chunk_index');
put_held(15);
is($node->safe_psql('postgres', $lanes),
	'chunks_lane_1 chunks_lane_10 chunks_lane_2 chunks_lane_3 chunks_lane_4 chunks_lane_5 chunks_lane_6 chunks_lane_7 chunks_lane_8 chunks_lane_9',
	'rolled back, its number (6) a hole, given again after a VACUUM pass (10 new before it)');
is($node->safe_psql('postgres', q{SELECT test_cs_get('pages', h(d(15))) = d(15)}),
	't', 'the chunk put in the reused lane reads back');
$node->safe_psql('postgres', 'VACUUM pg_history.chunk_index');
is($node->safe_psql('postgres', q{SELECT ok FROM test_cs_check('pages')}),
	't', "the audit passes after VACUUM: the rolled-back making's entries out, nothing of the reused lane counted dead");

# a log truncated truncates its lanes, each keeping its number, and the
# log the numbers it has given (a lane made after takes the next); a lane
# alone is not truncated; a truncation rolled back keeps them all
$node->safe_psql('postgres', 'CREATE TABLE tl () USING prolly_chunks');
my $a = $node->background_psql('postgres');
$a->query_safe(q{SELECT test_chunk_log_append('tl', '\x01')});
my $b = $node->background_psql('postgres');
$b->query_safe(q{SELECT test_chunk_log_append('tl', convert_to(repeat('x', 30000), 'UTF8'))});
is($node->safe_psql('postgres', q{SELECT count(*) FROM pg_class WHERE relname = 'tl_lane_1'}),
	'1', 'tl has a lane, the second writer\'s');
my $lane_size = q{SELECT pg_relation_size('tl_lane_1') / current_setting('block_size')::int};
cmp_ok($node->safe_psql('postgres', $lane_size), '>', 1, 'its lane has pages of chunks');
$node->safe_psql('postgres', 'BEGIN; TRUNCATE tl; ROLLBACK');
cmp_ok($node->safe_psql('postgres', $lane_size), '>', 1, 'a truncation rolled back keeps the lane\'s pages');
my ($ret, $out, $err) = $node->psql('postgres', 'TRUNCATE tl_lane_1');
like($err, qr/cannot truncate "tl_lane_1", a lane of a chunk log/, 'a lane alone is not truncated');
my $node_before = $node->safe_psql('postgres', q{SELECT relfilenode FROM pg_class WHERE relname = 'tl_lane_1'});
$node->safe_psql('postgres', 'TRUNCATE tl');
is($node->safe_psql('postgres', $lane_size), '1', 'the log truncated, its lane is its meta page alone');
isnt($node->safe_psql('postgres', q{SELECT relfilenode FROM pg_class WHERE relname = 'tl_lane_1'}),
	$node_before, '... in storage of its own');
my $loc = $b->query_safe(q{SELECT test_chunk_log_append('tl', '\x0b')});
is($node->safe_psql('postgres', qq{SELECT test_chunk_log_read('tl', $loc, 1)}), '\x0b',
	'the lane\'s writer appends to it again, read back');
$node->safe_psql('postgres', q{SELECT test_chunk_log_append('tl', '\x02')});
is($node->safe_psql('postgres', q{SELECT string_agg(relname, ' ' ORDER BY relname) FROM pg_class WHERE relname LIKE 'tl_lane_%'}),
	'tl_lane_1 tl_lane_2', 'a lane made after the truncation takes the next number, not one its lanes have');
$b->quit;
$a->quit;
$_->quit for @held;
@held = ();
$node->stop('immediate');
$node->start;
is($node->safe_psql('postgres', qq{SELECT test_chunk_log_read('tl', $loc, 1)}), '\x0b',
	'after crash recovery, the truncated lane\'s chunk reads back');
$node->safe_psql('postgres', q{BEGIN; CREATE TABLE t2 () USING prolly_chunks; TRUNCATE t2; COMMIT; TRUNCATE t2});
pass('a log without lanes truncates');
$node->safe_psql('postgres', 'DROP TABLE tl');
is($node->safe_psql('postgres', q{SELECT count(*) FROM pg_class WHERE relname LIKE 'tl_lane_%'}),
	'0', 'the lane goes with its log');

$_->quit for @held;
$node->stop;
done_testing();
