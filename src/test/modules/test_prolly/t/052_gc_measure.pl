# Copyright (c) 2026, PostgreSQL Global Development Group

# Reclamation measured: the space branches take and give back after they
# are deleted and the store collected (default, then full); how long
# default and full collections take; and how long a collection holds
# appends off (its window) under a commit load.  Small by default, as a
# test; PROLLY_GC_MEASURE_SCALE multiplies the rows and changes, and
# PROLLY_GC_MEASURE_BRANCHES sets the branches, for a measurement whose
# numbers (printed as notes) go in the spec.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(time);

my $scale = $ENV{PROLLY_GC_MEASURE_SCALE} // 1;
my $nbranches = $ENV{PROLLY_GC_MEASURE_BRANCHES} // 4;
my $rows = 20000 * $scale;
my $changes = 1000 * $scale;

my $node = PostgreSQL::Test::Cluster->new('measure');
$node->init;
$node->append_conf('postgresql.conf', qq{
autovacuum = off
shared_buffers = 128MB
prolly.gc_journal_threshold = -1
});
$node->start;
$node->safe_psql('postgres', 'CREATE DATABASE m');
$node->safe_psql('m', 'CREATE EXTENSION test_prolly');

sub sql { return $node->safe_psql('m', $_[0]); }
sub timed { my $t = time; my $r = sql($_[0]); return (time - $t, $r); }

# the chunk store's files, in bytes
my $space = q{SELECT sum((pg_stat_file(d || '/' || f)).size)
	FROM (SELECT 'base/' || oid AS d FROM pg_database WHERE datname = 'm') db,
		pg_ls_dir(d) f
	WHERE f LIKE 'prolly\_journal%' OR f LIKE 'prolly\_table\_%'};
sub mb { return sprintf('%.1f MB', $_[0] / 1048576); }

sql(qq{CREATE SCHEMA app VERSIONED;
	CREATE TABLE app.t (id int PRIMARY KEY, v text);
	INSERT INTO app.t SELECT g, md5(g::text) FROM generate_series(1, $rows) g;
	SELECT dolt_commit('-m', 'base')});
my ($t_first) = timed('SELECT test_chunk_gc(false)');
my $base = sql($space);

for my $b (1 .. $nbranches)
{
	my $lo = ($b - 1) * $changes % $rows;
	sql(qq{SELECT dolt_branch('b$b'); SELECT dolt_checkout('b$b');
		UPDATE app.t SET v = md5(v) WHERE id > $lo AND id <= $lo + $changes;
		SELECT dolt_commit('-m', 'on b$b'); SELECT dolt_checkout('main')});
}
my $with = sql($space);
my ($t_default, $r_default) = timed('SELECT kept || \' \' || freed FROM test_chunk_gc(false)');
my $settled = sql($space);
sql(join(' ', map { "SELECT dolt_branch('-D', 'b$_');" } 1 .. $nbranches));
my ($t_after) = timed('SELECT test_chunk_gc(false)');
my $after_default = sql($space);
my ($t_full, $r_full) = timed('SELECT kept || \' \' || freed FROM test_chunk_gc(true)');
my $after_full = sql($space);
my ($t_full_live) = timed('SELECT test_chunk_gc(true)');

note("rows $rows, branches $nbranches, $changes rows changed on each");
note('space: base ' . mb($base) . ', with branches ' . mb($with)
	. ', collected ' . mb($settled) . ', branches deleted and collected '
	. mb($after_default) . ', then fully ' . mb($after_full));
note(sprintf('default collection of the base: %.0f ms; of the branches\' changes: '
	. '%.0f ms (kept freed: %s); after the deletes: %.0f ms',
	$t_first * 1000, $t_default * 1000, $r_default, $t_after * 1000));
note(sprintf('full collection after the deletes: %.0f ms (kept freed: %s); '
	. 'again, all live: %.0f ms', $t_full * 1000, $r_full, $t_full_live * 1000));
cmp_ok($after_default, '>=', $after_full, 'a default collection keeps what settled');
cmp_ok($after_full, '<', $with, 'a full one gives the deleted branches\' space back');

# The window under a commit load: sessions committing on branches of their
# own while collections run.
my @writers;
for my $w (1 .. 4)
{
	sql(qq{SELECT dolt_branch('w$w')});
	my $s = $node->background_psql('m');
	$s->query_safe(qq{SELECT dolt_checkout('w$w')});
	$s->query_until(qr/started/, qq{
		\\echo started
		DO \$\$
		BEGIN
			FOR i IN 1 .. 1000000 LOOP
				UPDATE app.t SET v = md5(v || i) WHERE id = $w * 100 + i % 50;
				PERFORM dolt_commit('-m', 'load ' || i);
				COMMIT;
			END LOOP;
		END \$\$;
	});
	push @writers, $s;
}
my @windows;
my @totals;
for my $i (1 .. 10)
{
	my ($t, $win) = timed('SELECT window_usec FROM test_chunk_gc(false)');
	push @windows, $win / 1000;
	push @totals, $t * 1000;
}
my $commits = sql(q{SELECT count(*) FROM dolt_log('w1')});
$node->stop('immediate');
eval { $_->quit; } for @writers;
@windows = sort { $a <=> $b } @windows;
@totals = sort { $a <=> $b } @totals;
note(sprintf('under 4 committing sessions: window median %.2f ms, max %.2f ms; '
	. 'collection median %.0f ms, max %.0f ms; %d commits on one writer\'s branch',
	$windows[5], $windows[-1], $totals[5], $totals[-1], $commits));
cmp_ok($windows[-1], '<', $totals[-1], 'the window is a part of the collection');

done_testing();
