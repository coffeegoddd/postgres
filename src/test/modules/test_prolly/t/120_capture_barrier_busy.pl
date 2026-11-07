# Copyright (c) 2026, PostgreSQL Global Development Group

# Commits from capture with DDL and row writers going on: a session writing rows and one changing
# tables' columns, each committing over and over, while another makes commit after commit.  Every
# commit has a root, none of them errs or waits on another forever, the DDL never fails, and once
# the writers stop, a commit's root is the tables'.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('busy');
$node->init(allows_streaming => 'logical');
$node->append_conf('postgresql.conf', "log_line_prefix = ''\nautovacuum = off\ncapture_to_trees = on\nlogical_decoding_work_mem = 64kB\n");
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_prolly');
my $b = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
my $on = "SET catalog_branch = $b; SET search_path = s, public;";
$node->safe_psql('postgres', "$on CREATE SCHEMA s VERSIONED;
	CREATE TABLE s.rows_t (id integer PRIMARY KEY, v text);
	CREATE TABLE s.cols_t (id integer PRIMARY KEY, a integer);
	INSERT INTO s.cols_t SELECT g, g FROM generate_series(1, 200) g;
	CREATE TABLE s.done (who text PRIMARY KEY);
	CREATE PROCEDURE public.write_rows(n integer) LANGUAGE plpgsql AS \$\$
	BEGIN
		FOR i IN 1 .. n LOOP
			INSERT INTO s.rows_t VALUES (i, 'v' || i);
			UPDATE s.rows_t SET v = v || '+' WHERE id = i / 2;
			DELETE FROM s.rows_t WHERE id = i - 50;
			UPDATE s.cols_t SET a = a + 1 WHERE id = i % 200 + 1;
			COMMIT;
		END LOOP;
		INSERT INTO s.done VALUES ('rows');
	END \$\$;
	CREATE PROCEDURE public.change_columns(n integer) LANGUAGE plpgsql AS \$\$
	BEGIN
		FOR i IN 1 .. n LOOP
			ALTER TABLE s.cols_t ADD COLUMN extra integer DEFAULT 7;
			COMMIT;
			ALTER TABLE s.rows_t ADD COLUMN note text;
			COMMIT;
			ALTER TABLE s.cols_t DROP COLUMN extra;
			COMMIT;
			ALTER TABLE s.rows_t DROP COLUMN note;
			COMMIT;
		END LOOP;
		INSERT INTO s.done VALUES ('columns');
	END \$\$;");
$node->poll_query_until('postgres', q{SELECT count(*) = 1 FROM pg_stat_activity WHERE backend_type = 'capture worker'}) or die 'no worker';

# the writers, started and left going
my $rows = $node->background_psql('postgres');
$rows->query_until(qr/started/, "\\echo started\n$on CALL write_rows(20000);\n\\echo rows-done\n");
my $cols = $node->background_psql('postgres');
$cols->query_until(qr/started/, "\\echo started\n$on CALL change_columns(400);\n\\echo columns-done\n");

# commits, one after another, as long as they're going
my ($made, $empty) = (0, 0);
while ($node->safe_psql('postgres', "$on SELECT count(*) FROM s.done") < 2)
{
	my $root = $node->safe_psql('postgres', "$on SELECT test_capture_barrier('s')");
	$root =~ /^[0-9a-f]{40}$/ ? $made++ : $empty++;
}
ok($made >= 10, "commits made with writers going on ($made)");
is($empty, 0, 'each with a root');
for my $bg ([$rows, 'rows-done'], [$cols, 'columns-done'])
{
	$bg->[0]{stdout} = '';
	$bg->[0]{run}->pump until $bg->[0]{stdout} =~ /$bg->[1]/ || !$bg->[0]{run}->pumpable;
	like($bg->[0]{stdout}, qr/$bg->[1]/, "$bg->[1], with no error");
	$bg->[0]->quit;
}

my $root = $node->safe_psql('postgres', "$on SELECT test_capture_barrier('s')");
is($root, $node->safe_psql('postgres', "$on SELECT test_ws_root_capture('s')"), 'once they stop, a commit is the tables');
is($node->safe_psql('postgres', "$on SELECT count(*) FROM s.rows_t"), '50', 'the rows written');
unlike(slurp_file($node->logfile), qr/^(ERROR|FATAL|PANIC):|deadlock/m, 'nothing went wrong');

$node->stop;
done_testing();
