# Copyright (c) 2026, PostgreSQL Global Development Group

# A keyless row's change-log entry keeps its values out of line as their
# hash pointers.  A collection between the write and the fold keeps the
# trees they name, even once only the log's pages name them (the row's
# heap versions vacuumed away): the fold reads them, and the commit after
# has the right rows.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('keyless_toast_gc');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;
sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{CREATE EXTENSION test_prolly;
	CREATE FUNCTION big(seed int) RETURNS text LANGUAGE sql IMMUTABLE AS $$
		SELECT string_agg(md5((seed * 10000 + g)::text), '') FROM generate_series(1, 3200) g
	$$;
	CREATE SCHEMA kg VERSIONED;
	CREATE TABLE kg.t (a int, b text STORAGE EXTERNAL);
	INSERT INTO kg.t SELECT g, big(g) FROM generate_series(1, 4) g;
	SELECT dolt_commit('-Am', 'loaded')});

# a row with a new long value, updated to another, and one deleted: their
# old versions vacuumed away, so only the log names the new value's first
sql(q{INSERT INTO kg.t VALUES (10, big(10))});
sql(q{UPDATE kg.t SET b = big(11) WHERE a = 10});
sql(q{DELETE FROM kg.t WHERE a = 1});
sql(q{VACUUM kg.t});
sql(q{SELECT dolt_gc()});

is(sql(q{SELECT test_fold_root() = test_fold_root(true)}), 't',
	'after a collection, the fold from the baseline is a full one');
sql(q{SELECT dolt_commit('-am', 'after the collection')});
is(sql(q{SELECT count(*) FROM dolt_status}), '0', 'committed clean');
is(sql(q{SELECT string_agg(diff_type || ':' || coalesce((to_row).a, (from_row).a), ',' ORDER BY diff_type)
	FROM dolt_diff('HEAD~1', 'HEAD', NULL::kg.t)}),
	'added:10,removed:1', 'the commit has the rows changed');
is(sql(q{SELECT b = big(11) FROM kg.t WHERE a = 10}), 't',
	'and the long value reads back');
sql(q{SELECT dolt_gc()});
is(sql(q{SELECT count(*) FROM kg.t WHERE length(b) = length(big(1))}), '4',
	'every long value reads after another collection');

$node->stop;
done_testing();
