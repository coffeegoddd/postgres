# Copyright (c) 2026, PostgreSQL Global Development Group

# What a VACUUM lets go of from the release queue first is a saving, not
# its work: one VACUUM command, over all the tables it vacuums, lets go of
# no more than prolly.release_max_per_pass entries (and the step that
# passes it), leaving the rest to the background drain, and a release step
# that fails is reported as a warning, the VACUUM going on and the queue as
# it was.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('vacuum_release_bounds');
$node->init;
$node->append_conf(
	'postgresql.conf', q{
debug_block_mapped_heap = on
autovacuum = off
prolly.release_max_per_pass = 50
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{CREATE EXTENSION test_prolly; CREATE EXTENSION injection_points;
	CREATE TABLE small (a int) WITH (autovacuum_enabled = off);
	INSERT INTO small VALUES (1), (2), (3);
	DELETE FROM small WHERE a = 2;
	CREATE TABLE small2 (a int) WITH (autovacuum_enabled = off);
	INSERT INTO small2 VALUES (1);
	CREATE TABLE big (a int, b int8) WITH (fillfactor = 10);
	INSERT INTO big SELECT g, g FROM generate_series(1, 20000) g});
sql('DROP TABLE big');
my $pending = sql(q{SELECT test_bmap_release_pending()});
cmp_ok($pending, '>', 0, 'a dropped table is queued');

# a step that fails: VACUUM warns, vacuums, and leaves the queue
my ($ret, $out, $err) = $node->psql('postgres', q{
	SELECT injection_points_set_local();
	SELECT injection_points_attach('bmap-release-step', 'error');
	VACUUM small;
	SELECT n_dead_tup FROM pg_stat_user_tables WHERE relname = 'small';
});
is($ret, 0, 'a VACUUM whose release step fails succeeds');
like($err, qr/WARNING:  could not let go of dropped storage before vacuuming "small"/,
	'and warns of it');
is(sql(q{SELECT count(*) FROM small}), '2', 'the table reads as it did');
is(sql(q{SELECT test_bmap_release_pending()}), $pending, 'the queue is as it was');

# one VACUUM of two tables lets go of no more than the bound
my $drained = $node->safe_psql('postgres', q{
	SELECT pg_stat_reset_shared('prolly');
	VACUUM small, small2;
	SELECT pg_stat_force_next_flush();
	SELECT value FROM pg_stat_prolly WHERE name = 'bmap_release_drained';
});
$drained = (split /\n/, $drained)[-1];
cmp_ok($drained, '>', 0, 'a VACUUM lets go of some of the queue');
cmp_ok($drained, '<=', 50 + 128,
	'over both tables, no more than the bound and a step past it');
cmp_ok(sql(q{SELECT test_bmap_release_pending()}), '>', 0,
	'leaving the rest to the background drain');
sql(q{SELECT * FROM test_bmap_release_drain()});
is(sql(q{SELECT test_bmap_release_pending()}), '0', 'the drain lets go of the rest');

$node->stop;
done_testing();
