# Copyright (c) 2026, PostgreSQL Global Development Group

# A hard reset to a commit whose kept pages are gone may give a table the
# pages kept as of the head, which isn't MVCC-safe: a snapshot taken
# before the reset commits would then read the head's rows, neither its
# own nor the commit's.  So while another backend holds an older snapshot
# (a REPEATABLE READ transaction here, which hasn't read the table, so
# holds no lock a restore would find taken), the reset writes the rows
# instead, and that snapshot keeps reading what it saw.  Once it ends, the head's
# pages are given back as before.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('head_restore_snapshot');
$node->init;
$node->append_conf('postgresql.conf', qq{
autovacuum = off
prolly_fold_worker = off
prolly.kept_commits = 1
});
$node->start;
sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{CREATE SCHEMA q VERSIONED;
	CREATE TABLE q.t (id int PRIMARY KEY, v int);
	INSERT INTO q.t SELECT g, 0 FROM generate_series(1, 1000) g;
	CREATE TABLE q.u (id int PRIMARY KEY)});
my $a = sql(q{SELECT (dolt_commit('-Am', 'a'))[1]});
sql(q{UPDATE q.t SET v = 1 WHERE id <= 100; SELECT dolt_commit('-am', 'b')});
sql(q{UPDATE q.t SET v = 2 WHERE id <= 100; SELECT dolt_commit('-am', 'c')});
my $c = sql(q{SELECT dolt_hashof('HEAD')});
my $counts = q{SELECT pg_stat_force_next_flush();
	SELECT value FROM pg_stat_prolly WHERE name = 'relations_restored'};

# (back and forth, so a is no longer kept and c, the head, is: only the
# head is kept here, as a restore carries the other kept commits along)
sub cycle
{
	sql("SELECT dolt_reset('--hard', '$c')");
	sql("SELECT dolt_reset('--hard', '$a')");
	sql("SELECT dolt_reset('--hard', '$c')");
}
cycle();
sql(q{UPDATE q.t SET v = 5});
is(sql('SELECT sum(v) FROM q.t'), '5000', 'the working set changed');

my $rr = $node->background_psql('postgres');
# (its snapshot taken reading another table: no lock held on q.t, which
# the restore would otherwise find taken)
$rr->query_safe(
	q{BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM q.u});
my $before = sql($counts);
sql("SELECT dolt_reset('--hard', '$a')");
is(sql($counts), $before, 'with an older snapshot held, nothing is restored');
is(sql('SELECT sum(v) FROM q.t'), '0', 'the reset reached the commit');
is($rr->query_safe('SELECT sum(v) FROM q.t'),
	'5000', 'the older snapshot still reads what it saw');
$rr->query_safe('COMMIT');
$rr->quit;

# With no older snapshot, the head's pages are given back.
cycle();
sql(q{UPDATE q.t SET v = 5});
$before = sql($counts);
sql("SELECT dolt_reset('--hard', '$a')");
cmp_ok(sql($counts), '>', $before,
	'with no older snapshot, the head\'s pages are given back');
is(sql('SELECT sum(v) FROM q.t'), '0', 'and the reset reached the commit');

$node->stop;
done_testing();
