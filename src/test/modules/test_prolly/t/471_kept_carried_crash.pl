# Copyright (c) 2026, PostgreSQL Global Development Group

# A restore gives a table storage made of a kept commit's roots, and the
# other kept commits' roots, which the page store keeps, are named as of
# that storage (carried).  After a crash past the restore's commit, both
# commits are still restorable.  A restore that fails part way through, or
# that a crash ends before its commit, leaves the old storage and every
# root as they were: the next reset restores from them.  Redo is checked page by page throughout
# (wal_consistency_checking).

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('kept_carried_crash');
$node->init;
$node->append_conf('postgresql.conf', qq{
wal_consistency_checking = all
autovacuum = off
prolly_fold_worker = off
checkpoint_timeout = 1h
});
$node->start;
sub sql { return $node->safe_psql('postgres', $_[0]); }
sub counted
{
	my ($name) = @_;
	return sql(qq{SELECT pg_stat_force_next_flush();
		SELECT coalesce((SELECT value FROM pg_stat_prolly WHERE name = '$name'), 0)});
}
my $digest = q{SELECT md5(string_agg(id || ':' || v || ':' || coalesce(md5(big), ''), ',' ORDER BY id)) FROM app.t};

sql(q{CREATE EXTENSION injection_points;
	CREATE SCHEMA app VERSIONED;
	CREATE TABLE app.t (id int PRIMARY KEY, v int NOT NULL, big text);
	ALTER TABLE app.t ALTER COLUMN big SET STORAGE EXTERNAL;
	CREATE INDEX t_v ON app.t (v);
	INSERT INTO app.t SELECT g, 0, CASE WHEN g <= 10 THEN repeat(md5(g::text), 200) END
	FROM generate_series(1, 3000) g});
my $a = sql(q{SELECT (dolt_commit('-Am', 'a'))[1]});
my $at_a = sql($digest);
sql(q{UPDATE app.t SET v = 1 WHERE id <= 300;
	UPDATE app.t SET big = repeat('b', 5000) WHERE id <= 3});
my $b = sql(q{SELECT (dolt_commit('-am', 'b'))[1]});
my $at_b = sql($digest);

# reset to a, which carries b's roots; a crash once it commits
sql(q{UPDATE app.t SET v = 2 WHERE id % 5 = 0});
my $restored = counted('relations_restored');
sql(qq{SELECT dolt_reset('--hard', '$a')});
cmp_ok(counted('relations_restored'), '>', $restored, 'the reset to a restored');
is(sql($digest), $at_a, 'and gave a\'s rows');
$node->stop('immediate');
$node->start;
is(sql($digest), $at_a, 'after a crash, a\'s rows');
$restored = counted('relations_restored');
my $applied = counted('apply_rows');
sql(qq{SELECT dolt_reset('--hard', '$b')});
cmp_ok(counted('relations_restored'), '>', $restored, 'b, carried, restored after the crash');
is(counted('apply_rows') - $applied, 0, 'no row written');
is(sql($digest), $at_b, 'b\'s rows');

# a restore that fails part way: it rolls back, a still kept as it was
sql(q{UPDATE app.t SET v = 3 WHERE id % 5 = 1});
my $dirty = sql($digest);
my ($ret, $out, $err) = $node->psql('postgres', qq{
	SELECT injection_points_set_local();
	SELECT injection_points_attach('bmap-root-shared', 'error');
	SELECT dolt_reset('--hard', '$a');
});
like($err, qr/bmap-root-shared/, 'the restore failed part way');
is(sql($digest), $dirty, 'the working set as it was');
$restored = counted('relations_restored');
sql(qq{SELECT dolt_reset('--hard', '$a')});
cmp_ok(counted('relations_restored'), '>', $restored, 'a restored after the failure');
is(sql($digest), $at_a, 'a\'s rows');

# a crash in the middle of a restore, before its commit
sql(q{UPDATE app.t SET v = 4 WHERE id % 5 = 2});
$dirty = sql($digest);
sql(q{SELECT injection_points_attach('bmap-root-shared', 'wait')});
my $bg = $node->background_psql('postgres', on_error_stop => 0);
$bg->query_until(qr/start/, qq{\\echo start
	SELECT dolt_reset('--hard', '$b');
});
$node->wait_for_event('client backend', 'bmap-root-shared');
$node->stop('immediate');
# (its backend gone with the crash: ended without writing to it, as a
# write to the closed pipe would kill the test)
eval { $bg->{run}->kill_kill; };
$node->start;
is(sql($digest), $dirty, 'after the crash, the working set as it was');
$restored = counted('relations_restored');
$applied = counted('apply_rows');
sql(qq{SELECT dolt_reset('--hard', '$b')});
cmp_ok(counted('relations_restored'), '>', $restored, 'b restored from the old storage');
is(counted('apply_rows') - $applied, 0, 'no row written');
is(sql($digest), $at_b, 'b\'s rows');
$restored = counted('relations_restored');
sql(qq{SELECT dolt_reset('--hard', '$a')});
cmp_ok(counted('relations_restored'), '>', $restored, 'and a, carried again');
is(sql($digest), $at_a, 'a\'s rows');
is(sql(q{SELECT count(*) FROM app.t WHERE v = 0}), '3000', 'found by the index');

$node->stop;
done_testing();
