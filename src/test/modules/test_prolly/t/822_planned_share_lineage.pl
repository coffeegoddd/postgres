# Copyright (c) 2026, PostgreSQL Global Development Group

# A move that gives a table another branch's kept pages where that costs
# less than writing its rows (a share the move plans) gives the table new
# storage that carries its lineage on, as a restore's does: the pages are
# given only where the table is stored alike on both branches, so the
# branch's own kept roots of the table stay restorable.  A reset back to
# one of the branch's own kept commits after such a move restores it,
# writing no row.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('planned_share_lineage');
$node->init;
$node->append_conf('postgresql.conf', qq{
autovacuum = off
prolly_fold_worker = off
});
$node->start;
sub sql { return $node->safe_psql('postgres', $_[0]); }
sub on_b
{
	return $node->safe_psql('postgres',
		"DO \$\$ BEGIN PERFORM dolt_checkout('b'); END \$\$;\n" . $_[0]);
}
sub counted
{
	my ($name) = @_;
	return sql(qq{SELECT coalesce((SELECT value FROM pg_stat_prolly
		WHERE name = '$name'), 0)});
}
my $digest = q{SELECT md5(string_agg(id || ':' || v, ',' ORDER BY id)) FROM app.t};

sql(q{ALTER DATABASE postgres SET prolly.history_window = '1000 years'});
sql(q{CREATE SCHEMA app VERSIONED;
	CREATE TABLE app.t (id int PRIMARY KEY, v int NOT NULL);
	CREATE INDEX t_v ON app.t (v);
	CREATE TABLE app.u (id int PRIMARY KEY, v int NOT NULL);
	INSERT INTO app.t SELECT g, 0 FROM generate_series(1, 20000) g;
	INSERT INTO app.u VALUES (1, 0)});
sql(q{SELECT dolt_commit('-Am', 'c0')});
sql(q{SELECT dolt_branch('b')});

# main: c1 changes many of t's rows, and keeps its pages
sql(q{UPDATE app.t SET v = 1 WHERE id <= 8000});
sql(q{SELECT dolt_commit('-am', 'c1')});

# b: b0, its own (t's row 1 changed), kept; then b1 with t as main's c1
# has it, but u changed, and no root of its own
on_b(q{UPDATE app.t SET v = 2 WHERE id = 1});
my $b0 = on_b(q{SELECT (dolt_commit('-am', 'b0'))[1]});
my $at_b0 = on_b($digest);
my $b1 = on_b(q{SET debug_prolly_page_roots = off;
	UPDATE app.t SET v = 1 WHERE id <= 8000;
	UPDATE app.u SET v = 1;
	SELECT (dolt_commit('-am', 'b1'))[1]});
my $at_b1 = on_b($digest);

# back to b0 (restored), then to b1: t is given main's kept pages of c1,
# not 8,000 rows written
on_b(qq{SELECT dolt_reset('--hard', '$b0')});
is(on_b($digest), $at_b0, 'b0\'s rows');
my $shared = counted('move_tables_shared');
on_b(qq{SELECT dolt_reset('--hard', '$b1')});
cmp_ok(counted('move_tables_shared'), '>', $shared,
	't given main\'s kept pages of c1');
is(on_b($digest), $at_b1, 'b1\'s rows');

# b's own kept b0 is still of t's storage: restored, no row written
my $restored = counted('relations_restored');
my $applied = counted('apply_rows');
on_b(qq{SELECT dolt_reset('--hard', '$b0')});
cmp_ok(counted('relations_restored') - $restored, '>', 0,
	'b0 restored from its own root after the share');
is(counted('apply_rows') - $applied, 0, 'no row written');
is(on_b($digest), $at_b0, 'b0\'s rows');
is(on_b(q{SET enable_seqscan = off; SELECT count(*) FROM app.t WHERE v = 2}),
	'1', 'found by the index');

$node->stop;
done_testing();
