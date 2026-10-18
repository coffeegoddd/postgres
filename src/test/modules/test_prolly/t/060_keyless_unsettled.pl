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
my $bg2 = $node->background_psql('postgres');
$bg2->query_safe(q{BEGIN; INSERT INTO ku.h VALUES (-2, 'rolled back')});
sql(q{INSERT INTO ku.h VALUES (0, 'mine'); SELECT dolt_commit('-am', 'mine')});
$bg->query_safe(q{COMMIT});
$bg2->query_safe(q{ROLLBACK});
$bg->quit;
$bg2->quit;

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
is(sql(q{SELECT string_agg(v, ',' ORDER BY n) FROM ku.h WHERE v <> 'base'}),
	'committed later,mine,after',
	'a hard reset to it gives back the committed rows, not the rolled back one');
is(sql(q{SELECT count(*) FROM ku.h}), '5003', 'and every row');

$node->stop;
done_testing();
