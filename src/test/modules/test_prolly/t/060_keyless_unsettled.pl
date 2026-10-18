# Copyright (c) 2026, PostgreSQL Global Development Group

# A table without a primary key, folded while another transaction's rows
# are in flight on its pages: those pages are carried, and the next fold
# reads them again rather than the whole table, counting the rows that
# committed meanwhile (and not those that didn't).

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('keyless_unsettled');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE EXTENSION test_prolly;
	CREATE SCHEMA ku VERSIONED;
	CREATE TABLE ku.h (n int, v text);
	INSERT INTO ku.h SELECT g, 'base' FROM generate_series(1, 5000) g;
	SELECT dolt_commit('-Am', 'base');
});

# Rows in flight while a commit folds.
my $bg = $node->background_psql('postgres');
$bg->query_safe(q{BEGIN; INSERT INTO ku.h VALUES (-1, 'committed later')});
# (and rows in flight on pages added to the table before the fold)
my $bg3 = $node->background_psql('postgres');
$bg3->query_safe(q{BEGIN;
	INSERT INTO ku.h SELECT 100000 + g, 'extended' FROM generate_series(1, 2000) g});
my $bg2 = $node->background_psql('postgres');
$bg2->query_safe(q{BEGIN; INSERT INTO ku.h VALUES (-2, 'rolled back')});
sql(q{INSERT INTO ku.h VALUES (0, 'mine'); SELECT dolt_commit('-am', 'mine')});
$bg->query_safe(q{COMMIT});
$bg2->query_safe(q{ROLLBACK});
$bg3->query_safe(q{COMMIT});
$bg->quit;
$bg2->quit;
$bg3->quit;

# The next commit counts them, reading the carried pages, not the table.
# (the counters are the backend's: one session)
my $full = sql(q{
	SELECT test_fold_counters(true) \\g /dev/null
	INSERT INTO ku.h VALUES (1, 'after');
	SELECT dolt_commit('-am', 'after') \\g /dev/null
	SELECT full_tables FROM test_fold_counters();
});
is($full, '0', 'the next fold reads the carried pages, not the whole table');
is(sql(q{SELECT dirty FROM dolt_branches() WHERE name = 'main'}), 'f',
	'and its commit holds the rows committed meanwhile');

# What the commit holds is what the table holds.
sql(q{SELECT dolt_reset('--hard', 'HEAD')});
is(sql(q{SELECT string_agg(v, ',' ORDER BY n) FROM ku.h WHERE v NOT IN ('base', 'extended')}),
	'committed later,mine,after',
	'a hard reset to it gives back the committed rows, not the rolled back one');
is(sql(q{SELECT count(*) FROM ku.h}), '7003', 'and every row');

# A fold's own rows, of a subtransaction its (repeatable read) snapshot is
# older than: the fold saw them, though its snapshot can't say so; the next
# fold reads the table in full rather than count them again.
sql(q{
	BEGIN ISOLATION LEVEL REPEATABLE READ;
	SELECT 1;
	SAVEPOINT s;
	INSERT INTO ku.h VALUES (10, 'own subtransaction');
	RELEASE s;
	SELECT dolt_commit('-am', 'own subtransaction');
	COMMIT;
});
sql(q{DELETE FROM ku.h WHERE n = 10; SELECT dolt_commit('-am', 'own row deleted')});
sql(q{SELECT dolt_reset('--hard', 'HEAD')});
is(sql(q{SELECT count(*) FROM ku.h WHERE v = 'own subtransaction'}), '0',
	'a fold after its own subtransaction\'s row counts its deletion');

# Rows in flight on pages added to the table after a fold began: the fold
# carries them; they held nothing it saw, so the next fold reads them as new
# pages, still not the whole table.
SKIP:
{
	skip 'injection points not supported by this build', 3
	  if $ENV{enable_injection_points} ne 'yes';
	sql('CREATE EXTENSION injection_points');

	my $folder = $node->background_psql('postgres');
	$folder->query_safe(q{SELECT injection_points_set_local();
		SELECT injection_points_attach('prolly-fold-begun', 'wait')});
	$folder->query_until(qr/started/, q{
		\\echo started
		INSERT INTO ku.h VALUES (2, 'folder');
		SELECT dolt_commit('-am', 'folding');
		\\echo done
	});
	$node->poll_query_until('postgres', q{SELECT count(*) > 0 FROM pg_stat_activity
		WHERE wait_event = 'prolly-fold-begun'}) or die 'the fold did not wait';
	my $grower = $node->background_psql('postgres');
	$grower->query_safe(q{BEGIN;
		INSERT INTO ku.h SELECT 200000 + g, 'grown' FROM generate_series(1, 2000) g});
	sql(q{SELECT injection_points_wakeup('prolly-fold-begun')});
	$folder->query_until(qr/done/, '');
	$grower->query_safe(q{COMMIT});
	$folder->quit;
	$grower->quit;

	my $full = sql(q{
		SELECT test_fold_counters(true) \\g /dev/null
		INSERT INTO ku.h VALUES (3, 'after growth');
		SELECT dolt_commit('-am', 'after growth') \\g /dev/null
		SELECT full_tables FROM test_fold_counters();
	});
	is($full, '0', 'a fold after pages were added in flight reads no table in full');
	is(sql(q{SELECT dirty FROM dolt_branches() WHERE name = 'main'}), 'f',
		'and its commit holds the rows committed meanwhile');
	is(sql(q{SELECT count(*) FROM ku.h WHERE v = 'grown'}), '2000', 'every one');
}

$node->stop;
done_testing();
