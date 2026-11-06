# Copyright (c) 2026, PostgreSQL Global Development Group

# One of every object kind a versioned schema can hold: captured, restored into an empty
# schema of its name on another branch, and the same by pg_dump, object by object.  A kind
# a commit comes to hold that this schema does not fails here.  So too the root is the same
# whatever a session had set when it captured it.

use strict;
use warnings FATAL => 'all';
use File::Basename;
use IPC::Run;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('coverage');
$node->init;
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_prolly');
$node->safe_psql('postgres', 'CREATE ROLE g_cov NOLOGIN TRAVELS; CREATE ROLE g_reader NOLOGIN TRAVELS');
my $from = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
my $to = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');

# what only a superuser can make, and then its owner's
my $superuser = q{
	CREATE TEXT SEARCH PARSER prs (START = prsd_start, GETTOKEN = prsd_nexttoken, END = prsd_end, LEXTYPES = prsd_lextype, HEADLINE = prsd_headline);
	CREATE TEXT SEARCH TEMPLATE tmp (INIT = dsimple_init, LEXIZE = dsimple_lexize);
	CREATE TYPE m AS ENUM ('a', 'b');
	ALTER TYPE m OWNER TO g_cov;
	CREATE FUNCTION mlt(m, m) RETURNS boolean IMMUTABLE LANGUAGE sql AS 'SELECT $1::text < $2::text';
	CREATE FUNCTION mle(m, m) RETURNS boolean IMMUTABLE LANGUAGE sql AS 'SELECT $1::text <= $2::text';
	CREATE FUNCTION meq(m, m) RETURNS boolean IMMUTABLE LANGUAGE sql AS 'SELECT $1::text = $2::text';
	CREATE FUNCTION mge(m, m) RETURNS boolean IMMUTABLE LANGUAGE sql AS 'SELECT $1::text >= $2::text';
	CREATE FUNCTION mgt(m, m) RETURNS boolean IMMUTABLE LANGUAGE sql AS 'SELECT $1::text > $2::text';
	CREATE FUNCTION mcmp(m, m) RETURNS integer IMMUTABLE LANGUAGE sql AS 'SELECT CASE WHEN $1::text < $2::text THEN -1 WHEN $1::text > $2::text THEN 1 ELSE 0 END';
	CREATE FUNCTION mcmp2(m, integer) RETURNS integer IMMUTABLE LANGUAGE sql AS 'SELECT 0';
	ALTER FUNCTION mlt(m, m) OWNER TO g_cov; ALTER FUNCTION mle(m, m) OWNER TO g_cov; ALTER FUNCTION meq(m, m) OWNER TO g_cov;
	ALTER FUNCTION mge(m, m) OWNER TO g_cov; ALTER FUNCTION mgt(m, m) OWNER TO g_cov; ALTER FUNCTION mcmp(m, m) OWNER TO g_cov; ALTER FUNCTION mcmp2(m, integer) OWNER TO g_cov;
	CREATE OPERATOR <<< (LEFTARG = m, RIGHTARG = m, FUNCTION = mlt, COMMUTATOR = >>>, NEGATOR = >>==, RESTRICT = scalarltsel, JOIN = scalarltjoinsel);
	CREATE OPERATOR <<== (LEFTARG = m, RIGHTARG = m, FUNCTION = mle, COMMUTATOR = >>==, NEGATOR = >>>);
	CREATE OPERATOR === (LEFTARG = m, RIGHTARG = m, FUNCTION = meq, COMMUTATOR = ===, HASHES = false, MERGES = false);
	CREATE OPERATOR >>== (LEFTARG = m, RIGHTARG = m, FUNCTION = mge, COMMUTATOR = <<==, NEGATOR = <<<);
	CREATE OPERATOR >>> (LEFTARG = m, RIGHTARG = m, FUNCTION = mgt, COMMUTATOR = <<<, NEGATOR = <<==);
	ALTER OPERATOR <<< (m, m) OWNER TO g_cov; ALTER OPERATOR <<== (m, m) OWNER TO g_cov; ALTER OPERATOR === (m, m) OWNER TO g_cov;
	ALTER OPERATOR >>== (m, m) OWNER TO g_cov; ALTER OPERATOR >>> (m, m) OWNER TO g_cov;
	CREATE OPERATOR FAMILY fam USING btree;
	ALTER OPERATOR FAMILY fam USING btree OWNER TO g_cov;
	CREATE OPERATOR CLASS in_fam FOR TYPE m USING btree FAMILY fam AS
		OPERATOR 1 <<<, OPERATOR 2 <<==, OPERATOR 3 ===, OPERATOR 4 >>==, OPERATOR 5 >>>, FUNCTION 1 mcmp(m, m);
	ALTER OPERATOR CLASS in_fam USING btree OWNER TO g_cov;
	ALTER OPERATOR FAMILY fam USING btree ADD FUNCTION 1 (m, integer) mcmp2(m, integer);
};

# all else, as its owner
my $owner = q{
	CREATE TYPE point2 AS (x integer, y integer);
	CREATE TYPE span AS RANGE (subtype = integer);
	CREATE DOMAIN positive AS integer NOT NULL CONSTRAINT pos CHECK (VALUE > 0);
	COMMENT ON DOMAIN positive IS 'on a domain';
	CREATE COLLATION mine (PROVIDER = libc, LC_COLLATE = 'C', LC_CTYPE = 'C');
	CREATE CONVERSION conv FOR 'LATIN1' TO 'UTF8' FROM iso8859_1_to_utf8;
	CREATE FUNCTION twice(integer) RETURNS integer IMMUTABLE LANGUAGE sql AS 'SELECT $1 * 2';
	CREATE FUNCTION atomic_f(integer) RETURNS integer LANGUAGE sql BEGIN ATOMIC SELECT $1 + 1; END;
	CREATE FUNCTION definer() RETURNS text SECURITY DEFINER SET work_mem = '1MB' LANGUAGE sql AS 'SELECT current_user::text';
	REVOKE ALL ON FUNCTION definer() FROM PUBLIC;
	GRANT EXECUTE ON FUNCTION definer() TO g_reader;
	CREATE PROCEDURE proc(integer) LANGUAGE sql AS 'SELECT $1';
	CREATE FUNCTION sfn(integer, integer) RETURNS integer IMMUTABLE LANGUAGE sql AS 'SELECT $1 + $2';
	CREATE AGGREGATE total(integer) (SFUNC = sfn, STYPE = integer, INITCOND = '0');
	CREATE FUNCTION text_of(point2) RETURNS text IMMUTABLE LANGUAGE sql AS 'SELECT ($1).x::text';
	CREATE CAST (point2 AS text) WITH FUNCTION text_of(point2);
	CREATE SEQUENCE seq START 5 INCREMENT 2 CACHE 3;
	SELECT nextval('seq');
	CREATE TEXT SEARCH DICTIONARY dict (TEMPLATE = tmp, stopwords = 'english', accept = false);
	CREATE TEXT SEARCH CONFIGURATION cfg (PARSER = prs);
	ALTER TEXT SEARCH CONFIGURATION cfg ADD MAPPING FOR asciiword, word WITH dict;

	CREATE TABLE t (
		id integer GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
		n integer DEFAULT 1 CONSTRAINT n_nn NOT NULL,
		v text COLLATE mine,
		m m,
		p point2,
		r span,
		d positive,
		g integer GENERATED ALWAYS AS (twice(n)) STORED,
		vg integer GENERATED ALWAYS AS (n + 1) VIRTUAL,
		s bigint DEFAULT nextval('seq'),
		CONSTRAINT n_pos CHECK (n > 0),
		CONSTRAINT v_one UNIQUE (v),
		CONSTRAINT n_ex EXCLUDE USING btree (s WITH =)
	) WITH (fillfactor = 80);
	ALTER TABLE t ALTER COLUMN v SET STORAGE EXTERNAL;
	ALTER TABLE t ALTER COLUMN n SET STATISTICS 50;
	ALTER TABLE t ADD CONSTRAINT w_nv CHECK (n < 1000) NOT VALID;
	CREATE INDEX t_expr ON t (lower(v)) WHERE n > 0;
	CREATE INDEX t_ops ON t (m in_fam);
	ALTER TABLE t REPLICA IDENTITY USING INDEX t_pkey;
	ALTER TABLE t CLUSTER ON t_pkey;
	COMMENT ON TABLE t IS 'on a table';
	COMMENT ON COLUMN t.v IS 'on a column';
	COMMENT ON CONSTRAINT n_pos ON t IS 'on a check';
	COMMENT ON INDEX t_expr IS 'on an index';
	GRANT SELECT ON t TO g_reader;
	GRANT UPDATE (v) ON t TO g_reader;
	INSERT INTO t (n, v, m, p, r, d) VALUES (1, 'a', 'a', ROW(1, 2), '[1,2)', 1);
	CREATE STATISTICS st (ndistinct) ON n, s FROM t;

	CREATE TABLE child (id integer PRIMARY KEY, tid integer, CONSTRAINT to_t FOREIGN KEY (tid) REFERENCES t (id) ON DELETE SET NULL DEFERRABLE);
	COMMENT ON TABLE child IS 'with a key';

	CREATE TABLE ranged (id integer, r integer, v text, PRIMARY KEY (id, r)) PARTITION BY RANGE (r);
	CREATE TABLE r1 PARTITION OF ranged FOR VALUES FROM (0) TO (10);
	CREATE TABLE r2 PARTITION OF ranged FOR VALUES FROM (10) TO (20) PARTITION BY LIST (id);
	CREATE TABLE r2a PARTITION OF r2 FOR VALUES IN (1, 2);
	CREATE TABLE rd PARTITION OF ranged DEFAULT;
	INSERT INTO ranged VALUES (1, 5, 'a'), (1, 15, 'b');

	CREATE TABLE base_t (a integer CHECK (a > 0), b text);
	CREATE TABLE heir_t (c integer) INHERITS (base_t);
	CREATE TABLE keyless (a integer, b text UNIQUE);
	INSERT INTO keyless VALUES (1, 'x');

	CREATE TABLE printed (
		id integer PRIMARY KEY,
		d date DEFAULT '2020-01-02' CONSTRAINT d_after CHECK (d > '2000-01-02'),
		ts timestamptz DEFAULT '2020-01-02 03:04:05+00',
		i interval DEFAULT '1 day 2 hours',
		f double precision DEFAULT 0.1,
		b bytea DEFAULT '\\x0102',
		a text[] DEFAULT '{a,b}',
		"Odd Name" integer,
		"select" integer
	);
	CREATE INDEX printed_d ON printed (d) WHERE d > '2000-01-02';
	CREATE VIEW printed_v AS SELECT d, i, f, "Odd Name" FROM printed WHERE ts > '2000-01-02 00:00:00+00' AND i > '1 hour';
	INSERT INTO printed (id, "Odd Name", "select") VALUES (1, 1, 2);

	CREATE VIEW v1 AS SELECT id, v FROM t WHERE n > 0;
	ALTER VIEW v1 ALTER COLUMN v SET DEFAULT 'x';
	COMMENT ON VIEW v1 IS 'on a view';
	GRANT SELECT ON v1 TO g_reader;
	CREATE MATERIALIZED VIEW mv AS SELECT id FROM t WITH NO DATA;
	CREATE INDEX mv_id ON mv (id);
	CREATE FUNCTION seen() RETURNS trigger LANGUAGE plpgsql AS 'BEGIN RETURN NEW; END';
	CREATE TRIGGER tr BEFORE UPDATE OF v ON t FOR EACH ROW WHEN (OLD.v <> NEW.v) EXECUTE FUNCTION seen();
	CREATE TRIGGER tr_off AFTER INSERT ON child FOR EACH ROW EXECUTE FUNCTION seen();
	ALTER TABLE child DISABLE TRIGGER tr_off;
	CREATE RULE rl AS ON INSERT TO v1 DO INSTEAD NOTHING;
	ALTER TABLE child ENABLE ROW LEVEL SECURITY;
	ALTER TABLE child FORCE ROW LEVEL SECURITY;
	CREATE POLICY pl ON child FOR SELECT TO g_reader USING (tid IS NOT NULL);
	ALTER SEQUENCE seq OWNED BY t.s;
};

# a commit is the same whatever the session that made it had set
my @settings = (
	q{DateStyle = 'SQL, DMY'}, q{DateStyle = 'German'}, q{DateStyle = 'Postgres, MDY'},
	q{IntervalStyle = 'sql_standard'}, q{IntervalStyle = 'iso_8601'}, q{IntervalStyle = 'postgres_verbose'},
	q{TimeZone = 'Asia/Tokyo'}, q{extra_float_digits = 0}, q{extra_float_digits = -5},
	q{bytea_output = 'escape'}, q{xmloption = 'document'}, q{xmlbinary = 'hex'},
	q{quote_all_identifiers = on}, q{search_path = app, public}, q{search_path = pg_catalog},
	q{lc_monetary = 'C'}, q{default_text_search_config = 'simple'},
	q{row_security = off}, q{backslash_quote = off});
my $settled = join('', map { "SET $settings[$_]; \\echo =====setting$_\nSELECT public.test_ws_root_capture('app') = :'r';\n\\echo =====end\nRESET ALL; SET catalog_branch = $from; SET client_min_messages = warning;\n" } 0 .. $#settings);

my ($rc, $out, $err) = $node->psql('postgres', qq{
	SET client_min_messages = warning;
	SET catalog_branch = $from;
	CREATE SCHEMA app VERSIONED; ALTER SCHEMA app OWNER TO g_cov;
	GRANT USAGE ON SCHEMA app TO g_reader;
	SET search_path = app;
	$superuser
	SET ROLE g_cov;
	$owner
	RESET ROLE; SET search_path = public;
	SELECT test_ws_root_capture('app') AS r \\gset
	CREATE SCHEMA emptied VERSIONED;
	\\echo =====kinds
	SELECT test_ws_diff(test_ws_root_capture('emptied'), :'r');
	\\echo =====end
	DROP SCHEMA emptied;
	SET catalog_branch = $to;
	CREATE SCHEMA app VERSIONED; ALTER SCHEMA app OWNER TO g_cov;
	GRANT USAGE ON SCHEMA app TO g_reader;
	SELECT test_ws_root_restore(:'r', 'app') IS NULL AS restored \\gset
	\\echo =====same
	SELECT test_ws_root_capture('app') = :'r';
	\\echo =====end
	SET catalog_branch = $from;
	$settled
});
is($rc, 0, 'made, captured and restored') or diag($err);
like($out, qr/=====same\nt\n/, 'it captures as the root it was restored from');

for my $i (0 .. $#settings)
{
	like($out, qr/=====setting$i\nt\n/, "captured under $settings[$i], the same root");
}

# every kind a commit can hold, by what the code says those are
my $code = slurp_file(dirname(__FILE__) . '/../../../../backend/prolly/ws/ws_obj.c');
my %kinds = map { $_ => 1 } $code =~ /^\s*\[WS_OBJ_\w+\] = "([a-z_-]+)",$/mg;
ok(scalar(keys %kinds) > 20, 'the kinds are read from the code');
my ($said) = $out =~ /=====kinds\n(.*?)\n?=====end/s;
my %held = map { /^object added (\S+) / ? ($1 => 1) : () } split /\n/, $said // '';
for my $kind (sort keys %kinds)
{
	ok($held{$kind}, "this schema holds a $kind");
}

# a dump, by its objects: of all that the branch shows, as a cast is of no schema
sub dump_of
{
	my ($branch) = @_;
	local $ENV{PGOPTIONS} = "-c catalog_branch=$branch";
	my ($stdout, $stderr);
	IPC::Run::run(
		[ 'pg_dump', '--schema-only', '-d', $node->connstr('postgres') ],
		'>', \$stdout, '2>', \$stderr) or die "pg_dump: $stderr";
	my %objects;
	my $at;
	for my $line (split /\n/, $stdout)
	{
		next if $line =~ /^\\(un)?restrict /;
		if ($line =~ /^-- (?:Data for )?Name: (.*); Owner: (.*)$/) { $at = $1; $objects{$at} = "Owner: $2\n"; next; }
		next if !defined $at || $line =~ /^--$/ || $line eq '';
		$objects{$at} .= "$line\n";
	}
	return \%objects;
}

my $was = dump_of($from);
my $now = dump_of($to);
ok(scalar(keys %$was) > 50, 'a dump of many objects');
for my $type ('CAST', 'OPERATOR CLASS', 'TEXT SEARCH PARSER', 'POLICY', 'TABLE ATTACH')
{
	ok((grep { /; Type: \Q$type\E;/ } keys %$was), "a dump that has a $type");
}
is_deeply([ grep { !exists $now->{$_} } sort keys %$was ], [], 'none is lost');
is_deeply([ grep { !exists $was->{$_} } sort keys %$now ], [], 'none is new');
for my $object (sort keys %$was)
{
	is($now->{$object}, $was->{$object}, "the same: $object") if exists $now->{$object};
}

# each dump is of its own branch, or all this says nothing
{
	local $ENV{PGOPTIONS} = "-c catalog_branch=$to";
	$node->safe_psql('postgres', q{COMMENT ON TABLE app.t IS 'changed'});
}
my $changed = dump_of($to);
my @differ = grep { ($changed->{$_} // '') ne ($was->{$_} // '') } sort keys %$was;
is_deeply(\@differ, [ 'TABLE t; Type: COMMENT; Schema: app' ], 'a dump of the other branch sees a change, and only it');

$node->stop;
done_testing();
