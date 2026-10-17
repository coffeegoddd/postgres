# Copyright (c) 2026, PostgreSQL Global Development Group

# A commit's fold doesn't see transactions running when its snapshot was
# taken, or committed since.  Their rows stay out of the commit, and the
# next fold, which starts from the commit's, still finds them, though
# their transactions' commits changed no page.  A fold never starts from
# a baseline its snapshot is older than, and a standby folds too.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('unseen');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{CREATE EXTENSION test_prolly;
	CREATE SCHEMA c VERSIONED;
	CREATE TABLE c.t (id int PRIMARY KEY, v text);
	CREATE TABLE c.k (a int, b text);
	INSERT INTO c.t SELECT g, 'v' || g FROM generate_series(1, 2000) g;
	INSERT INTO c.k SELECT g, 'k' FROM generate_series(1, 100) g;
	SELECT dolt_commit('-Am', 'base')});
my $same = q{SELECT test_fold_root() = test_fold_root(true)};
my $diff = q{SELECT string_agg(diff_type || ' ' || coalesce((to_row).id, (from_row).id), ', '
	ORDER BY coalesce((to_row).id, (from_row).id)) FROM dolt_diff('HEAD~1', 'HEAD', NULL::c.t)};

# A transaction running across a commit.
my $a = $node->background_psql('postgres');
$a->query_safe(q{BEGIN; UPDATE c.t SET v = 'a' WHERE id = 1;
	INSERT INTO c.t VALUES (5000, 'new'); DELETE FROM c.t WHERE id = 2;
	INSERT INTO c.k VALUES (-1, 'running')});
sql(q{UPDATE c.t SET v = 'committed' WHERE id = 3; SELECT dolt_commit('-am', 'while a runs')});
is(sql($diff), 'modified 3', 'the commit has only what was committed');
$a->query_safe('COMMIT');
is(sql($same), 't', 'the next fold finds the running transaction\'s rows');
is(sql(q{SELECT string_agg(table_name, ', ' ORDER BY 1) FROM dolt_status}), 'c.k, c.t',
	'they are changes of the working set');
sql(q{SELECT dolt_commit('-am', 'a committed')});
is(sql($diff), 'modified 1, removed 2, added 5000', 'and of its next commit');
is(sql(q{SELECT count(*) FROM c.k WHERE a = -1}), '1', 'a keyless table\'s too');
is(sql(q{SELECT count(*) FROM dolt_status}), '0', 'nothing is left over');

# A transaction that committed after the commit's snapshot was taken.
my $rr = $node->background_psql('postgres');
$rr->query_safe(q{BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1});
sql(q{UPDATE c.t SET v = 'late' WHERE id = 1500});
$rr->query_safe(q{UPDATE c.t SET v = 'rr' WHERE id = 5; SELECT dolt_commit('-am', 'repeatable read'); COMMIT});
is(sql($diff), 'modified 5', 'a commit\'s snapshot leaves out what committed after it');
is(sql($same), 't', 'which the next fold finds');
sql(q{SELECT dolt_commit('-am', 'late')});
is(sql($diff), 'modified 1500', 'and commits');

# A transaction that aborts after a commit's fold saw it running.
$a->query_safe(q{BEGIN; UPDATE c.t SET v = 'aborted' WHERE id = 6});
sql(q{SELECT dolt_commit('--allow-empty', '-am', 'while it runs')});
$a->query_safe('ROLLBACK');
is(sql($same), 't', 'an aborted one\'s rows are as they were');
is(sql(q{SELECT count(*) FROM dolt_status}), '0', 'and nothing changed');

# A running transaction's row whose key is stored out of line: the
# commit's fold can't carry its key, so the next fold reads the table.
sql(q{CREATE TABLE c.x (id text PRIMARY KEY, v int);
	ALTER TABLE c.x ALTER COLUMN id SET STORAGE EXTERNAL;
	INSERT INTO c.x VALUES ('a', 1);
	SELECT dolt_commit('-Am', 'x')});
$a->query_safe(q{BEGIN; INSERT INTO c.x VALUES (repeat('k', 2500), 2)});
sql(q{UPDATE c.x SET v = 0 WHERE id = 'a'; SELECT dolt_commit('-am', 'while the long key runs')});
$a->query_safe('COMMIT');
is(sql($same), 't', 'a row whose key wasn\'t carried is found');
is(sql(q{SELECT string_agg(table_name, ', ') FROM dolt_status}), 'c.x', 'as a change');
sql(q{SELECT dolt_commit('-am', 'long key')});

# A snapshot older than the baseline's fold: the baseline has what the
# snapshot doesn't see, so the table is read in full.
$rr->query_safe(q{BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1});
sql(q{UPDATE c.t SET v = 'newer' WHERE id = 7; SELECT dolt_commit('-am', 'newer')});
is($rr->query_safe($same), 't', 'an older snapshot doesn\'t fold from a newer baseline');
is($rr->query_safe(q{SELECT string_agg(table_name, ', ') FROM dolt_status}), 'c.t',
	'and sees the working set as it was');
$rr->query_safe('COMMIT');

# A standby folds, though it can't take the lock (nor begin a fold).
$node->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('unseen_standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->start;
sql(q{UPDATE c.t SET v = 'standby' WHERE id = 8; SELECT dolt_commit('-am', 'standby')});
$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));
is($standby->safe_psql('postgres', q{SELECT count(*) FROM dolt_status}), '0',
	'a standby folds the working set');

$standby->stop;
$a->quit;
$rr->quit;
$node->stop;
done_testing();
