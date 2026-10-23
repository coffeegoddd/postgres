# Copyright (c) 2026, PostgreSQL Global Development Group

# Large values in versioned tables: committed out of line, each long value
# in a value tree of its own, they come back from history through a crash
# and on a standby, and a few bytes inserted into one commit a few nodes.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('large_values');
$node->init(allows_streaming => 1);
# (the rows a reset writes, not pages kept as of its commit given back)
$node->append_conf('postgresql.conf', "autovacuum = off\nprolly.kept_commits = 0");
$node->start;
$node->backup('b');
my $standby = PostgreSQL::Test::Cluster->new('large_values_standby');
$standby->init_from_backup($node, 'b', has_streaming => 1);
$standby->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE EXTENSION test_prolly;
	CREATE SCHEMA lv VERSIONED;
	CREATE TABLE lv.big (id int PRIMARY KEY, t text, b bytea);
	INSERT INTO lv.big
	SELECT g, string_agg(md5(g || ':' || i), '' ORDER BY i),
		decode(repeat(to_hex(g * 17), 30000), 'hex')
	FROM generate_series(1, 5) g, generate_series(1, 2000 * g) i GROUP BY g;
	CREATE TABLE lv.keyless (t text);
	INSERT INTO lv.keyless SELECT repeat('k', 100000) FROM generate_series(1, 3);
});
my $digest = q{SELECT string_agg(id || ':' || length(t) || ':' || md5(t) || ':' || md5(b), ',' ORDER BY id) FROM lv.big};
my $before = sql($digest);
like($before, qr/^1:64000:/, 'rows with long values are stored');
my $c1 = sql(q{SELECT (dolt_commit('-Am', 'large'))[1]});

sql(q{UPDATE lv.big SET t = overlay(t PLACING 'XYZ' FROM 96000 FOR 0) WHERE id = 3});
my $after = sql($digest);
my $c2 = sql(q{SELECT (dolt_commit('-am', 'an insertion'))[1]});
cmp_ok(sql(qq{SELECT test_walk_new_count('$c1', '$c2')}), '<', 20,
	'three bytes inserted into a 192 KB value commit a few nodes');

# Through a crash: history is read back whole.
$node->stop('immediate');
$node->start;
sql(qq{SELECT dolt_reset('--hard', '$c1')});
is(sql($digest), $before, 'the first commit\'s values after a crash');
sql(q{DELETE FROM lv.keyless});
sql(qq{SELECT dolt_reset('--hard', '$c2')});
is(sql($digest), $after, 'and the second\'s');
is(sql(q{SELECT count(*), sum(length(t)) FROM lv.keyless}), '3|300000',
	'identical long rows of a keyless table');

# On a standby, history is read from the replayed chunk store.
$node->wait_for_replay_catchup($standby);
is( $standby->safe_psql('postgres',
		qq{SELECT diff_type || ':' || length((from_row).t) || ':' || length((to_row).t)
		   FROM dolt_diff('$c1', '$c2', NULL::lv.big)}),
	'modified:192000:192003',
	'a standby reads the values from history');

# A reset that keeps a row's long value as stored, its row changed by
# another transaction after the reset's fold read it, keeps nothing of
# that transaction's: it fails, to be retried.
SKIP:
{
	skip 'injection points not supported by this build', 2
	  if $ENV{enable_injection_points} ne 'yes';
	sql(q{
		CREATE EXTENSION injection_points;
		CREATE TABLE lv.kept (id int PRIMARY KEY, n int, body text);
		ALTER TABLE lv.kept ALTER COLUMN body SET STORAGE EXTERNAL;
		INSERT INTO lv.kept SELECT 1, 0, string_agg(md5(i::text), '') FROM generate_series(1, 2000) i;
	});
	my $k1 = sql(q{SELECT (dolt_commit('-Am', 'kept'))[1]});
	sql(q{UPDATE lv.kept SET n = 1});
	sql(q{SELECT dolt_commit('-am', 'counted')});
	my $s = $node->background_psql('postgres', on_error_stop => 0);
	$s->query_safe(q{SELECT injection_points_set_local();
		SELECT injection_points_attach('prolly-update-keeping', 'wait')});
	$s->query_until(qr/resetting/, qq{
		\\echo resetting
		SELECT dolt_reset('--hard', '$k1');
		\\echo reset
	});
	$node->poll_query_until('postgres', q{SELECT count(*) > 0 FROM pg_stat_activity
		WHERE wait_event = 'prolly-update-keeping'}) or die 'the reset did not wait';
	sql(q{UPDATE lv.kept SET body = 'concurrent'});
	sql(q{SELECT injection_points_wakeup('prolly-update-keeping')});
	$s->query_until(qr/reset/, '');
	like($s->{stderr}, qr/changed while the branch was moved/,
		'a row changed after the fold read it fails the reset');
	$s->quit;
	is(sql(q{SELECT n || ':' || body FROM lv.kept}), '1:concurrent',
		'and the row is as the other transaction left it');
}

$standby->stop;
$node->stop;
done_testing();
