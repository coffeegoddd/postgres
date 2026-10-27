# Copyright (c) 2026, PostgreSQL Global Development Group

# VACUUM sets pages all-visible and freezes them when they are shared only
# with the pages a commit keeps (the relation's own snapshots): it copies
# them, and the commit's pages stay as they were, so a hard reset still
# gives them back.  Pages shared with another branch, by a share or by a
# first use made of the kept pages, are left as before.  A standby replays
# it all, and a crash after leaves both as they were.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('vacuum_kept_pages');
$node->init(allows_streaming => 1);
$node->append_conf(
	'postgresql.conf', q{
autovacuum = off
prolly_fold_worker = off
checkpoint_timeout = 1h
wal_keep_size = '256MB'
wal_consistency_checking = 'all'
prolly.vacuum_copy_kept_pages = on
});
# (roots sharing the relations' pages as they are: frozen ones share none)
$node->append_conf('postgresql.conf', 'prolly.freeze_roots = off');
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_prolly');
$node->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('vacuum_kept_pages_standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sub visible
{
	my ($t, $db) = @_;
	$db //= 'postgres';
	return $node->safe_psql($db, qq{
		SELECT relallvisible || '/' || relpages FROM pg_class
		WHERE oid = '$t'::regclass});
}

sub counted
{
	my ($name) = @_;
	return sql(qq{SELECT pg_stat_force_next_flush();
		SELECT value FROM pg_stat_prolly WHERE name = '$name'});
}

sql(q{
	CREATE SCHEMA kv VERSIONED;
	CREATE TABLE kv.t (id int PRIMARY KEY, v text) WITH (autovacuum_enabled = off);
	INSERT INTO kv.t SELECT g, repeat('x', 100) FROM generate_series(1, 3000) g;
	SELECT dolt_commit('-Am', 'c1');
});
my $pages = sql(q{SELECT pg_relation_size('kv.t') / 8192});
cmp_ok($pages, '>', 20, 'the table has some pages');
is(sql(q{SELECT test_bmap_roots_of(pg_relation_filenode('kv.t'))}),
	'1', 'c1 keeps it by a root');

# Its pages are shared only with c1's snapshot: VACUUM sets them all-visible.
sql('VACUUM kv.t');
is(visible('kv.t'), "$pages/$pages", 'VACUUM sets every page kept by c1 all-visible');

# c1's pages are as they were: a hard reset gives them back.
sql(q{UPDATE kv.t SET v = 'changed' WHERE id % 7 = 0});
my $restored = counted('relations_restored');
sql(q{SELECT dolt_reset('--hard')});
cmp_ok(counted('relations_restored'), '>', $restored, 'a reset gives c1\'s pages back');
is(sql(q{SELECT count(*) FROM kv.t WHERE v = 'changed'}), '0',
	'with c1\'s rows');

# The restored storage, its old storage gone, shares its pages with no
# other map (its own snapshot aside): VACUUM sets them all-visible (the
# restore gives it no visibility map).
sql('VACUUM kv.t');
$pages = sql(q{SELECT pg_relation_size('kv.t') / 8192});
is(visible('kv.t'), "$pages/$pages", 'VACUUM sets the restored storage\'s pages all-visible');

# Changed and committed again: a VACUUM that isn't aggressive, freezing
# what it can, freezes the kept pages too, so the table's relfrozenxid
# moves up (one left shared would hold it back).
sql(q{UPDATE kv.t SET v = 'c2' WHERE id % 5 = 0; SELECT dolt_commit('-am', 'c2')});
# (transactions after c2's, which a page left unfrozen holds it back by)
sql('SELECT txid_current()') for 1 .. 20;
sql(q{SET vacuum_freeze_min_age = 0; VACUUM kv.t});
$pages = sql(q{SELECT pg_relation_size('kv.t') / 8192});
is(visible('kv.t'), "$pages/$pages", 'VACUUM sets c2\'s kept pages all-visible');
cmp_ok(sql(q{SELECT age(relfrozenxid) FROM pg_class WHERE oid = 'kv.t'::regclass}),
	'<=', 3, 'and freezes them, moving relfrozenxid up to now');
sql(q{UPDATE kv.t SET v = 'gone'; SELECT dolt_reset('--hard')});
is(sql(q{SELECT count(*) FILTER (WHERE v = 'c2') || '/' || count(*) FROM kv.t}),
	'600/3000', 'a reset still gives c2\'s pages back');

# A branch first used of the kept pages shares them with another map:
# VACUUM leaves them as before, uncopied.
sql(q{
	CREATE TABLE kv.u (id int PRIMARY KEY, v text) WITH (autovacuum_enabled = off);
	INSERT INTO kv.u SELECT g, repeat('y', 100) FROM generate_series(1, 3000) g;
	SELECT dolt_commit('-Am', 'c3');
	SELECT dolt_branch('b');
});
my $kept = counted('first_use_tables_kept');
is($node->safe_psql('postgres/b', 'SELECT count(*) FROM kv.u'), '3000',
	'b is first used');
cmp_ok(counted('first_use_tables_kept'), '>', $kept, 'made of the pages main keeps');
my $upages = sql(q{SELECT pg_relation_size('kv.u') / 8192});
sql('VACUUM kv.u');
is(visible('kv.u'), "0/$upages", 'VACUUM leaves the pages b shares as they were');

# and on b, whose pages are main's too
$node->safe_psql('postgres/b', 'VACUUM kv.u');
is(visible('kv.u', 'postgres/b'), "0/$upages", 'nor b\'s');

# the standby replays it all
$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));
is($standby->safe_psql('postgres',
		q{SELECT count(*) FILTER (WHERE v = 'c2') || '/' || count(*) FROM kv.t}),
	'600/3000', 'the standby reads the vacuumed table');

# a crash after: the table and the kept pages as they were
$node->stop('immediate');
$node->start;
is(sql(q{SELECT count(*) FILTER (WHERE v = 'c2') || '/' || count(*) FROM kv.t}),
	'600/3000', 'after a crash, the table as it was');
sql(q{UPDATE kv.t SET v = 'again'; SELECT dolt_reset('--hard')});
is(sql(q{SELECT count(*) FILTER (WHERE v = 'c2') || '/' || count(*) FROM kv.t}),
	'600/3000', 'and a reset gives the kept pages back');
is($node->safe_psql('postgres/b', 'SELECT count(*) FROM kv.u'), '3000',
	'b reads its rows');

# what the map knows of b's sharing survives the crash: VACUUM still
# leaves those pages as they were
sql('VACUUM kv.u');
is(visible('kv.u'), "0/$upages", 'after a crash, VACUUM leaves the pages b shares');

$standby->stop;
$node->stop;
done_testing();
