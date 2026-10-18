# Copyright (c) 2026, PostgreSQL Global Development Group

# A versioned table may hold OID pointers (values in its TOAST relation,
# from before values were stored by content) beside hash pointers.  An
# UPDATE of another column keeps either.  A rewrite converts every OID
# pointer: VACUUM FULL, CLUSTER, REPACK, REPACK (CONCURRENTLY) with
# concurrent changes, and a rewriting ALTER TABLE.  The table keeps its
# TOAST relation, emptied.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('rewrites');
$node->init(allows_streaming => 'logical');
$node->append_conf('postgresql.conf', qq{autovacuum = off
prolly_value_memo_entries = 100});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql('CREATE EXTENSION test_prolly');
sql('CREATE SCHEMA w VERSIONED');
sql('CREATE TABLE w.docs (id int PRIMARY KEY, n int, t text, b bytea)');

my $value = q{(SELECT string_agg(md5(%s || '/' || i), '' ORDER BY i)
	FROM generate_series(1, 3000) i)};
my $digest = q{SELECT md5(string_agg(id || ':' || n || ':' || md5(t) || ':' || md5(b), ','
	ORDER BY id)) FROM w.docs};
my $forms = q{SELECT count(*) FILTER (WHERE pg_column_toast_chunk_id(t) IS NOT NULL)
		|| ' oid, ' || count(*) FILTER (WHERE test_toast_pointer(t) IS NOT NULL) || ' hash'
	FROM w.docs};

# Rows from before (their values in the TOAST relation), and new ones.
sub old_rows
{
	my ($from, $to) = @_;
	sql(sprintf(q{SET debug_toast_relation = on;
		INSERT INTO w.docs SELECT g, 0, %s, convert_to(%s, 'UTF8')
		FROM generate_series(%d, %d) g}, sprintf($value, 'g'), sprintf($value, 'g || \'b\''),
		$from, $to));
}
old_rows(1, 3);
sql(sprintf(q{INSERT INTO w.docs SELECT g, 0, %s, convert_to(%s, 'UTF8')
	FROM generate_series(4, 6) g}, sprintf($value, 'g'), sprintf($value, 'g || \'b\'')));
is(sql($forms), '3 oid, 3 hash', 'OID and hash pointers in one table');

# The OID pointers' values are remembered by the memo, as large as the
# server's setting: a commit after an UPDATE of another column finds them.
is(sql('SHOW prolly_value_memo_entries'), '100', 'the memo\'s size is a setting');
sql(q{SELECT length(dolt_commit('-Am', 'both')::text)});
sql('UPDATE w.docs SET n = n + 1');
sql(q{SELECT count(*) FROM test_memo_counters(true)});
is($node->safe_psql('postgres', q{SELECT length(dolt_commit('-am', 'counted')::text) > 0;
	SELECT hits FROM test_memo_counters(true)}), "t\n6",
	'and a commit finds the OID pointers\' values in it');
my $before = sql($digest);
sql('UPDATE w.docs SET n = n + 1');
is(sql($forms), '3 oid, 3 hash', 'an UPDATE of another column keeps both');
$before = sql($digest);

sub converted
{
	my ($what) = @_;
	is(sql($forms), '0 oid, 6 hash', "$what converts every OID pointer");
	is(sql($digest), $before, "$what keeps the values");
	is(sql(q{SELECT reltoastrelid <> 0 AND pg_relation_size(reltoastrelid) = 0
		FROM pg_class WHERE oid = 'w.docs'::regclass}), 't',
		"$what leaves the TOAST relation, empty");
}

sql('VACUUM FULL w.docs');
converted('VACUUM FULL');

sql('DELETE FROM w.docs WHERE id <= 3');
old_rows(1, 3);
sql('UPDATE w.docs SET n = n + 1 WHERE id <= 3');
$before = sql($digest);
sql('CLUSTER w.docs USING docs_pkey');
converted('CLUSTER');

sql('DELETE FROM w.docs WHERE id <= 3');
old_rows(1, 3);
sql('UPDATE w.docs SET n = n + 1 WHERE id <= 3');
$before = sql($digest);
sql('REPACK w.docs');
converted('REPACK');

sql('DELETE FROM w.docs WHERE id <= 3');
old_rows(1, 3);
sql('UPDATE w.docs SET n = n + 1 WHERE id <= 3');
$before = sql($digest);
sql('ALTER TABLE w.docs ALTER COLUMN n TYPE bigint');
converted('a rewriting ALTER TABLE');

# REPACK (CONCURRENTLY), with changes made while it runs: a hash pointer
# kept by an UPDATE of another column, a new one, and an OID pointer.
SKIP:
{
	skip 'injection points not supported by this build', 3
	  if $ENV{enable_injection_points} ne 'yes';
	sql('CREATE EXTENSION injection_points');
	sql('DELETE FROM w.docs WHERE id <= 3');
	old_rows(1, 3);
	my $r = $node->background_psql('postgres');
	$r->query_safe(q{SELECT injection_points_set_local();
		SELECT injection_points_attach('repack-concurrently-before-lock', 'wait')});
	$r->query_until(qr/repacking/, q{
		\echo repacking
		REPACK (CONCURRENTLY) w.docs;
		\echo repacked
	});
	$node->poll_query_until('postgres', q{SELECT count(*) > 0 FROM pg_stat_activity
		WHERE wait_event = 'repack-concurrently-before-lock'})
	  or die 'REPACK did not stop';
	sql('UPDATE w.docs SET n = n + 1');
	sql(sprintf(q{UPDATE w.docs SET t = %s WHERE id = 5}, sprintf($value, '50')));
	$before = sql($digest);
	sql(q{SELECT injection_points_wakeup('repack-concurrently-before-lock')});
	$r->query_until(qr/repacked/, '');
	$r->quit;
	converted('REPACK (CONCURRENTLY)');
}

$node->stop;
done_testing();
