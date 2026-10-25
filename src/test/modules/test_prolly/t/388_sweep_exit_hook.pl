# Copyright (c) 2026, PostgreSQL Global Development Group

# A page sweep shows each relation it holds in its backend's slot, which
# the backend clears as it exits (a before_shmem_exit callback).  In a
# fresh backend, the first hold comes inside the sweep's own error
# cleanup (PG_ENSURE_ERROR_CLEANUP), which must stay the latest exit
# callback when it ends: so the slot's callback is registered before it.
# Each sweep here holds a relation to lower its shared_below there, in a
# backend that registered nothing before, and once more after a reset.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('sweep_exit_hook');
$node->init;
$node->append_conf('postgresql.conf', q{
debug_block_mapped_heap = on
autovacuum = off
prolly_fold_worker = off
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE EXTENSION injection_points;
	SELECT injection_points_attach('bmap-shared-below-held', 'notice');
	CREATE SCHEMA kp VERSIONED;
	CREATE TABLE kp.t (a int, b text) WITH (fillfactor = 10);
	INSERT INTO kp.t SELECT g, repeat('t', 300) FROM generate_series(1, 2000) g;
	SELECT dolt_commit('-Am', 'load');
	SELECT dolt_branch('other');
});
sql(q{UPDATE kp.t SET b = 'u' WHERE a < 100});
sql(q{CREATE TABLE x (a int, b text);
	INSERT INTO x SELECT g, repeat('x', 300) FROM generate_series(1, 5000) g;
	DROP TABLE x; CHECKPOINT});

for my $i (1 .. 3)
{
	my ($ret, $out, $err) = $node->psql('postgres',
		q{SELECT freed, recounted, truncated, moved FROM prolly_page_sweep()});
	is($ret, 0, "a fresh backend's sweep $i succeeds") or diag($err);
	unlike($err, qr/before_shmem_exit/, "with no exit callback error ($i)");
	like($err, qr/bmap-shared-below-held/,
		"having held a relation to lower its shared_below ($i)");
}
my ($ret, $out, $err) = $node->psql('postgres',
	q{SELECT dolt_reset('--hard'); SELECT freed FROM prolly_page_sweep(); SELECT 1});
is($ret, 0, 'a reset, then a sweep, in one fresh backend') or diag($err);
unlike(slurp_file($node->logfile), qr/before_shmem_exit callback/,
	'the log has no exit callback error');

$node->stop;
done_testing();
