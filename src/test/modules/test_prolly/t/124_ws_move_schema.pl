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
	return sql(q{SELECT count(*) FROM (SELECT i.indexrelid FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid
		JOIN pg_namespace n ON n.oid = c.relnamespace JOIN pg_am a ON a.oid = c.relam
		WHERE n.nspname = 'app' AND a.amname = 'btree' AND c.relkind = 'i' OFFSET 0) x
		WHERE bt_index_check(x.indexrelid, true) IS NOT NULL});
}

# A move to a root, the enum step committed first; what was written, and checks it holds the root.
sub move_to
{
	my ($root, $name) = @_;
	sql(qq{SELECT test_ws_move_prepare('app', '$root')});
	my $stats = sql(qq{SELECT test_ws_move('app', '$root')});
	is(read_root(), $root, "$name: the tables hold the root");
	is(capture_root(), $root, "$name: and capture has it");
	cmp_ok(indexes_check(), '>', 10, "$name: each index agrees with its table");
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
	REFRESH MATERIALIZED VIEW pet_names;
	CREATE FUNCTION app.is_pos(integer) RETURNS boolean LANGUAGE sql IMMUTABLE AS 'SELECT $1 > 0';
	CREATE DOMAIN pos AS integer CHECK (app.is_pos(VALUE));
	CREATE TABLE dom_t (id integer PRIMARY KEY, v pos);
	CREATE TYPE inner_e AS ENUM ('x', 'y');
	CREATE TYPE empty_e AS ENUM ();
	CREATE TABLE uses_empty (id integer PRIMARY KEY, e empty_e);
	CREATE TYPE outer_t AS (a integer, b inner_e);
	CREATE TABLE comp (id integer PRIMARY KEY, o outer_t);
	CREATE AGGREGATE app.cnt(*) (sfunc = int8inc, stype = int8, initcond = '0');
	CREATE TABLE tick (id serial PRIMARY KEY, what text);
	CREATE SEQUENCE alone;
	CREATE TABLE pp (id integer, k integer, PRIMARY KEY (id, k)) PARTITION BY RANGE (k);
	CREATE TABLE pp_low PARTITION OF pp FOR VALUES FROM (0) TO (10);
	CREATE TABLE kp (id integer, k integer, PRIMARY KEY (id, k)) PARTITION BY RANGE (k);
	CREATE TABLE kp_low PARTITION OF kp FOR VALUES FROM (0) TO (10);
	CREATE TABLE base_t (id integer PRIMARY KEY, a text);
	CREATE TABLE child_t (b text) INHERITS (base_t);
	CREATE TABLE visits (id integer PRIMARY KEY, pet integer REFERENCES pets);
	CREATE MATERIALIZED VIEW mv_b AS SELECT id, name FROM pets;
	CREATE MATERIALIZED VIEW mv_a AS SELECT name FROM mv_b;
	CREATE INDEX mv_a_name ON mv_a (name);
	CREATE POLICY see_some ON people USING (id > 0);
	CREATE RULE visits_noop AS ON UPDATE TO visits DO ALSO NOTHING;
	RESET ROLE;
	CREATE TEXT SEARCH PARSER app.prs (START = prsd_start, GETTOKEN = prsd_nexttoken, END = prsd_end, LEXTYPES = prsd_lextype);
	SET ROLE app_owner;
	CREATE TEXT SEARCH CONFIGURATION app.cfg (PARSER = app.prs);
	ALTER TEXT SEARCH CONFIGURATION app.cfg ADD MAPPING FOR word WITH simple;
	CREATE FUNCTION app.quiet() RETURNS trigger LANGUAGE plpgsql AS
		$$BEGIN RAISE WARNING 'user code ran: % %', TG_TABLE_NAME, TG_OP; RETURN NULL; END$$;
	CREATE TABLE logged (id integer PRIMARY KEY, v text);
	CREATE TRIGGER logged_loud AFTER INSERT OR UPDATE OR DELETE ON logged FOR EACH ROW EXECUTE FUNCTION app.quiet();
	ALTER TABLE logged ENABLE ALWAYS TRIGGER logged_loud;
	INSERT INTO dom_t VALUES (1, 5);
	INSERT INTO comp VALUES (1, ROW(1, 'y'));
	INSERT INTO tick (what) VALUES ('a'), ('b');
	SELECT nextval('alone');
	INSERT INTO pp VALUES (1, 1);
	INSERT INTO kp VALUES (1, 1);
	INSERT INTO base_t VALUES (1, 'b');
	INSERT INTO child_t VALUES (2, 'c', 'cb');
	INSERT INTO visits VALUES (1, 1);
	INSERT INTO logged VALUES (1, 'one');
	REFRESH MATERIALIZED VIEW mv_b;
	REFRESH MATERIALIZED VIEW mv_a});
my $a = commit_root();
my $people = oid_of('app.people');
my $counted = oid_of('app.counted');
my $visits = oid_of('app.visits');
my $logged = oid_of('app.logged');
my $dom_t = oid_of('app.dom_t');
my $comp = oid_of('app.comp');

# The second: a column added to pets, which the foreign key and the view's base keep; a table
# dropped and another made; the enum given a value, which a kept table's row then has; the
# function a default calls changed; the trigger changed; and rows changed.
sql(q{SET ROLE app_owner; SET search_path = app;
	ALTER TYPE mood ADD VALUE 'calm' BEFORE 'happy';
	ALTER TYPE mood ADD VALUE 'meh' BEFORE 'sad';
	ALTER TYPE mood ADD VALUE 'joy' AFTER 'happy';
	ALTER TYPE mood ADD VALUE 'ecstatic' AFTER 'joy';
	ALTER TYPE inner_e ADD VALUE 'z';
	ALTER TYPE empty_e ADD VALUE 'first';
	ALTER TYPE empty_e ADD VALUE 'second'});
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
	REFRESH MATERIALIZED VIEW pet_names;
	CREATE OR REPLACE FUNCTION app.is_pos(integer) RETURNS boolean LANGUAGE sql IMMUTABLE AS 'SELECT $1 >= 1';
	DROP AGGREGATE app.cnt(*);
	CREATE AGGREGATE app.cnt(*) (sfunc = int8inc, stype = int8, initcond = '1');
	ALTER TABLE tick ADD COLUMN extra integer;
	INSERT INTO tick (what) VALUES ('c');
	SELECT nextval('alone');
	CREATE TABLE pp_high PARTITION OF pp FOR VALUES FROM (10) TO (20);
	INSERT INTO pp VALUES (2, 15);
	ALTER TABLE kp ADD COLUMN extra integer;
	ALTER TABLE child_t ADD COLUMN c2 integer;
	INSERT INTO visits VALUES (2, 3);
	DROP POLICY see_some ON people;
	CREATE POLICY see_some ON people USING (id > 1);
	DROP RULE visits_noop ON visits;
	CREATE RULE visits_noop AS ON DELETE TO visits DO ALSO NOTHING;
	COMMENT ON FUNCTION app.loud() IS 'loud';
	RESET ROLE;
	COMMENT ON TEXT SEARCH PARSER app.prs IS 'changed';
	SET ROLE app_owner;
	ALTER TEXT SEARCH CONFIGURATION app.cfg ADD MAPPING FOR numword WITH simple;
	UPDATE logged SET v = 'two';
	INSERT INTO uses_empty VALUES (1, 'second');
	REFRESH MATERIALIZED VIEW mv_b;
	REFRESH MATERIALIZED VIEW mv_a});
my $b = commit_root();
is($b, read_root(), 'the second root is what the tables hold');
my $log_at = -s $node->logfile;

# Back to the first, whose enum lacks a value: another type, so it's made again with the table using it.
my $stats = move_to($a, 'to the first');
# (made: people as its enum lost values, counted, gone, dom_t, comp, uses_empty, kp and its partition,
# and child_t; dropped: fresh and pp_high; altered: pets and tick, each dropping a column, pets' rows
# whose dropped column was all that differed left as they are)
like($stats, qr/ made 9 dropped 2 altered 2 kept 2$/, "what differs is made, dropped and altered: $stats");
isnt(oid_of('app.counted'), $counted, 'a table whose default used a function changed is made again');
isnt(oid_of('app.people'), $people, 'and one whose enum is');
isnt(oid_of('app.dom_t'), $dom_t, 'a table whose domain lost its check to a function changed is made again with it');
isnt(oid_of('app.comp'), $comp, 'and one whose composite type lost an attribute');
is(sql(q{SELECT count(*) FROM pg_constraint WHERE contypid = 'app.pos'::regtype}), '1', 'and the domain with its check');
is(sql(q{SELECT count(*) FROM pg_attribute WHERE attrelid = 'app.outer_t'::regclass AND attnum > 0 AND NOT attisdropped}), '2',
	"a composite type that lost an attribute to an enum made again is made again");
is(oid_of('app.visits'), $visits, 'a table referring to one made again is kept');
is(sql(q{SELECT count(*) FROM pg_constraint WHERE conrelid = 'app.visits'::regclass AND contype = 'f'}), '1', 'with its foreign key');
is(oid_of('app.logged'), $logged, 'a table with a trigger, its rows written, is kept');
is(sql(q{SELECT string_agg(name, ',' ORDER BY name) FROM app.pet_names}), 'rex,tom', 'a materialized view that held rows holds the new ones');
is(sql(q{SELECT string_agg(name, ',' ORDER BY name) FROM app.mv_a}), 'rex,tom', 'and one that reads another, after it');
is(sql(q{SELECT string_agg(id || ':' || name, ',' ORDER BY id) FROM app.named}), '1:ada,2:grace', 'a view made again over the table');
is(sql(q{SELECT count(*) FROM pg_constraint WHERE conrelid = 'app.pets'::regclass AND contype = 'f'}), '1', 'and the foreign key');
is(sql(q{SELECT string_agg(e.enumlabel, ',' ORDER BY e.enumsortorder) FROM pg_enum e WHERE e.enumtypid = 'app.mood'::regtype}),
	'sad,happy', 'the enum as the target has it');
is(sql(q{SELECT (SELECT last_value FROM app.alone) || ' ' || (SELECT last_value FROM app.tick_id_seq)}), '1 2', 'sequences where the target has them');

# To the second: the enum's values added first, so the type is the target's and the table using it is kept.
$people = oid_of('app.people');
my $uses_empty = oid_of('app.uses_empty');
my $pp = oid_of('app.pp');
my $kp = oid_of('app.kp');
$stats = move_to($b, 'to the second');
# (made: fresh, counted, dom_t, kp and its partition, child_t and pp_high; dropped: gone; altered: pets
# and tick, each adding a column, every row of pets given a value in it)
like($stats, qr/ made 7 dropped 1 altered 2 kept 0$/, "what differs is made, dropped and altered: $stats");
is(oid_of('app.people'), $people, 'a table using an enum given values is kept, and only its rows written');
is(sql(q{SELECT string_agg(e.enumlabel, ',' ORDER BY e.enumsortorder) FROM pg_enum e WHERE e.enumtypid = 'app.mood'::regtype}),
	'meh,sad,calm,happy,joy,ecstatic', 'with values added first, between and last');
is(oid_of('app.uses_empty'), $uses_empty, 'and one using an enum that had no values');
is(oid_of('app.pp'), $pp, 'a partitioned table given a partition is kept');
is(sql(q{SELECT tableoid::regclass FROM app.pp WHERE id = 2}), 'app.pp_high', 'with the partition made and its rows');
isnt(oid_of('app.kp'), $kp, 'and one whose definition changed is made again, with its partitions');
is(sql(q{SELECT string_agg(name || ':' || coalesce(legs::text, '-'), ',' ORDER BY id) FROM app.pets}), 'rex:4,tom:4,polly:2', 'a table made again holds the rows');
is(sql(q{SELECT tgtype & 2 FROM pg_trigger WHERE tgname = 'people_loud'}), '0', 'a trigger changed is made again as the target has it');
is(sql(q{SELECT (SELECT last_value FROM app.alone) || ' ' || (SELECT last_value FROM app.tick_id_seq)}), '2 3', 'sequences where the target has them');
unlike(slurp_file($node->logfile, $log_at), qr/user code ran/, 'and no trigger ran, a kept one on a table whose rows were written either');

# A move to where it is drops, makes and refreshes nothing.
my $mv_file = sql(q{SELECT pg_relation_filenode('app.mv_a')});
is(sql(qq{SELECT test_ws_move('app', '$b')}), 'tables 0 deleted 0 inserted 0 sequences 0 made 0 dropped 0 altered 0 kept 0', 'a move to where it is writes nothing');
is(sql(q{SELECT pg_relation_filenode('app.mv_a')}), $mv_file, 'not even a materialized view');

# Rolled back, a move that changes definitions leaves all as it was.
sql(qq{BEGIN; SELECT test_ws_move('app', '$a'); ROLLBACK});
is(read_root(), $b, 'a move rolled back leaves the tables');
is(capture_root(), $b, 'and capture');

unlike(slurp_file($node->logfile), qr/^PANIC:|terminated by signal|capture can't apply/m, 'nothing crashed, and capture applied it all');
$node->stop;
done_testing();
