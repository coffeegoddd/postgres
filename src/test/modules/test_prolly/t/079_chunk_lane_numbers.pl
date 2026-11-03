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
$node->append_conf('postgresql.conf', 'autovacuum = off');
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

# a log with lanes, and a lane, not truncated
$node->safe_psql('postgres', 'CREATE TABLE tl () USING prolly_chunks');
my $a = $node->background_psql('postgres');
$a->query_safe(q{SELECT test_chunk_log_append('tl', '\x01')});
$node->safe_psql('postgres', q{SELECT test_chunk_log_append('tl', '\x02')});
is($node->safe_psql('postgres', q{SELECT count(*) FROM pg_class WHERE relname = 'tl_lane_1'}),
	'1', 'tl has a lane');
my ($ret, $out, $err) = $node->psql('postgres', 'TRUNCATE tl');
like($err, qr/cannot truncate chunk log "tl", which has lanes/, 'a log with lanes is not truncated');
($ret, $out, $err) = $node->psql('postgres', 'TRUNCATE tl_lane_1');
like($err, qr/cannot truncate "tl_lane_1", a lane of a chunk log/, 'a lane is not truncated');
$node->safe_psql('postgres', q{BEGIN; CREATE TABLE t2 () USING prolly_chunks; TRUNCATE t2; COMMIT; TRUNCATE t2});
pass('a log without lanes truncates');
$node->safe_psql('postgres', 'DROP TABLE tl');
is($node->safe_psql('postgres', q{SELECT count(*) FROM pg_class WHERE relname LIKE 'tl_lane_%'}),
	'0', 'the lane goes with its log');

$a->quit;
$_->quit for @held;
$node->stop;
done_testing();
