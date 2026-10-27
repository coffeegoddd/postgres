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
# (VACUUM sets kept pages all-visible only with this on)
prolly.vacuum_copy_kept_pages = on
# (roots sharing the relations' pages as they are: frozen ones share none)
prolly.freeze_roots = off
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

# main written after a branch's first use of its kept pages: the path to
# the page written is copied, and the subtrees main's tree has in common
# with the kept snapshot are reached through that snapshot by the branch
# too.  A sweep, with the branch still sharing them, changes nothing VACUUM
# does.
sub table_pages
{
	my ($t) = @_;
	return sql(qq{SELECT pg_relation_size('kv.$t') / 8192});
}

sub visible_of
{
	my ($t) = @_;
	return sql(qq{SELECT relallvisible FROM pg_class WHERE oid = 'kv.$t'::regclass});
}

sql(q{
	CREATE TABLE kv.w (id int PRIMARY KEY, v text) WITH (autovacuum_enabled = off);
	INSERT INTO kv.w SELECT g, repeat('w', 100) FROM generate_series(1, 3000) g;
	SELECT dolt_commit('-Am', 'c2');
	SELECT dolt_branch('e');
});
is($node->safe_psql('postgres/e', 'SELECT count(*) FROM kv.w'), '3000', 'e is first used');
sql(q{UPDATE kv.w SET v = 'z' WHERE id = 1});
sql('VACUUM kv.w');
my $before = visible_of('w');
cmp_ok($before, '<', table_pages('w') / 2, 'VACUUM leaves most of the pages e shares');
sql(q{SELECT count(*) FROM prolly_page_sweep()});
sql('VACUUM kv.w');
is(visible_of('w'), $before,
	'main written since e\'s first use: with e still sharing, a sweep changes nothing VACUUM does');

# the same with a plain share (a first use not made of kept pages, while
# main keeps a snapshot)
sql(q{
	CREATE TABLE kv.x (id int PRIMARY KEY, v text) WITH (autovacuum_enabled = off);
	INSERT INTO kv.x SELECT g, repeat('x', 100) FROM generate_series(1, 3000) g;
	SELECT dolt_commit('-Am', 'c3');
	SELECT dolt_branch('f');
});
is((split /\n/, sql(q{SET prolly.kept_commits = 0; SELECT dolt_checkout('f');
	SELECT count(*) FROM kv.x}))[-1], '3000', 'f is first used, sharing main\'s storage');
sql(q{UPDATE kv.x SET v = 'z' WHERE id = 1});
sql('VACUUM kv.x');
$before = visible_of('x');
cmp_ok($before, '<', table_pages('x') / 2, 'VACUUM leaves most of the pages f shares');
sql(q{SELECT count(*) FROM prolly_page_sweep()});
sql('VACUUM kv.x');
is(visible_of('x'), $before,
	'with f still sharing, a sweep changes nothing VACUUM does');

# and once e and f are deleted and swept, every page is vacuumed
sql(q{SELECT dolt_branch('-D', 'e'); SELECT dolt_branch('-D', 'f')});
sql(q{SELECT count(*) FROM prolly_page_sweep()});
sql('VACUUM kv.w; VACUUM kv.x');
is(visible_of('w'), table_pages('w'), 'e gone: every page of w all-visible');

# a table restored of a commit's root (a hard reset) holds that root as its
# own, as one captured of it: once the branch that shared it is gone, its
# pages are vacuumed after a sweep has looked
sql(q{
	CREATE TABLE kv.r (id int PRIMARY KEY, v text) WITH (autovacuum_enabled = off);
	INSERT INTO kv.r SELECT g, repeat('r', 100) FROM generate_series(1, 3000) g;
	SELECT dolt_commit('-Am', 'r1');
	UPDATE kv.r SET v = 's';
	SELECT dolt_commit('-am', 'r2');
	SELECT dolt_reset('--hard', 'HEAD~1');
});
is(sql(q{SELECT count(*) FROM kv.r WHERE v = 's'}), '0', 'the reset gives r1\'s rows back');
sql(q{SELECT dolt_branch('g')});
is($node->safe_psql('postgres/g', 'SELECT count(*) FROM kv.r'), '3000', 'g is first used');
sql('VACUUM kv.r');
cmp_ok(visible_of('r'), '<', table_pages('r') / 2, 'VACUUM leaves the pages g shares');
sql(q{SELECT dolt_branch('-D', 'g')});
sql(q{SELECT count(*) FROM prolly_page_sweep()});
sql('VACUUM kv.r');
is(visible_of('r'), table_pages('r'),
	'restored of r1\'s root, held as its own: every page all-visible');
is(visible_of('x'), table_pages('x'), 'f gone: every page of x all-visible');

$standby->stop;
$node->stop;
done_testing();
