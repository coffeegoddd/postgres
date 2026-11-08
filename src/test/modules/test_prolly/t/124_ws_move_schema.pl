# Copyright (c) 2026, PostgreSQL Global Development Group

# A versioned schema moved in place to roots whose definitions differ from its own: what differs is
# dropped, what that took of the tables the target keeps is made again, what the target has and the
# schema then lacks is made, and the rest keeps its rows and has only those that differ written.
# After each move the tables hold the target, and capture has it.  (Nothing outside a versioned schema
# can depend on what's in it, so nothing outside is dropped with it.)

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('move_schema');
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

my $commits = 0;
sub commit_root
{
	$commits++;
	return sql(qq{SELECT test_capture_commit_on('app', 'main', 'c$commits')});
}

sub read_root { return sql(q{SELECT test_ws_root_capture('app')}); }
sub capture_root { return sql(q{SELECT test_capture_barrier('app')}); }

sub indexes_check
{
	return sql(q{SELECT count(*) FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid
		JOIN pg_namespace n ON n.oid = c.relnamespace JOIN pg_am a ON a.oid = c.relam
		WHERE n.nspname = 'app' AND a.amname = 'btree' AND c.relkind = 'i'
		AND bt_index_check(i.indexrelid, true) IS NULL});
}

# A move to a root, the enum step committed first; what was written, and checks it holds the root.
sub move_to
{
	my ($root, $name) = @_;
	sql(qq{SELECT test_ws_move_prepare('app', '$root')});
	my $stats = sql(qq{SELECT test_ws_move('app', '$root')});
	is(read_root(), $root, "$name: the tables hold the root");
	is(capture_root(), $root, "$name: and capture has it");
	is(indexes_check(), '0', "$name: each index agrees with its table");
	return $stats;
}

sub oid_of { my ($rel) = @_; return sql(qq{SELECT '$rel'::regclass::oid}); }

# The first root: tables, a view and a materialized view over them, a function a default calls, an
# enum a kept table uses, a trigger, and a foreign key between tables.
# (a role whose objects can travel in a commit, and that a commit marks)
$node->safe_psql('postgres', q{CREATE ROLE app_owner; SELECT test_ws_role_mark(test_ws_role_bytes(ARRAY['app_owner']))});
sql(q{CREATE SCHEMA app VERSIONED; GRANT ALL ON SCHEMA app TO app_owner; SET ROLE app_owner; SET search_path = app;
	CREATE TYPE mood AS ENUM ('sad', 'happy');
	CREATE FUNCTION app.start() RETURNS integer LANGUAGE sql IMMUTABLE AS 'SELECT 7';
	CREATE TABLE people (id integer PRIMARY KEY, name text, feels mood);
	CREATE TABLE pets (id integer PRIMARY KEY, owner integer REFERENCES people, name text);
	CREATE TABLE counted (id integer PRIMARY KEY, n integer DEFAULT app.start());
	CREATE TABLE gone (id integer PRIMARY KEY);
	CREATE VIEW named AS SELECT id, name FROM people;
	CREATE MATERIALIZED VIEW pet_names AS SELECT name FROM pets;
	CREATE FUNCTION app.loud() RETURNS trigger LANGUAGE plpgsql AS
		$$BEGIN RAISE WARNING 'user code ran: % %', TG_TABLE_NAME, TG_OP; RETURN NEW; END$$;
	CREATE TRIGGER people_loud BEFORE INSERT OR UPDATE ON people FOR EACH ROW EXECUTE FUNCTION app.loud();
	ALTER TABLE people ENABLE ALWAYS TRIGGER people_loud;
	INSERT INTO people VALUES (1, 'ada', 'happy'), (2, 'grace', 'sad');
	INSERT INTO pets VALUES (1, 1, 'rex'), (2, 2, 'tom');
	INSERT INTO counted (id) VALUES (1), (2);
	INSERT INTO gone VALUES (1);
	REFRESH MATERIALIZED VIEW pet_names});
my $a = commit_root();
my $people = oid_of('app.people');
my $counted = oid_of('app.counted');

# The second: a column added to pets, which the foreign key and the view's base keep; a table
# dropped and another made; the enum given a value, which a kept table's row then has; the
# function a default calls changed; the trigger changed; and rows changed.
sql(q{SET ROLE app_owner; SET search_path = app;
	ALTER TYPE mood ADD VALUE 'calm' BEFORE 'happy'});
sql(q{SET ROLE app_owner; SET search_path = app;
	ALTER TABLE pets ADD COLUMN legs integer;
	DROP TABLE gone;
	CREATE TABLE fresh (id integer PRIMARY KEY, what text);
	INSERT INTO fresh VALUES (1, 'new');
	CREATE OR REPLACE FUNCTION app.start() RETURNS integer LANGUAGE sql IMMUTABLE AS 'SELECT 8';
	DROP TRIGGER people_loud ON people;
	CREATE TRIGGER people_loud AFTER INSERT OR UPDATE ON people FOR EACH ROW EXECUTE FUNCTION app.loud();
	ALTER TABLE people ENABLE ALWAYS TRIGGER people_loud;
	UPDATE people SET feels = 'calm' WHERE id = 2;
	INSERT INTO people VALUES (3, 'katherine', 'calm');
	UPDATE pets SET legs = 4;
	INSERT INTO pets VALUES (3, 3, 'polly', 2);
	INSERT INTO counted (id) VALUES (3);
	REFRESH MATERIALIZED VIEW pet_names});
my $b = commit_root();
is($b, read_root(), 'the second root is what the tables hold');
my $log_at = -s $node->logfile;

# Back to the first, whose enum lacks a value: another type, so it's made again with the table using it.
my $stats = move_to($a, 'to the first');
like($stats, qr/ made 4 dropped 1$/, "people as its enum is made again, pets, gone, and counted as its default went with the function; fresh dropped: $stats");
isnt(oid_of('app.counted'), $counted, 'a table whose default used a function changed is made again');
isnt(oid_of('app.people'), $people, 'and one whose enum is');
is(sql(q{SELECT string_agg(name, ',' ORDER BY name) FROM app.pet_names}), 'rex,tom', 'a materialized view that held rows holds the new ones');
is(sql(q{SELECT string_agg(id || ':' || name, ',' ORDER BY id) FROM app.named}), '1:ada,2:grace', 'a view made again over the table');
is(sql(q{SELECT count(*) FROM pg_constraint WHERE conrelid = 'app.pets'::regclass AND contype = 'f'}), '1', 'and the foreign key');
is(sql(q{SELECT string_agg(e.enumlabel, ',' ORDER BY e.enumsortorder) FROM pg_enum e WHERE e.enumtypid = 'app.mood'::regtype}),
	'sad,happy', 'the enum as the target has it');

# To the second: the enum's value added first, so the type is the target's and the table using it is kept.
$people = oid_of('app.people');
$stats = move_to($b, 'to the second');
like($stats, qr/ made 3 dropped 1$/, "pets, fresh and counted made, gone dropped, people kept: $stats");
is(oid_of('app.people'), $people, 'a table using an enum given a value is kept, and only its rows written');
is(sql(q{SELECT string_agg(name || ':' || coalesce(legs::text, '-'), ',' ORDER BY id) FROM app.pets}), 'rex:4,tom:4,polly:2', 'a table made again holds the rows');
is(sql(q{SELECT tgtype & 2 FROM pg_trigger WHERE tgname = 'people_loud'}), '0', 'a trigger changed is made again as the target has it');
unlike(slurp_file($node->logfile, $log_at), qr/user code ran/, 'and no trigger ran');

# A move to where it is drops and makes nothing.
is(sql(qq{SELECT test_ws_move('app', '$b')}), 'tables 0 deleted 0 inserted 0 sequences 0 made 0 dropped 0', 'a move to where it is writes nothing');

# Rolled back, a move that changes definitions leaves all as it was.
sql(qq{BEGIN; SELECT test_ws_move('app', '$a'); ROLLBACK});
is(read_root(), $b, 'a move rolled back leaves the tables');
is(capture_root(), $b, 'and capture');

unlike(slurp_file($node->logfile), qr/^PANIC:|terminated by signal|capture can't apply/m, 'nothing crashed, and capture applied it all');
$node->stop;
done_testing();
