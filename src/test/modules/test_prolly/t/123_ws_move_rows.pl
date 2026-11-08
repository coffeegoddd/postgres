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
	CREATE TABLE ident (id integer GENERATED ALWAYS AS IDENTITY PRIMARY KEY, v text);
	CREATE TABLE ser (id serial PRIMARY KEY, v text);
	CREATE TABLE spans (id integer PRIMARY KEY, r int4range, EXCLUDE USING gist (r WITH &&));
	CREATE TABLE duq (id integer PRIMARY KEY, code integer UNIQUE DEFERRABLE);
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
	INSERT INTO ident (v) VALUES ('one');
	INSERT INTO ser (v) VALUES ('one');
	INSERT INTO spans VALUES (1, '[1,5)'), (2, '[5,10)');
	INSERT INTO duq VALUES (1, 10), (2, 20);
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
	INSERT INTO ident (v) VALUES ('two');
	INSERT INTO ser (v) VALUES ('two');
	UPDATE spans SET r = '[20,30)' WHERE id = 1; UPDATE spans SET r = '[1,5)' WHERE id = 2; UPDATE spans SET r = '[5,10)' WHERE id = 1;
	UPDATE duq SET code = 30 - code;
	SELECT nextval('counter')});
my $b = commit_root();
is($b, read_root(), 'the second is too');
isnt($a, $b, 'and they differ');
# (what the user's code does: a trigger says so, and the rule would have a row not go in)
my $log_at = -s $node->logfile;

# Back to the first.
my $stats = sql(qq{SELECT test_ws_move('app', '$a')});
like($stats, qr/^tables 11 deleted \d+ inserted \d+ sequences 3 made 0 dropped 0 altered 0 kept 0$/, "a move to the first writes each table and sequence that differs: $stats");
my ($deleted, $inserted) = $stats =~ /deleted (\d+) inserted (\d+)/;
# kv: 5 updated taken away and put in again, 5 deleted put back, the new one taken away; notes: a 'b' and
# the 'c' away, an 'a' back; parted: the row from high back to low; codes, later, spans and duq: both
# rows; gen: one; ident and ser: the row added taken away
is($deleted, 5 + 1 + 2 + 1 + 2 + 2 + 1 + 1 + 1 + 2 + 2, 'taking away only the rows that differ');
is($inserted, 5 + 5 + 1 + 1 + 2 + 2 + 1 + 0 + 0 + 2 + 2, 'and putting in only those');
is(read_root(), $a, 'the tables hold the first root');
is(capture_root(), $a, 'and capture has it, without the writes decoded back');
unlike(slurp_file($node->logfile, $log_at), qr/user code ran/, 'no trigger ran, ALWAYS or REPLICA');
is(sql(q{SELECT string_agg(id || code, ',' ORDER BY id) FROM app.codes}), '1x,2y', 'nor a rule');
is(sql(q{SELECT b FROM app.gen WHERE id = 1}), '2', 'a stored generated column as the root has it');
is(sql(q{SELECT tableoid::regclass FROM app.parted WHERE id = 1}), 'app.parted_low', 'a row in the partition it was in');
is(sql(q{SELECT last_value || ' ' || is_called FROM app.counter}), '2 true', 'a sequence where it was');
is(sql(q{SELECT (SELECT last_value FROM app.ident_id_seq) || ' ' || (SELECT last_value FROM app.ser_id_seq)}), '1 1',
	"an identity's and a serial's where they were");
is(indexes_check(), '12', 'each index agrees with its table');

# To the second, with capture held from applying it: the target's maps are held for capture until then.
my $injections = $ENV{enable_injection_points} eq 'yes' && !$windows_os;
my $told = q{SELECT count(*) FROM pg_history.pointers WHERE name LIKE 'capture/told/%'};
if ($injections)
{
	$node->safe_psql('postgres', "CREATE EXTENSION injection_points");
	my $bg = $node->background_psql('postgres');
	$bg->query_safe($on);
	$node->safe_psql('postgres', q{SELECT injection_points_attach('capture-apply-begin', 'wait')});
	# (the move's own barrier has nothing to apply, and the worker then waits to apply the move)
	$bg->query_safe(qq{SELECT test_ws_move('app', '$b') AS stats \\gset});
	$stats = $bg->query_safe(q{\echo :stats});
	$node->wait_for_event('capture worker', 'capture-apply-begin');
	is($node->safe_psql('postgres', $told), '11', "each map told is held while capture hasn't applied it");
	$node->safe_psql('postgres', q{SELECT injection_points_detach('capture-apply-begin'); SELECT injection_points_wakeup('capture-apply-begin')});
	$bg->quit;
}
else
{
	$stats = sql(qq{SELECT test_ws_move('app', '$b')});
}
like($stats, qr/^tables 11 deleted 21 inserted 20 sequences 3 made 0 dropped 0 altered 0 kept 0$/, "a move to the second: $stats");
is(read_root(), $b, 'holds the second root');
is(capture_root(), $b, 'and capture has it');
is($node->safe_psql('postgres', $told), '0', 'and no longer holds the maps for it once it has applied them');
is(sql(qq{SELECT test_ws_move('app', '$b')}), 'tables 0 deleted 0 inserted 0 sequences 0 made 0 dropped 0 altered 0 kept 0', 'a move to where it is writes nothing');

# Writing after a move, applied with it: the worker held once its barrier is saved, so that it reads the
# move and the writes after it in one go.  Capture goes on from the root's maps, an update that leaves a
# value out of line where it was finding it in them.
SKIP:
{
	skip 'injection points not supported by this build', 2 unless $injections;
	$node->safe_psql('postgres', q{SELECT injection_points_attach('capture-marker-saved', 'wait')});
	sql(qq{SELECT test_ws_move('app', '$a')});
	$node->wait_for_event('capture worker', 'capture-marker-saved');
	sql(q{UPDATE app.kv SET v = 'after' WHERE id = 1; INSERT INTO app.kv VALUES (200, 'after', NULL);
		DELETE FROM app.notes WHERE body IS NULL});
	$node->safe_psql('postgres', q{SELECT injection_points_detach('capture-marker-saved'); SELECT injection_points_wakeup('capture-marker-saved')});
	my $after = read_root();
	is(capture_root(), $after, 'what is written after a move is captured on top of it, applied with it');
	is($node->safe_psql('postgres', $told), '0', 'and the maps are let go of');
	sql(qq{SELECT test_ws_move('app', '$b')});
}

# A restart just after a move: capture tells it again, or passes over what it applied.
sql(qq{SELECT test_ws_move('app', '$a')});
$node->stop('immediate');
$node->start;
is(capture_root(), $a, 'capture has a move after a crash');
is($node->safe_psql('postgres', $told), '0', 'and lets go of its maps, whether it applied them before or after');
is(read_root(), $a, 'as the tables do');

# Writers wait for a move, and what one commits while the move waits for it is moved too.
{
	my $writer = $node->background_psql('postgres');
	$writer->query_safe("$on BEGIN; INSERT INTO app.notes VALUES ('late')");
	my $mover = $node->background_psql('postgres');
	$mover->query_safe($on);
	$mover->query_until(qr/moving/, "\\echo moving\nSELECT test_ws_move('app', '$b');\n");
	$node->poll_query_until('postgres', q{SELECT count(*) > 0 FROM pg_locks WHERE NOT granted}) or die 'the move did not wait';
	$writer->query_safe('COMMIT');
	$mover->query_safe('SELECT 1');
	is(read_root(), $b, 'a keyless row a writer committed as the move waited is taken away too');
	$writer->quit;

	$mover->query_safe(qq{BEGIN; SELECT test_ws_move('app', '$a')});
	my ($ret, $out, $err) = $node->psql('postgres', "$on SET lock_timeout = '100ms'; SELECT nextval('app.counter')");
	like($err, qr/lock timeout/, 'a sequence is held from use during a move');
	($ret, $out, $err) = $node->psql('postgres', "$on SET lock_timeout = '100ms'; INSERT INTO app.kv VALUES (400, 'w', NULL)");
	like($err, qr/lock timeout/, 'and a table from writers');
	is($node->safe_psql('postgres', "$on SET lock_timeout = '100ms'; SELECT count(*) FROM app.kv WHERE id = 100"), '1',
		"while readers read what was there");
	$mover->query_safe('COMMIT');
	$mover->quit;
	is(read_root(), $a, 'which the move then commits');
}

# What a move asks is asked before anything's held, writing pg_history among it, which is a superuser's:
# a role that isn't one is refused without waiting for a writer first.
$node->safe_psql('postgres', "SET catalog_branch = $branch; " . q{CREATE ROLE mover; GRANT USAGE ON SCHEMA app TO mover;
	GRANT SELECT ON ALL TABLES IN SCHEMA app TO mover; GRANT SELECT ON ALL SEQUENCES IN SCHEMA app TO mover;
	GRANT USAGE ON SCHEMA pg_history TO mover; GRANT SELECT ON pg_history.capture_barriers, pg_history.capture_healing TO mover;
	GRANT SELECT, INSERT, UPDATE ON pg_history.pointers TO mover;
	GRANT EXECUTE ON FUNCTION test_ws_move(text, text, integer), test_ws_use_history_store() TO mover});
my $writing = $node->background_psql('postgres');
$writing->query("$on BEGIN; INSERT INTO app.kv VALUES (500, 'w', NULL)");
my ($ret, $out, $err) = $node->psql('postgres', "$on SET ROLE mover; SET lock_timeout = '5s'; SELECT test_ws_move('app', '$b')");
like($err, qr/permission denied for table pointers/, 'a move by a role that may not write pg_history is refused, not left waiting for a writer first');
$node->safe_psql('postgres', "$on REVOKE SELECT ON ALL TABLES IN SCHEMA app FROM mover");
$writing->{stderr} = '';
$writing->query_safe('ROLLBACK');
$writing->quit;
$node->safe_psql('postgres', "$on REVOKE SELECT ON ALL SEQUENCES IN SCHEMA app FROM mover");

# A table made between the lock and the root would have writers the move doesn't hold off.
SKIP:
{
	skip 'injection points not supported by this build', 1 unless $injections;
	$node->safe_psql('postgres', q{SELECT injection_points_attach('capture-barrier-locked', 'wait')});
	my $mover = $node->background_psql('postgres', on_error_stop => 0);
	$mover->query_safe($on);
	$mover->query_until(qr/moving/, "\\echo moving\nSELECT test_ws_move('app', '$b');\n");
	$node->wait_for_event('client backend', 'capture-barrier-locked');
	sql(q{CREATE TABLE app.sneaked (id integer PRIMARY KEY)});
	$node->safe_psql('postgres', q{SELECT injection_points_detach('capture-barrier-locked'); SELECT injection_points_wakeup('capture-barrier-locked')});
	$mover->query('SELECT 1');
	like($mover->{stderr}, qr/a table of schema "app" was made or dropped as it was being moved/, 'is refused, to try again');
	$mover->quit;
	sql(q{DROP TABLE app.sneaked});
}
is(read_root(), $a, 'and leaves the tables');

# Rolled back, or failing, it leaves everything as it was.
sql(qq{BEGIN; SELECT test_ws_move('app', '$b'); ROLLBACK});
is(read_root(), $a, 'a move rolled back leaves the tables');
is(capture_root(), $a, 'and capture');
is(sql(q{SELECT last_value || ' ' || is_called FROM app.counter}), '2 true', 'and the sequence, whose setting is otherwise not undone');

($ret, $out, $err) = $node->psql('postgres', "$on BEGIN; INSERT INTO app.kv VALUES (300, 'first', NULL); SELECT test_ws_move('app', '$b'); COMMIT");
like($err, qr/a commit from capture can't run in a transaction that has written anything/, 'a move after a write is refused');

unlike(slurp_file($node->logfile), qr/^PANIC:|terminated by signal|capture can't apply/m, 'nothing crashed, and capture applied it all');
$node->stop;
done_testing();
