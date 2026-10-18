# Copyright (c) 2026, PostgreSQL Global Development Group

# A long history: dolt_log() lists every commit, ordered by its depth,
# however deep the history is (without a stack frame a commit).

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('long_history');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE SCHEMA lh VERSIONED;
	CREATE TABLE lh.t (id int PRIMARY KEY);
	SELECT dolt_commit('-Am', 'base');
	DO $$
	BEGIN
		FOR i IN 1 .. 8000 LOOP
			PERFORM dolt_commit('--allow-empty', '-m', 'empty ' || i);
		END LOOP;
	END $$;
});
is(sql(q{SET max_stack_depth = '100kB';
	SELECT count(*), max(commit_order) FROM dolt_log('main')}),
	'8001|8001', 'every commit listed, the deepest last in order');
is(sql(q{SELECT message FROM dolt_log('main') ORDER BY commit_order DESC LIMIT 1}),
	'empty 8000', 'the head first');

$node->stop;
done_testing();
