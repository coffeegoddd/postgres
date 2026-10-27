# Copyright (c) 2026, PostgreSQL Global Development Group

# By default (prolly.vacuum_copy_kept_pages off), VACUUM leaves the pages a
# commit keeps as they are: it sets none of them all-visible and copies
# none, so the kept versions cost no storage of their own.  An aggressive
# VACUUM still freezes them, copying them.  (t/237 covers the setting on.)

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('vacuum_kept_default');
$node->init;
$node->append_conf(
	'postgresql.conf', q{
autovacuum = off
prolly_fold_worker = off
});
$node->start;
sub sql { return $node->safe_psql('postgres', $_[0]); }
sql('CREATE EXTENSION test_prolly');

is(sql(q{SHOW prolly.vacuum_copy_kept_pages}), 'off',
	'prolly.vacuum_copy_kept_pages is off by default');

sql(q{
	CREATE SCHEMA kv VERSIONED;
	CREATE TABLE kv.z (id int PRIMARY KEY, v text) WITH (autovacuum_enabled = off);
	INSERT INTO kv.z SELECT g, repeat('z', 100) FROM generate_series(1, 3000) g;
	SELECT dolt_commit('-Am', 'z');
});
is(sql(q{SELECT test_bmap_roots_of(pg_relation_filenode('kv.z'))}),
	'1', 'the commit keeps the table by a root');
my $pages = sql(q{SELECT pg_relation_size('kv.z') / 8192});
my $visible = q{SELECT relallvisible || '/' || relpages FROM pg_class
	WHERE oid = 'kv.z'::regclass};
my $map = q{SELECT string_agg(phys::text, ',' ORDER BY blkno)
	FROM test_bmap_map(pg_relation_filenode('kv.z'))};
my $before = sql($map);

sql('VACUUM kv.z');
is(sql($visible), "0/$pages", 'VACUUM leaves the kept pages as they are');
is(sql($map), $before, 'copying none of them');

# an aggressive VACUUM still freezes them, copying them
sql('VACUUM (FREEZE) kv.z');
is(sql($visible), "$pages/$pages", 'VACUUM FREEZE still freezes them');
isnt(sql($map), $before, 'copying them');

$node->stop;
done_testing();
