# Copyright (c) 2026, PostgreSQL Global Development Group

# Under dolt.transaction_commit with prolly.freeze_roots on, a root due at
# a transaction's commit is captured among the branch's other writers, and
# had if the commit's snapshot sees every writer of the branch that
# committed.  The branch remembers its last few writers that ended one by
# one, and only the latest of those before.  A writer at work since before
# them all holds every snapshot's xmin back, so the commits of more
# writers than are remembered once kept the latest of those before from
# preceding the xmin: no snapshot was taken to see them, and no keep was
# had till that writer ended.  Seen is asked of the snapshot's running
# transactions instead: none of those, up to the latest, has committed.
#
# First as it was found: commits with roots not frozen (each keep finds
# the other writer's lock), then roots frozen again in the same session.
# Then with roots frozen throughout, after more commits than the branch
# remembers one by one.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('freeze_after_unfrozen');
$node->init;
$node->append_conf('postgresql.conf', "autovacuum = off\nprolly_fold_worker = off");
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sub counted
{
	my ($name) = @_;
	return sql(qq{SELECT pg_stat_force_next_flush();
		SELECT coalesce((SELECT value FROM pg_stat_prolly WHERE name = '$name'), 0)});
}

sql(q{CREATE SCHEMA fr VERSIONED;
	CREATE TABLE fr.t (id int PRIMARY KEY, v int);
	CREATE TABLE fr.w (id int PRIMARY KEY, v int);
	INSERT INTO fr.t SELECT g, 0 FROM generate_series(1, 100) g;
	INSERT INTO fr.w VALUES (1, 0);
	SELECT dolt_commit('-Am', 'c0')});

# another writer of the branch, at work throughout: it holds fr.w
my $writer = $node->background_psql('postgres');
$writer->query_safe(q{BEGIN; UPDATE fr.w SET v = 1});

# roots not frozen: every keep finds fr.w held (more commits than the
# branch remembers one by one)
sleep(3);
my $kept = counted('pages_kept');
my $s = $node->background_psql('postgres');
$s->query_safe(q{SET dolt.transaction_commit = on; SET prolly.root_interval = '2s';
	SET prolly.freeze_roots = off});
$s->query_safe("UPDATE fr.t SET v = $_ WHERE id = 1") for 1 .. 12;
$s->query_safe('SELECT pg_stat_force_next_flush()');
is(counted('pages_kept'), $kept, 'roots not frozen: no keep had');

# frozen again, the interval passed: the next commit's keep is had
sleep(3);
$s->query_safe(q{SET prolly.freeze_roots = on});
$s->query_safe('UPDATE fr.t SET v = 100 WHERE id = 1');
$s->query_safe('SELECT pg_stat_force_next_flush()');
cmp_ok(counted('pages_kept'), '>', $kept,
	'frozen again: a keep had among the writers');
$s->quit;

# frozen throughout: commits within a long interval try no keep, then one
# due is had
$kept = counted('pages_kept');
my $f = $node->background_psql('postgres');
$f->query_safe(q{SET dolt.transaction_commit = on; SET prolly.root_interval = '1h'});
$f->query_safe("UPDATE fr.t SET v = 200 + $_ WHERE id = 2") for 1 .. 12;
$f->query_safe('SELECT pg_stat_force_next_flush()');
is(counted('pages_kept'), $kept, 'within the interval: no keep tried');
sleep(3);
$f->query_safe(q{SET prolly.root_interval = '2s'});
$f->query_safe('UPDATE fr.t SET v = 300 WHERE id = 2');
$f->query_safe('SELECT pg_stat_force_next_flush()');
cmp_ok(counted('pages_kept'), '>', $kept,
	'frozen throughout: a keep had after more writers ended than are remembered');
$f->quit;

$writer->query_safe('COMMIT');
$writer->quit;
$node->stop;
done_testing();
