# Copyright (c) 2026, PostgreSQL Global Development Group

# A schema as a root value and back: what is restored captures as the root it came
# from.  And a dry run of a root says what Postgres refuses of it, and leaves nothing.

use strict;
use feature 'state';
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('root');
$node->init;
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_prolly');
my $branch = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
$ENV{PGOPTIONS} = "-c catalog_branch=$branch";
$node->safe_psql('postgres', q{CREATE ROLE g_root NOLOGIN TRAVELS; CREATE ROLE g_reader NOLOGIN TRAVELS});

my $everything = q{
	CREATE TYPE mood AS ENUM ('sad', 'ok');
	CREATE DOMAIN pos AS integer CHECK (VALUE > 0);
	CREATE TYPE pair AS (x integer, y text);
	CREATE FUNCTION twice(integer) RETURNS integer LANGUAGE sql IMMUTABLE AS 'SELECT $1 * 2';
	CREATE SEQUENCE seq1 START 10;
	SELECT nextval('seq1');

	CREATE TABLE parent_t (id integer PRIMARY KEY, name text NOT NULL UNIQUE, m mood, p pos, q pair,
		g integer GENERATED ALWAYS AS (twice(id)) STORED, v integer GENERATED ALWAYS AS (id + 1) VIRTUAL,
		d integer DEFAULT nextval('seq1'), big text, CHECK (id > 0));
	INSERT INTO parent_t (id, name, m, p, q, big) VALUES
		(1, 'one', 'sad', 5, ROW(1, 'a'), repeat('x', 5000)), (2, 'two', 'ok', 6, ROW(2, 'b'), NULL);
	ALTER TABLE parent_t ADD COLUMN gone integer; ALTER TABLE parent_t DROP COLUMN gone;
	CREATE INDEX parent_t_lower ON parent_t (lower(name)) WHERE id > 0;
	ALTER TABLE parent_t ADD CONSTRAINT late CHECK (id < 1000) NOT VALID;
	ALTER TABLE parent_t SET (fillfactor = 70);
	ALTER TABLE parent_t ALTER COLUMN big SET STORAGE EXTERNAL;
	COMMENT ON TABLE parent_t IS 'the parent';
	COMMENT ON COLUMN parent_t.name IS 'its name';
	GRANT SELECT ON parent_t TO g_reader;

	CREATE TABLE child_t (id integer PRIMARY KEY, parent integer REFERENCES parent_t (id) ON DELETE CASCADE, note text);
	INSERT INTO child_t VALUES (10, 1, 'n1'), (11, 2, NULL);
	CREATE TABLE mutual_a (id integer PRIMARY KEY, b integer);
	CREATE TABLE mutual_b (id integer PRIMARY KEY, a integer REFERENCES mutual_a (id));
	ALTER TABLE mutual_a ADD FOREIGN KEY (b) REFERENCES mutual_b (id);

	CREATE TABLE keyless (a integer, b text);
	INSERT INTO keyless VALUES (1, 'x'), (1, 'x'), (NULL, NULL);
	CREATE TABLE ident (id integer GENERATED ALWAYS AS IDENTITY PRIMARY KEY, x text);
	INSERT INTO ident (x) VALUES ('a'), ('b');

	CREATE TABLE ranged (id integer, r integer, v text, PRIMARY KEY (id, r)) PARTITION BY RANGE (r);
	CREATE TABLE r1 PARTITION OF ranged FOR VALUES FROM (0) TO (10);
	CREATE TABLE r2 PARTITION OF ranged FOR VALUES FROM (10) TO (20) PARTITION BY LIST (id);
	CREATE TABLE r2a PARTITION OF r2 FOR VALUES IN (1, 2);
	CREATE TABLE r2b PARTITION OF r2 DEFAULT;
	CREATE TABLE rd PARTITION OF ranged DEFAULT;
	INSERT INTO ranged VALUES (1, 5, 'a'), (1, 15, 'b'), (9, 15, 'c'), (3, 100, 'd');
	CREATE TABLE refers (id integer PRIMARY KEY, a integer, b integer, FOREIGN KEY (a, b) REFERENCES ranged (id, r));

	CREATE TABLE base_t (a integer CHECK (a > 0), b text);
	CREATE TABLE heir_t (c integer) INHERITS (base_t);
	INSERT INTO base_t VALUES (1, 'p'); INSERT INTO heir_t VALUES (2, 'c', 3);

	CREATE VIEW v1 AS SELECT id, name FROM parent_t WHERE id > 0;
	CREATE VIEW v2 AS SELECT p.id, c.note FROM parent_t p JOIN child_t c ON c.parent = p.id;
	CREATE MATERIALIZED VIEW mv1 AS SELECT id FROM parent_t;
	CREATE INDEX mv1_id ON mv1 (id);
	CREATE FUNCTION seen() RETURNS trigger LANGUAGE plpgsql AS 'BEGIN RETURN NEW; END';
	CREATE TRIGGER tr1 BEFORE UPDATE OF name ON parent_t FOR EACH ROW WHEN (OLD.name <> NEW.name) EXECUTE FUNCTION seen();
	CREATE RULE rl1 AS ON INSERT TO v1 DO INSTEAD NOTHING;
	ALTER TABLE child_t ENABLE ROW LEVEL SECURITY;
	CREATE POLICY pl1 ON child_t USING (note IS NOT NULL);
	CREATE FUNCTION atomic_f() RETURNS bigint LANGUAGE sql BEGIN ATOMIC SELECT count(*) FROM parent_t WHERE id > 0; END;
	ALTER SEQUENCE seq1 OWNED BY parent_t.d;
};

sub in_one_session
{
	my ($script) = @_;
	return $node->psql('postgres', $script);
}

my ($rc, $out, $err) = in_one_session(qq{
	SET client_min_messages = warning;
	CREATE SCHEMA s VERSIONED; ALTER SCHEMA s OWNER TO g_root; CREATE SCHEMA d VERSIONED; ALTER SCHEMA d OWNER TO g_root;
	SET ROLE g_root; SET search_path = s;
	$everything
	RESET ROLE; SET search_path = public;
	SELECT test_ws_root_capture('s') AS r \\gset
	\\echo =====captured
	\\echo :r
	SELECT test_ws_root_restore(:'r', 'd');
	SELECT test_ws_root_capture('d') AS r2 \\gset
	\\echo =====again
	\\echo :r2
	\\echo =====diff
	SELECT test_ws_diff(:'r', :'r2');
	\\echo =====end
});
is($rc, 0, 'it all runs') or diag($err);
my ($r1) = $out =~ /=====captured\n(\S+)/;
my ($r2) = $out =~ /=====again\n(\S+)/;
my ($diff) = $out =~ /=====diff\n(.*?)\n?=====end/s;
my @changed = grep { !/^data \S+ same$/ && !/^rows \S+ \+0 -0 ~0$/ && $_ ne '' } split /\n/, $diff // '';
is("@changed", '', 'restored, it differs from its root in nothing') or diag($diff // '');
is($r2, $r1, 'and is that root');

# what it holds is really there: its rows, read through the parts that hold them
is($node->safe_psql('postgres', q{SELECT id, name, m, p, q, g, v, length(big) FROM d.parent_t ORDER BY id}),
	"1|one|sad|5|(1,a)|2|2|5000\n2|two|ok|6|(2,b)|4|3|", 'a table of many kinds of column');
is($node->safe_psql('postgres', q{SELECT tableoid::regclass::text, * FROM d.ranged ORDER BY id, r}),
	"d.r1|1|5|a\nd.r2a|1|15|b\nd.rd|3|100|d\nd.r2b|9|15|c", 'each row in its partition');
is($node->safe_psql('postgres', q{SELECT count(*) FROM d.keyless}), '3', 'keyless rows, each');
is($node->safe_psql('postgres', q{SELECT a FROM d.base_t ORDER BY a}), "1\n2", 'an heir read through its parent');
is($node->safe_psql('postgres', q{SELECT nextval('d.seq1') = nextval('s.seq1')}), 't', 'a sequence where it was');
is($node->safe_psql('postgres', q{SET enable_seqscan = off; SELECT id FROM d.parent_t WHERE lower(name) = 'two'}), '2', 'an index, built');
is($node->safe_psql('postgres', q{SELECT relowner::regrole FROM pg_class WHERE oid = 'd.parent_t'::regclass}), 'g_root', 'its owner');
is($node->safe_psql('postgres', q{SELECT has_table_privilege('g_reader', 'd.parent_t', 'SELECT')}), 't', 'its grant');
is($node->safe_psql('postgres', q{SELECT relrowsecurity FROM pg_class WHERE oid = 'd.child_t'::regclass}), 't', 'its row security');

($rc, $out, $err) = $node->psql('postgres', q{INSERT INTO d.child_t VALUES (99, 12345, 'x')});
like($err, qr/foreign key/, 'its foreign keys hold');
($rc, $out, $err) = $node->psql('postgres', q{INSERT INTO d.ranged VALUES (5, 5, 'x'), (5, 5, 'x')});
like($err, qr/duplicate key/, 'its key holds across partitions');

# a schema with something in it is refused, whatever it would leave
($rc, $out, $err) = in_one_session(q{SELECT test_ws_root_capture('s') AS r \gset
	SELECT test_ws_root_restore(:'r', 'd')});
like($err, qr/schema "d" holds something/, 'a schema holding something is refused');

# an empty root, and a schema holding only a table
($rc, $out, $err) = in_one_session(q{
	SET client_min_messages = warning;
	CREATE SCHEMA e1 VERSIONED; CREATE SCHEMA e2 VERSIONED;
	SELECT test_ws_root_capture('e1') AS r \gset
	SELECT test_ws_root_restore(:'r', 'e2');
	SELECT test_ws_root_capture('e2') = :'r';
});
is($rc, 0, 'an empty root restores') or diag($err);
like($out, qr/^t$/m, 'as itself');

# as a role that may
($rc, $out, $err) = in_one_session(q{
	SET client_min_messages = warning;
	CREATE SCHEMA o1 VERSIONED; ALTER SCHEMA o1 OWNER TO g_root;
	CREATE SCHEMA o2 VERSIONED; ALTER SCHEMA o2 OWNER TO g_root;
	SET ROLE g_root;
	CREATE TABLE o1.t (a integer); INSERT INTO o1.t VALUES (1);
	CREATE VIEW o1.vv AS SELECT a FROM o1.t;
	SELECT test_ws_root_capture('o1') AS r \gset
	SELECT test_ws_root_restore(:'r', 'o2');
	SELECT test_ws_root_capture('o2') = :'r';
});
is($rc, 0, 'its owner may restore it') or diag($err);
like($out, qr/^t$/m, 'as itself, as that role');

# a dry run: the merged root Postgres would not have, and one it would
for my $case (
	[ 'a key to columns no longer unique', 'CREATE TABLE p (u integer CONSTRAINT p_u UNIQUE); CREATE TABLE c (a integer)',
		'ALTER TABLE c ADD CONSTRAINT f FOREIGN KEY (a) REFERENCES p (u)', 'ALTER TABLE p DROP CONSTRAINT p_u',
		qr/no unique constraint matching/ ],
	[ 'a key of a type no longer alike', 'CREATE TABLE p (u integer PRIMARY KEY); CREATE TABLE c (a integer)',
		'ALTER TABLE c ADD CONSTRAINT f FOREIGN KEY (a) REFERENCES p (u)', 'ALTER TABLE c ALTER COLUMN a TYPE text',
		qr/cannot be implemented/ ],
	[ 'a child without its table\'s check', 'CREATE TABLE p (a integer)',
		'CREATE TABLE c () INHERITS (p)', 'ALTER TABLE p ADD CONSTRAINT k CHECK (a > 0)',
		qr/child table is missing constraint/ ],
	[ 'partitions overlapping', 'CREATE TABLE p (a integer) PARTITION BY RANGE (a)',
		'CREATE TABLE p1 PARTITION OF p FOR VALUES FROM (0) TO (10)', 'CREATE TABLE p2 PARTITION OF p FOR VALUES FROM (5) TO (15)',
		qr/would overlap/ ],
	[ 'a clean merge', 'CREATE TABLE p (a integer)',
		'ALTER TABLE p ADD COLUMN b integer', 'ALTER TABLE p ADD COLUMN c integer', undef ])
{
	my ($name, $base, $ours, $theirs, $expect) = @$case;
	state $i = 0;
	$i++;
	($rc, $out, $err) = in_one_session(qq{
	SET client_min_messages = warning;
	CREATE SCHEMA b$i VERSIONED; ALTER SCHEMA b$i OWNER TO g_root;
	CREATE SCHEMA o_$i VERSIONED; ALTER SCHEMA o_$i OWNER TO g_root;
	CREATE SCHEMA t$i VERSIONED; ALTER SCHEMA t$i OWNER TO g_root;
	SET ROLE g_root;
	SET search_path = b$i; $base;
	SET search_path = o_$i; $base; $ours;
	SET search_path = t$i; $base; $theirs;
	RESET ROLE; SET search_path = public;
	SELECT test_ws_root_capture('b$i') AS rb \\gset
	SELECT test_ws_root_capture('o_$i') AS ro \\gset
	SELECT test_ws_root_capture('t$i') AS rt \\gset
	SELECT split_part(test_ws_merge(:'rb', :'ro', :'rt'), E'\\n', 1) AS m \\gset
	\\echo =====refused
	SELECT coalesce(test_ws_root_refused(:'m'), 'nothing');
	\\echo =====left
	SELECT count(*) FROM pg_namespace WHERE nspname LIKE 'ws_dry_run%';
	\\echo =====end
	});
	is($rc, 0, "$name: runs") or diag($err);
	my ($refused) = $out =~ /=====refused\n(.*?)\n=====left/s;
	if (defined $expect) { like($refused // '', $expect, "$name: refused, saying so"); }
	else { is($refused // '', 'nothing', "$name: goes through"); }
	my ($left) = $out =~ /=====left\n(.*?)\n=====end/s;
	is($left // '', '0', "$name: and leaves nothing");
}

$node->stop;
done_testing();
