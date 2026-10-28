# Copyright (c) 2026, PostgreSQL Global Development Group

# A restore's new storage carries the lineage of the storage it replaces
# on, so the branch's other kept roots of the table are of it with
# nothing renamed.  That lineage is logged with the map it makes: after a
# crash past the restore's commit, both commits still restore.  A restore
# that fails part way, or that a crash ends before its commit, leaves
# only orphaned storage, which nothing names: the old storage and its
# roots restore as before.  A move that restores another branch's kept
# commit carries the lineage on too, and holds the shared pages by roots
# of the branch's own.  Storage of another lineage (a truncation's) makes
# the branch's kept roots of the table gone, never restorable.  Redo is checked page by page
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

my $node = PostgreSQL::Test::Cluster->new('restore_lineage');
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
my $digest = q{SELECT md5(string_agg(id || ':' || v, ',' ORDER BY id)) FROM app.t};

# a reset to $to: did it restore, and how many rows did it write?
sub reset_to
{
	my ($to) = @_;
	my $restored = counted('relations_restored');
	my $applied = counted('apply_rows');
	sql(qq{SELECT dolt_reset('--hard', '$to')});
	return (counted('relations_restored') - $restored,
		counted('apply_rows') - $applied);
}

sql(q{ALTER DATABASE postgres SET prolly.history_window = '1000 years'});
sql(q{CREATE EXTENSION injection_points;
	CREATE SCHEMA app VERSIONED;
	CREATE TABLE app.t (id int PRIMARY KEY, v int NOT NULL);
	CREATE INDEX t_v ON app.t (v);
	INSERT INTO app.t SELECT g, 0 FROM generate_series(1, 3000) g});
my $a = sql(q{SELECT (dolt_commit('-Am', 'a'))[1]});
my $at_a = sql($digest);
sql(q{UPDATE app.t SET v = 1 WHERE id <= 300});
my $b = sql(q{SELECT (dolt_commit('-am', 'b'))[1]});
my $at_b = sql($digest);
my ($restored, $applied);

# a restore that fails part way, its new storage made (of the old one's
# lineage) and orphaned: the old storage's roots restore as before
sql(q{UPDATE app.t SET v = 2 WHERE id % 5 = 0});
my $dirty = sql($digest);
my ($ret, $out, $err) = $node->psql('postgres', qq{
	SELECT injection_points_set_local();
	SELECT injection_points_attach('bmap-root-shared', 'error');
	SELECT dolt_reset('--hard', '$a');
});
like($err, qr/bmap-root-shared/, 'the restore failed part way');
is(sql($digest), $dirty, 'the working set as it was');
($restored, $applied) = reset_to($a);
cmp_ok($restored, '>', 0, 'a restored after the failure');
is($applied, 0, 'no row written');
is(sql($digest), $at_a, 'a\'s rows');

# a crash past a restore's commit: the lineage was logged with the map
($restored, $applied) = reset_to($b);
cmp_ok($restored, '>', 0, 'b restored');
$node->stop('immediate');
$node->start;
is(sql($digest), $at_b, 'after a crash, b\'s rows');
($restored, $applied) = reset_to($a);
cmp_ok($restored, '>', 0, 'a restored after the crash');
is($applied, 0, 'no row written');
($restored, $applied) = reset_to($b);
cmp_ok($restored, '>', 0, 'and b');
is($applied, 0, 'no row written');

# a crash in the middle of a restore, before its commit
sql(q{UPDATE app.t SET v = 3 WHERE id % 5 = 1});
$dirty = sql($digest);
sql(q{SELECT injection_points_attach('bmap-root-shared', 'wait')});
my $bg = $node->background_psql('postgres', on_error_stop => 0);
$bg->query_until(qr/start/, qq{\\echo start
	SELECT dolt_reset('--hard', '$a');
});
$node->wait_for_event('client backend', 'bmap-root-shared');
$node->stop('immediate');
eval { $bg->{run}->kill_kill; };
$node->start;
is(sql($digest), $dirty, 'after the crash, the working set as it was');
($restored, $applied) = reset_to($a);
cmp_ok($restored, '>', 0, 'a restored from the old storage');
is($applied, 0, 'no row written');
is(sql($digest), $at_a, 'a\'s rows');
($restored, $applied) = reset_to($b);
cmp_ok($restored, '>', 0, 'and b');
is(sql($digest), $at_b, 'b\'s rows');

# another branch's kept commit restored: its new storage carries the
# session storage's lineage on, so the branch's own kept roots of the
# table stay restorable, and those it holds of the shared pages are its
# own
sql(q{SELECT dolt_branch('s')});
my $s = sub {
	$node->safe_psql('postgres',
		"DO \$\$ BEGIN PERFORM dolt_checkout('s'); END \$\$;\n" . $_[0]);
};
$s->(q{UPDATE app.t SET v = 5 WHERE id <= 100});
my $s1 = $s->(q{SELECT (dolt_commit('-am', 's1'))[1]});
my $at_s1 = $s->($digest);
$s->(q{UPDATE app.t SET v = 6 WHERE id <= 200});
my $shared = counted('move_tables_shared');
$s->(qq{SELECT dolt_reset('--hard', '$a')});
cmp_ok(counted('move_tables_shared'), '>', $shared,
	's given main\'s kept pages of a');
is($s->($digest), $at_a, 'a\'s rows on s');
$s->(q{UPDATE app.t SET v = 7 WHERE id <= 200});
$restored = counted('relations_restored');
$applied = counted('apply_rows');
$s->(qq{SELECT dolt_reset('--hard', '$a')});
cmp_ok(counted('relations_restored') - $restored, '>', 0,
	'a restored on s again from the roots it holds of the shared pages');
is(counted('apply_rows') - $applied, 0, 'no row written');
is($s->($digest), $at_a, 'a\'s rows on s');
$restored = counted('relations_restored');
$applied = counted('apply_rows');
$s->(qq{SELECT dolt_reset('--hard', '$s1')});
cmp_ok(counted('relations_restored') - $restored, '>', 0,
	's1 restored from its own root, of the lineage carried on');
is(counted('apply_rows') - $applied, 0, 'no row written');
is($s->($digest), $at_s1, 's1\'s rows');

# a truncation gives storage of another lineage: main's kept roots of the
# table are gone, and the reset writes the rows
sql(q{TRUNCATE app.t; INSERT INTO app.t SELECT g, 7 FROM generate_series(1, 3000) g});
($restored, $applied) = reset_to($a);
cmp_ok($applied, '>', 0, 'after a truncation, a\'s rows written, not restored');
is(sql($digest), $at_a, 'a\'s rows');
my $c = sql(q{UPDATE app.t SET v = 8 WHERE id <= 10;
	SELECT (dolt_commit('-am', 'c'))[1]});
my $at_c = sql($digest);
($restored, $applied) = reset_to($a);
($restored, $applied) = reset_to($c);
cmp_ok($restored, '>', 0, 'a commit kept after the truncation restores');
is($applied, 0, 'no row written');
is(sql($digest), $at_c, 'c\'s rows');

# a dropped table: nothing of it restores into its new one
sql(q{DROP TABLE app.t;
	CREATE TABLE app.t (id int PRIMARY KEY, v int NOT NULL);
	CREATE INDEX t_v ON app.t (v)});
($restored, $applied) = reset_to($c);
is(sql($digest), $at_c, 'after a drop, c\'s rows');

$node->stop;
done_testing();
