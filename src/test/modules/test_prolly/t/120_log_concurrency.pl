# Copyright (c) 2026, PostgreSQL Global Development Group

# The change finder "log" with other sessions: a fold by a snapshot older
# than its baseline's fold, a truncation the fold's snapshot doesn't see,
# a prepared transaction's writes, two folds consuming the same entries,
# a branch's first use that empties its copies, and reclamation walking a
# "pages" baseline while "log" is set.  After each, the fold is the tree a
# fold of every row gives.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('log_concurrency');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->append_conf('postgresql.conf', 'max_prepared_transactions = 5');
$node->append_conf('postgresql.conf', 'debug_prolly_change_finder = log');
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

# The working set's fold from the baseline is the full fold's tree.
my $same = q{SELECT test_fold_root() = test_fold_root(true)};

sub wait_for_lock
{
	my ($query) = @_;
	return $node->poll_query_until('postgres', qq{
		SELECT count(*) > 0 FROM pg_locks l JOIN pg_stat_activity a USING (pid)
		WHERE NOT l.granted AND a.query LIKE '%$query%'});
}

sql(q{
	CREATE EXTENSION test_prolly;
	CREATE SCHEMA s VERSIONED;
	CREATE TABLE s.t (id int PRIMARY KEY, v text);
	CREATE TABLE s.u (id int PRIMARY KEY, v text);
	CREATE TABLE s.h (n int, v text);
	INSERT INTO s.h SELECT g, 'x' FROM generate_series(1, 5000) g;
	INSERT INTO s.t SELECT g, 'x' FROM generate_series(1, 100) g;
	INSERT INTO s.u SELECT g, 'x' FROM generate_series(1, 100) g;
	SELECT dolt_commit('-Am', 'base');
});

my $a = $node->background_psql('postgres', on_error_stop => 0);
my $c = $node->background_psql('postgres');

# A snapshot older than the baseline's fold: another session's checkpoint
# consumed the entry of a row this snapshot doesn't see.  Its fold reads
# the table in full, by its old snapshot, without the row, and leaves its
# baseline unsettled, so the next fold reads the table in full again.
$a->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1');
sql(q{INSERT INTO s.t VALUES (1000, 'b')});
sql(q{SELECT prolly_checkpoint()});
$a->query_safe(q{SELECT prolly_checkpoint(); COMMIT});
is(sql($same), 't', 'a fold by an older snapshot leaves its baseline unsettled');
is(sql(q{SELECT count(*) FROM dolt_status}), '1', 'and the row is a change');
sql(q{SELECT dolt_commit('-am', 'after the old snapshot');
	SELECT dolt_reset('--hard')});
is(sql(q{SELECT count(*) FROM s.t WHERE id = 1000}), '1',
	'the commit holds the row');

# A truncation committed after the fold's snapshot: the snapshot reads the
# table's new storage, empty, but not the entry saying its rows were
# replaced.
$a->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM s.t');
sql(q{TRUNCATE s.u});
is($a->query_safe($same), 't',
	'a truncation the snapshot doesn\'t see reads the table in full');
$a->query_safe('COMMIT');
sql(q{SELECT dolt_commit('-am', 'truncated')});

# A prepared transaction's writes, committed after a fold: their entries
# stay for the next.
my $p = $node->background_psql('postgres');
$p->query_safe(q{BEGIN; INSERT INTO s.t VALUES (2000, 'prepared');
	PREPARE TRANSACTION 'lp'});
$p->quit;
sql(q{INSERT INTO s.t VALUES (2001, 'mine');
	SELECT dolt_commit('-am', 'while prepared')});
sql(q{COMMIT PREPARED 'lp'});
is(sql($same), 't', 'a prepared transaction\'s entries stay for the next fold');
sql(q{SELECT dolt_commit('-am', 'prepared'); SELECT dolt_reset('--hard')});
is(sql(q{SELECT count(*) FROM s.t WHERE id = 2000}), '1',
	'and the next commit holds its row');

# Two folds consuming the same entries: the second waits for the first,
# and folds by a statement snapshot taken before it waited.
sql(q{INSERT INTO s.t VALUES (3000, 'twice')});
$a->query_safe(q{BEGIN; SELECT prolly_checkpoint()});
$c->query_until(qr/started/, q{
	\echo started
	SELECT prolly_checkpoint();
	\echo done
});
ok(wait_for_lock('prolly_checkpoint'), 'the second fold waits for the first');
sql(q{INSERT INTO s.t VALUES (3001, 'meanwhile')});
$a->query_safe('COMMIT');
$c->query_until(qr/done/, '');
is($c->{stderr}, '', 'and folds');
is(sql($same), 't', 'two folds consuming the same entries');
sql(q{SELECT dolt_commit('-am', 'twice'); SELECT dolt_reset('--hard')});
is(sql(q{SELECT count(*) FROM s.t WHERE id IN (3000, 3001)}), '2',
	'the commit holds both rows');

# A branch's first use under repeatable read, after a commit the snapshot
# doesn't see: its copies are emptied, the log's too (reset), and folded
# again.
$a->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1');
sql(q{INSERT INTO s.t VALUES (4000, 'unseen')});
$a->query(q{SELECT dolt_checkout('-b', 'lb')});
like($a->{stderr}, qr/could not serialize access/,
	'a first use whose copies hold rows the snapshot lacks is refused');
$a->{stderr} = '';
$a->query_safe('ROLLBACK');
$a->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1');
$a->query_safe(q{SELECT dolt_checkout('-b', 'lb')});
is($a->query_safe($same), 't', 'retried, its fold is a full one\'s');
is($a->query_safe(q{SELECT count(*) FROM dolt_status}), '0', 'clean');
$a->query_safe('COMMIT');
$a->query_safe(q{SELECT dolt_checkout('main')});
sql(q{SELECT dolt_commit('-am', 'unseen')});

# A collection while "log" is set, of a "pages" baseline made while a
# transaction was in flight (a collection waits for none to be): the next
# "pages" fold still reads what the baseline names.  (This doesn't show
# that each state is walked by the finder that wrote it, as here a "pages"
# fold carries nothing a collection would take.)
sql(q{SET debug_prolly_change_finder = pages;
	UPDATE s.h SET v = 'by pages' WHERE n = 1;
	SELECT dolt_commit('-am', 'by pages')});
$c->query_safe(q{BEGIN; INSERT INTO s.h VALUES (-1, 'in flight')});
sql(q{SET debug_prolly_change_finder = pages;
	UPDATE s.h SET v = 'by pages again' WHERE n = 2;
	SELECT dolt_commit('-am', 'by pages again')});
$c->query_safe('COMMIT');
sql(q{SELECT dolt_gc('--full')});
is(sql(qq{SET debug_prolly_change_finder = pages; $same}), 't',
	'a pages baseline survives a collection under log');

$a->quit;
$c->quit;
$node->stop;
done_testing();
