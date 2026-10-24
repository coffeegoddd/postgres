# Copyright (c) 2026, PostgreSQL Global Development Group

# A branch first used of main's kept pages shares them with another map, so
# VACUUM on main leaves them alone.  Once that branch is deleted and its
# map let go of, a page sweep, counting every holder, lowers main's
# shared_below: VACUUM sets the pages all-visible again.  Not while another
# map still shares them.  The lowering is logged: a crash after it keeps
# it, and a standby replays it.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('shared_below_sweep');
$node->init(allows_streaming => 1);
$node->append_conf(
	'postgresql.conf', q{
autovacuum = off
prolly_fold_worker = off
checkpoint_timeout = 1h
wal_keep_size = '256MB'
wal_consistency_checking = 'all'
});
$node->start;
$node->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('shared_below_sweep_standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sub visible
{
	return sql(q{SELECT relallvisible || '/' || relpages FROM pg_class
		WHERE oid = 'kv.u'::regclass});
}

sql(q{
	CREATE SCHEMA kv VERSIONED;
	CREATE TABLE kv.u (id int PRIMARY KEY, v text) WITH (autovacuum_enabled = off);
	INSERT INTO kv.u SELECT g, repeat('y', 100) FROM generate_series(1, 3000) g;
	SELECT dolt_commit('-Am', 'c1');
	SELECT dolt_branch('b');
	SELECT dolt_branch('c');
});
my $pages = sql(q{SELECT pg_relation_size('kv.u') / 8192});
cmp_ok($pages, '>', 20, 'the table has some pages');

# b and c first used of the pages main keeps as of c1: shared elsewhere
is($node->safe_psql('postgres/b', 'SELECT count(*) FROM kv.u'), '3000', 'b is first used');
is($node->safe_psql('postgres/c', 'SELECT count(*) FROM kv.u'), '3000', 'c is first used');
sql('VACUUM kv.u');
is(visible(), "0/$pages", 'VACUUM leaves the pages b and c share');

# b deleted and swept: c still shares the pages, which stay alone
sql(q{SELECT dolt_branch('-D', 'b')});
sql(q{SELECT count(*) FROM prolly_page_sweep()});
sql('VACUUM kv.u');
is(visible(), "0/$pages", 'with c still sharing them, VACUUM leaves them after a sweep');

# c deleted too: before a sweep, they count as shared still; after it,
# and a crash (the lowering is logged), VACUUM sets them all-visible
sql(q{SELECT dolt_branch('-D', 'c')});
sql('VACUUM kv.u');
is(visible(), "0/$pages", 'before a sweep, VACUUM leaves them still');
sql(q{SELECT count(*) FROM prolly_page_sweep()});
$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));
$node->stop('immediate');
$node->start;
is(sql('SELECT count(*) FROM kv.u'), '3000', 'after a crash, the table as it was');
sql('VACUUM kv.u');
is(visible(), "$pages/$pages", 'after the sweep, VACUUM sets every page all-visible');
is($standby->safe_psql('postgres', 'SELECT count(*) FROM kv.u'), '3000',
	'the standby, having replayed it, reads the table');

# main's own kept pages: a reset still gives c1's rows back
sql(q{UPDATE kv.u SET v = 'changed' WHERE id % 3 = 0; SELECT dolt_reset('--hard')});
is(sql(q{SELECT count(*) FROM kv.u WHERE v = 'changed'}), '0', 'a reset gives c1\'s rows back');

$standby->stop;
$node->stop;
done_testing();
