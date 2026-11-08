# Copyright (c) 2026, PostgreSQL Global Development Group

# A versioned schema moved in place to a root whose tables differ from its own only in their rows
# and sequences: only what differs is written, the tables then hold the root's rows, capture has
# the root without decoding the writes back, and nothing of the user's runs.  An error, or the
# transaction rolled back, leaves it all as it was.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('move_rows');
$node->init(allows_streaming => 'logical');
$node->append_conf('postgresql.conf', "autovacuum = off\ncapture_to_trees = on\ndebug_block_mapped_heap = on\n");
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_prolly; CREATE EXTENSION amcheck');

my $branch = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
my $on = "SET catalog_branch = $branch; SELECT test_ws_use_history_store() IS NULL AS on_history \\gset\n";

sub sql
{
	my ($sql) = @_;
	return $node->safe_psql('postgres', "$on$sql");
}

# A commit of what capture has, and the root it holds, which the commit keeps.
my $commits = 0;
sub commit_root
{
	$commits++;
	return sql(qq{SELECT test_capture_commit_on('app', 'main', 'c$commits')});
}

# What reading the tables makes, and what capture has as of now.
sub read_root { return sql(q{SELECT test_ws_root_capture('app')}); }
sub capture_root { return sql(q{SELECT test_capture_barrier('app')}); }

# Each index of the schema checked against its table.
sub indexes_check
{
	return sql(q{SELECT count(*) FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid
		JOIN pg_namespace n ON n.oid = c.relnamespace JOIN pg_am a ON a.oid = c.relam
		WHERE n.nspname = 'app' AND a.amname = 'btree' AND c.relkind = 'i'
		AND bt_index_check(i.indexrelid, true) IS NOT NULL});
}

sql(q{CREATE SCHEMA app VERSIONED; SET search_path = app;
	CREATE TABLE kv (id integer PRIMARY KEY, v text, big text);
	CREATE TABLE notes (body text);
	CREATE TABLE parted (id integer, k integer, v text, PRIMARY KEY (id, k)) PARTITION BY RANGE (k);
	CREATE TABLE parted_low PARTITION OF parted FOR VALUES FROM (0) TO (10);
	CREATE TABLE parted_high PARTITION OF parted FOR VALUES FROM (10) TO (20);
	CREATE TABLE codes (id integer PRIMARY KEY, code text UNIQUE);
	CREATE TABLE later (id integer PRIMARY KEY DEFERRABLE INITIALLY DEFERRED, v integer);
	CREATE TABLE gen (id integer PRIMARY KEY, a integer, b integer GENERATED ALWAYS AS (a * 2) STORED);
	CREATE SEQUENCE counter;
	CREATE FUNCTION app.note() RETURNS trigger LANGUAGE plpgsql AS
		$$BEGIN RAISE WARNING 'user code ran: % %', TG_TABLE_NAME, TG_OP; RETURN NULL; END$$;
	CREATE TRIGGER kv_fired AFTER INSERT OR UPDATE OR DELETE ON kv FOR EACH ROW EXECUTE FUNCTION app.note();
	ALTER TABLE kv ENABLE ALWAYS TRIGGER kv_fired;
	CREATE TRIGGER notes_fired BEFORE INSERT OR DELETE ON notes FOR EACH ROW EXECUTE FUNCTION app.note();
	ALTER TABLE notes ENABLE REPLICA TRIGGER notes_fired;
	INSERT INTO kv SELECT g, 'v' || g, CASE WHEN g = 1 THEN repeat('long ', 3000) END FROM generate_series(1, 50) g;
	INSERT INTO notes VALUES ('a'), ('a'), ('b'), (NULL);
	INSERT INTO parted VALUES (1, 1, 'low'), (2, 15, 'high');
	INSERT INTO codes VALUES (1, 'x'), (2, 'y');
	INSERT INTO later VALUES (1, 10), (2, 20);
	INSERT INTO gen (id, a) VALUES (1, 1), (2, 2);
	SELECT nextval('counter'), nextval('counter');
	CREATE RULE codes_nothing AS ON INSERT TO codes DO INSTEAD NOTHING});
my $a = commit_root();
is($a, read_root(), 'the first root is what the tables hold');

sql(q{SET search_path = app;
	UPDATE kv SET v = 'changed' WHERE id <= 5;
	UPDATE kv SET big = repeat('other ', 3000) WHERE id = 1;
	DELETE FROM kv WHERE id > 45;
	INSERT INTO kv VALUES (100, 'new', NULL);
	DELETE FROM notes WHERE ctid = (SELECT min(ctid) FROM notes WHERE body = 'a');
	INSERT INTO notes VALUES ('b'), ('c');
	UPDATE parted SET k = 12 WHERE id = 1;
	UPDATE codes SET code = 'z' WHERE id = 1; UPDATE codes SET code = 'x' WHERE id = 2; UPDATE codes SET code = 'y' WHERE id = 1;
	UPDATE later SET id = 3 - id;
	UPDATE gen SET a = 10 WHERE id = 1;
	SELECT nextval('counter')});
my $b = commit_root();
is($b, read_root(), 'the second is too');
isnt($a, $b, 'and they differ');
# (what the user's code does: a trigger says so, and the rule would have a row not go in)
my $log_at = -s $node->logfile;

# Back to the first.
my $stats = sql(qq{SELECT test_ws_move('app', '$a')});
like($stats, qr/^tables 7 deleted \d+ inserted \d+ sequences 1 made 0 dropped 0$/, "a move to the first writes each table that differs: $stats");
my ($deleted, $inserted) = $stats =~ /deleted (\d+) inserted (\d+)/;
# kv: 5 updated taken away and put in again, 5 deleted put back, the new one taken away; notes: a 'b' and
# the 'c' away, an 'a' back; parted: the row from high back to low; codes and later: both rows; gen: one
is($deleted, 5 + 1 + 2 + 1 + 2 + 2 + 1, 'taking away only the rows that differ');
is($inserted, 5 + 5 + 1 + 1 + 2 + 2 + 1, 'and putting in only those');
is(read_root(), $a, 'the tables hold the first root');
is(capture_root(), $a, 'and capture has it, without the writes decoded back');
unlike(slurp_file($node->logfile, $log_at), qr/user code ran/, 'no trigger ran, ALWAYS or REPLICA');
is(sql(q{SELECT string_agg(id || code, ',' ORDER BY id) FROM app.codes}), '1x,2y', 'nor a rule');
is(sql(q{SELECT b FROM app.gen WHERE id = 1}), '2', 'a stored generated column as the root has it');
is(sql(q{SELECT tableoid::regclass FROM app.parted WHERE id = 1}), 'app.parted_low', 'a row in the partition it was in');
is(sql(q{SELECT last_value || ' ' || is_called FROM app.counter}), '2 true', 'a sequence where it was');
is(indexes_check(), '7', 'each index agrees with its table');

# Writing after a move: capture goes on from the root's maps.
sql(q{INSERT INTO app.kv VALUES (200, 'after', NULL); DELETE FROM app.notes WHERE body IS NULL});
my $after = read_root();
is(capture_root(), $after, 'what is written after a move is captured on top of it');

# To the second, and to where it already is.
like(sql(qq{SELECT test_ws_move('app', '$b')}), qr/^tables 7 /, 'a move to the second');
is(read_root(), $b, 'holds the second root');
is(capture_root(), $b, 'and capture has it');
is(sql(qq{SELECT test_ws_move('app', '$b')}), 'tables 0 deleted 0 inserted 0 sequences 0 made 0 dropped 0', 'a move to where it is writes nothing');

# Rolled back, or failing, it leaves everything as it was.
sql(qq{BEGIN; SELECT test_ws_move('app', '$a'); ROLLBACK});
is(read_root(), $b, 'a move rolled back leaves the tables');
is(capture_root(), $b, 'and capture');
is(sql(q{SELECT last_value || ' ' || is_called FROM app.counter}), '3 true', 'and the sequence, whose setting is otherwise not undone');

my ($ret, $out, $err) = $node->psql('postgres', "$on BEGIN; INSERT INTO app.kv VALUES (300, 'first', NULL); SELECT test_ws_move('app', '$a'); COMMIT");
like($err, qr/a commit from capture can't run in a transaction that has written anything/, 'a move after a write is refused');

unlike(slurp_file($node->logfile), qr/^PANIC:|terminated by signal|capture can't apply/m, 'nothing crashed, and capture applied it all');
$node->stop;
done_testing();
