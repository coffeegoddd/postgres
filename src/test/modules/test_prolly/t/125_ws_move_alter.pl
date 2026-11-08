# Copyright (c) 2026, PostgreSQL Global Development Group

# A versioned schema moved in place to roots whose tables differ in what an ALTER can change: the
# tables are altered rather than made again, so they keep what they are (their object ids, and the
# views and foreign keys on them), and a row only the alter rewrote is left as it is.  A table whose
# change no ALTER here makes, such as a column retyped, is made again.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('move_alter');
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
sub oid_of { my ($rel) = @_; return sql(qq{SELECT '$rel'::regclass::oid}); }

sub indexes_check
{
	return sql(q{SELECT count(*) FROM (SELECT i.indexrelid FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid
		JOIN pg_namespace n ON n.oid = c.relnamespace JOIN pg_am a ON a.oid = c.relam
		WHERE n.nspname = 'app' AND a.amname = 'btree' AND c.relkind = 'i' OFFSET 0) x
		WHERE bt_index_check(x.indexrelid, true) IS NOT NULL});
}

sub move_to
{
	my ($root, $name) = @_;
	my $stats = sql(qq{SELECT test_ws_move('app', '$root')});
	is(read_root(), $root, "$name: the tables hold the root");
	is(capture_root(), $root, "$name: and capture has it");
	cmp_ok(indexes_check(), '>', 10, "$name: each index agrees with its table");
	return $stats;
}

$node->safe_psql('postgres', q{CREATE ROLE app_owner; CREATE ROLE reader;
	SELECT test_ws_role_mark(test_ws_role_bytes(ARRAY['app_owner', 'reader']))});
sql(q{CREATE SCHEMA app VERSIONED; GRANT ALL ON SCHEMA app TO app_owner; SET ROLE app_owner; SET search_path = app;
	CREATE TABLE al (id integer PRIMARY KEY, a text NOT NULL, c text, d integer UNIQUE,
		b0 integer DEFAULT 1 CONSTRAINT b_positive CHECK (b0 > 0)) WITH (fillfactor = 90);
	ALTER TABLE al RENAME COLUMN b0 TO b;
	CREATE INDEX al_c ON al (c);
	CREATE TABLE refs (id integer PRIMARY KEY, al_id integer REFERENCES al);
	CREATE VIEW al_ids AS SELECT id FROM al;
	CREATE TABLE retyped (id integer PRIMARY KEY, v integer);
	CREATE FUNCTION app.loud() RETURNS trigger LANGUAGE plpgsql AS
		$$BEGIN RAISE WARNING 'user code ran: % %', TG_TABLE_NAME, TG_OP; RETURN NULL; END$$;
	CREATE TRIGGER al_loud AFTER INSERT OR UPDATE OR DELETE ON al FOR EACH ROW EXECUTE FUNCTION app.loud();
	ALTER TABLE al ENABLE ALWAYS TRIGGER al_loud;
	INSERT INTO al SELECT g, 'a' || g, 'c' || g, g, CASE WHEN g <= 2 THEN g END FROM generate_series(1, 10) g;
	INSERT INTO refs VALUES (1, 1), (2, 2);
	INSERT INTO retyped VALUES (1, 1);
	CREATE VIEW vc AS SELECT c FROM al;
	COMMENT ON COLUMN al.c IS 'cee';
	GRANT SELECT (c) ON al TO reader;
	CREATE TABLE nul (id integer PRIMARY KEY, v text);
	INSERT INTO nul VALUES (1, 'one');
	CREATE TABLE fresh_dep (id integer PRIMARY KEY, v integer);
	INSERT INTO fresh_dep VALUES (1, 1);
	CREATE TABLE u (id integer PRIMARY KEY, code integer CONSTRAINT u_code UNIQUE);
	CREATE TABLE uref (id integer PRIMARY KEY, code integer REFERENCES u (code));
	INSERT INTO u VALUES (1, 10); INSERT INTO uref VALUES (1, 10);
	CREATE TABLE cmt (id integer PRIMARY KEY, v integer);
	CREATE INDEX cmt_i ON cmt (v);
	COMMENT ON INDEX cmt_i IS 'an index with a comment';
	CREATE TABLE gen (id integer PRIMARY KEY, x integer, y integer GENERATED ALWAYS AS (x * 2) STORED);
	INSERT INTO gen (id, x) VALUES (1, 3)});
my $a = commit_root();

# (b's tag is of the name it was made with, so a column added back as b has it only if it's set)

# The second: a column renamed, two others swapping names, the last dropped with its check and another
# added, a not-null constraint dropped and another set, a default, the key renamed, an index for
# another, a storage parameter; one row changed and one added.  The rows otherwise differ only by the
# column dropped and the one added, null in them.
sql(q{SET ROLE app_owner; SET search_path = app;
	ALTER TABLE al RENAME COLUMN a TO a2;
	ALTER TABLE al RENAME COLUMN c TO tmp;
	ALTER TABLE al RENAME COLUMN d TO c;
	ALTER TABLE al RENAME COLUMN tmp TO d;
	ALTER TABLE al DROP COLUMN b;
	ALTER TABLE al ADD COLUMN f text;
	ALTER TABLE al ALTER COLUMN a2 DROP NOT NULL;
	ALTER TABLE al ALTER COLUMN c SET NOT NULL;
	ALTER TABLE al ALTER COLUMN d SET DEFAULT 'none';
	ALTER TABLE al RENAME CONSTRAINT al_pkey TO al_key;
	DROP INDEX al_c;
	CREATE INDEX al_d ON al (d);
	ALTER TABLE al SET (fillfactor = 70);
	UPDATE al SET f = 'set' WHERE id = 3;
	INSERT INTO al (id, a2, c, d) VALUES (11, NULL, 11, 'd11');
	ALTER TABLE al RENAME CONSTRAINT al_id_not_null TO al_id_nn;
	ALTER TABLE al ADD CONSTRAINT id_positive CHECK (id > 0) NOT VALID;
	ALTER TABLE al SET (toast.autovacuum_enabled = false);
	DROP VIEW vc;
	CREATE VIEW vc AS SELECT c FROM al;
	ALTER TABLE retyped ALTER COLUMN v TYPE bigint;
	ALTER TABLE nul ADD COLUMN extra integer;
	CREATE SEQUENCE app.counting;
	CREATE FUNCTION app.small(bigint) RETURNS boolean LANGUAGE sql IMMUTABLE AS 'SELECT $1 < 1000';
	ALTER TABLE fresh_dep ADD COLUMN g bigint;
	ALTER TABLE fresh_dep ALTER COLUMN g SET DEFAULT nextval('app.counting');
	ALTER TABLE fresh_dep ADD CONSTRAINT g_small CHECK (app.small(g));
	INSERT INTO fresh_dep (id, v) VALUES (2, 2);
	ALTER TABLE u DROP CONSTRAINT u_code CASCADE;
	ALTER TABLE u ADD CONSTRAINT u_code UNIQUE NULLS NOT DISTINCT (code);
	ALTER TABLE uref ADD FOREIGN KEY (code) REFERENCES u (code);
	DROP INDEX cmt_i;
	CREATE INDEX cmt_i ON cmt (v) WHERE v > 0;
	COMMENT ON INDEX cmt_i IS 'an index with a comment';
	ALTER TABLE gen RENAME COLUMN x TO x2});
my $b = commit_root();
is($b, read_root(), 'the second root is what the tables hold');
my $al = oid_of('app.al');
my $view = oid_of('app.al_ids');
my $refs = oid_of('app.refs');
my $retyped = oid_of('app.retyped');
my %oids = map { $_ => oid_of("app.$_") } qw(nul fresh_dep u uref cmt gen);
my $log_at = -s $node->logfile;

# (where each row lies, which a row written again doesn't)
my $places = q{SELECT string_agg(id || ':' || ctid, ',' ORDER BY id) FROM app.al WHERE id BETWEEN 3 AND 10};
my $placed = sql($places);
my $stats = move_to($a, 'back to the first');
# (rows 4 to 10 differ only by columns null in them, which their bytes don't have, so they're not even
# read; row 3's f, dropped, is all it differs by, so it's read and left; 1 and 2 have a b, and 11 goes)
# (altered: al, nul, u and gen; made again: retyped, cmt, and fresh_dep, whose default and check
# went with the sequence and function the first root hasn't)
like($stats, qr/^tables 4 deleted 3 inserted 2 sequences 0 made 3 dropped 0 altered 4 kept 1$/,
	"al altered and its rows only the alter rewrote left as they are; retyped made again: $stats");
is(sql($places), $placed, 'those rows where they were, not written again');
is(oid_of('app.al'), $al, 'the altered table is the same table');
is(oid_of('app.al_ids'), $view, 'and a view on it the same view');
is(oid_of('app.refs'), $refs, 'and a table referring to it kept');
is(sql(q{SELECT count(*) FROM pg_constraint WHERE conrelid = 'app.refs'::regclass AND contype = 'f'}), '1', 'with its foreign key');
isnt(oid_of('app.retyped'), $retyped, 'a table with a column retyped is made again');
is(sql(q{SELECT string_agg(attname, ',' ORDER BY attnum) FROM pg_attribute WHERE attrelid = 'app.al'::regclass AND attnum > 0 AND NOT attisdropped}),
	'id,a,c,d,b', "its columns as the first root has them, the one dropped from it added at the end");

%oids = map { $_ => oid_of("app.$_") } qw(nul fresh_dep u uref cmt gen);
$stats = move_to($b, 'to the second');
is(oid_of('app.nul'), $oids{nul}, 'a table given a column and no other change is altered');
sql(q{INSERT INTO app.nul VALUES (2, 'two', 2)});
is(capture_root(), read_root(), 'and capture, told of its layout though its rows were not written, has what is written to it after');
is(oid_of('app.fresh_dep'), $oids{fresh_dep}, 'a column added with a default from a sequence the target adds, and a check of a function it adds: altered, those made first');
is(oid_of('app.u'), $oids{u}, 'a table whose unique constraint a foreign key needs is altered');
is(sql(q{SELECT count(*) FROM pg_constraint WHERE conrelid = 'app.uref'::regclass AND contype = 'f'}), '1', 'and the key is made again');
isnt(oid_of('app.cmt'), $oids{cmt}, 'an index made again with a comment on it has its table made again');
is(oid_of('app.gen'), $oids{gen}, 'a column a generated one reads renamed: altered');
is(sql(q{SELECT c FROM app.vc ORDER BY 1 LIMIT 1}), '1', 'a view that named one column, and names the other after the swap (by its tag, so it differs), is made again');
is(sql(q{SELECT col_description('app.al'::regclass, attnum) FROM pg_attribute WHERE attrelid = 'app.al'::regclass AND attname = 'd'}),
	'cee', 'a column renamed keeps its comment');
is(sql(q{SELECT has_column_privilege('reader', 'app.al', 'd', 'SELECT')}), 't', 'and its grant');
# (rows 1 and 2 differ only by b, dropped, so they're left; 3 has an f, and 11 comes back)
# (altered: al, nul, fresh_dep, u and gen, fresh_dep's new row put in and its sequence set; made again:
# retyped and cmt)
like($stats, qr/^tables 5 deleted 1 inserted 3 sequences 1 made 2 dropped 0 altered 5 kept 2$/, "and back again: $stats");
is(oid_of('app.al'), $al, 'the same table still');
unlike(slurp_file($node->logfile, $log_at), qr/user code ran/, 'and no trigger ran');

unlike(slurp_file($node->logfile), qr/^PANIC:|terminated by signal|capture can't apply/m, 'nothing crashed, and capture applied it all');
$node->stop;
done_testing();
