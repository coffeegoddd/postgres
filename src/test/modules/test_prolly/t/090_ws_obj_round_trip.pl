# Copyright (c) 2026, PostgreSQL Global Development Group

# A schema's views and materialized views round-trip: generated, restored into
# another schema, and the same by the catalogs, naming nothing of where they came from.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('ws_obj');
$node->init(extra => ['--encoding=UTF8', '--locale=C']);
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_prolly');

# (a schema's objects by the catalogs, with its name out of them)
my $dump = sub {
	my ($schema) = @_;
	my $out = $node->safe_psql('postgres', qq{SELECT string_agg(
		format('%s %s %s owner=%s acl=%s opts=%s comment=%s def=%s cols=%s',
			c.relkind, c.relname, c.relispopulated, pg_get_userbyid(c.relowner), (SELECT array_agg(x::text ORDER BY x::text) FROM unnest(c.relacl) x), c.reloptions,
			obj_description(c.oid, 'pg_class'),
			replace(pg_get_viewdef(c.oid), '$schema.', ''),
			(SELECT string_agg(format('%s:%s:%s:%s', a.attname, format_type(a.atttypid, a.atttypmod), a.attacl,
				col_description(c.oid, a.attnum)), ',' ORDER BY a.attnum)
			 FROM pg_attribute a WHERE a.attrelid = c.oid AND a.attnum > 0)),
		E'\\n' ORDER BY c.relname)
		FROM pg_class c WHERE c.relnamespace = '"$schema"'::regnamespace AND c.relkind IN ('v', 'm')});
	$out =~ s/"?\Q$schema\E"?\.//g;
	return $out;
};
my $fresh = sub {
	my ($to) = @_;
	$node->safe_psql('postgres', qq{DROP SCHEMA IF EXISTS "$to" CASCADE; CREATE SCHEMA "$to";
		CREATE TABLE "$to".t (a int PRIMARY KEY, b text, "Odd c" int);
		CREATE TABLE "$to".pg_class_like (x int);});
};

$node->safe_psql('postgres', q{
	CREATE ROLE g_owner NOLOGIN;
	CREATE ROLE g_read NOLOGIN;
	CREATE ROLE x_person LOGIN;
	CREATE SCHEMA "S rc" AUTHORIZATION g_owner;
	SET ROLE g_owner;
	SET search_path = "S rc";
	CREATE TABLE t (a int PRIMARY KEY, b text, "Odd c" int);
	CREATE TABLE pg_class_like (x int);
	CREATE VIEW z_base AS SELECT a, b, "Odd c" FROM t WHERE a > 0;
	CREATE VIEW y_mid WITH (security_barrier = true) AS SELECT a, b FROM z_base;
	CREATE VIEW a_top WITH (security_invoker = true) AS SELECT y.a, y.b, 'it''s @ here'::text AS note FROM y_mid y;
	CREATE VIEW checked AS SELECT a FROM t WHERE a > 5 WITH LOCAL CHECK OPTION;
	CREATE VIEW grouped AS SELECT a, count(*) AS n FROM t GROUP BY a;
	CREATE MATERIALIZED VIEW m_base AS SELECT a FROM t WITH NO DATA;
	CREATE MATERIALIZED VIEW m_filled WITH (fillfactor = 50) AS SELECT a, b FROM t;
	CREATE VIEW on_m AS SELECT a FROM m_base;
	CREATE VIEW shadow AS SELECT x FROM pg_class_like, pg_catalog.pg_class LIMIT 0;
	COMMENT ON VIEW a_top IS 'the ''top''';
	COMMENT ON COLUMN a_top.note IS 'a note';
	COMMENT ON MATERIALIZED VIEW m_base IS 'on the base';
	GRANT SELECT ON a_top TO g_read;
	GRANT SELECT ON a_top TO PUBLIC;
	GRANT SELECT (a) ON z_base TO g_read WITH GRANT OPTION;
	GRANT SELECT ON m_base TO g_read;
});

my $held = $node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('S rc')});
my %keys = map { (split /\t/)[0] => 1 } split /\n/, $held;
ok($keys{$_}, "$_ is held")
  for qw(view/a_top view/y_mid view/z_base view/checked view/grouped view/on_m matview/m_base matview/m_filled);
my ($top) = grep { /^view\/a_top\t/ } split /\n/, $held;
my $value = pack('H*', (split /\t/, $top)[1]);
unlike($value, qr/S rc/, 'a definition does not name its schema');
like($value, qr/it''s @@ here/, 'an @ of the text is doubled');
like($value, qr/view\/y_mid/, 'it says what it needs');

$fresh->('Dest ination');
$node->safe_psql('postgres', q{SELECT test_ws_obj_copy('S rc', 'Dest ination')});
is( $dump->('Dest ination'),
	$dump->('S rc') =~ s/(m m_filled )t/$1f/r,
	'the same by the catalogs, with a materialized view empty');
is( $node->safe_psql('postgres', q{SELECT count(*) FROM pg_rewrite r
	JOIN pg_depend d ON d.objid = r.oid AND d.classid = 'pg_rewrite'::regclass
	JOIN pg_class q ON q.oid = d.refobjid JOIN pg_class c ON c.oid = r.ev_class
	WHERE c.relnamespace = '"Dest ination"'::regnamespace AND q.relnamespace = '"S rc"'::regnamespace}),
	'0', 'and nothing of the source is used');
is($node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('Dest ination')}), $held, 'and the same bytes there');

# (what a view needs elsewhere is refused)
$node->safe_psql('postgres', q{
	CREATE FUNCTION public.ws_explode() RETURNS int LANGUAGE plpgsql AS $$ BEGIN RAISE 'ran it'; END $$;
	SET ROLE g_owner;
	SET search_path = "S rc";
	CREATE MATERIALIZED VIEW m_runs AS SELECT public.ws_explode() AS x WITH NO DATA;
});
my ($rc, $out, $err) = $node->psql('postgres', q{SELECT test_ws_obj_bytes('S rc')});
like($err, qr/m_runs.*needs something in another schema/, 'what it needs elsewhere is refused');
$node->safe_psql('postgres', q{DROP MATERIALIZED VIEW "S rc".m_runs; DROP FUNCTION public.ws_explode()});

# what a commit cannot hold is refused by name, nothing left out in silence
my @refused = (
	[ q{CREATE VIEW r_other AS SELECT * FROM public.o}, 'another schema', 'CREATE TABLE public.o (a int); GRANT SELECT ON public.o TO g_owner', 'DROP TABLE public.o CASCADE' ],
	[ q{CREATE TABLE pg_class (x int); CREATE VIEW r_shadow AS SELECT x FROM "S rc".pg_class}, "say a schema's name", undef, 'DROP TABLE "S rc".pg_class CASCADE' ],
	[ q{CREATE TABLE "text" (x int); CREATE VIEW r_rowtype AS SELECT NULL::"S rc"."text" AS y}, "say a schema's name", undef, 'DROP TABLE "S rc"."text" CASCADE' ],
	[ q{CREATE VIEW r_nsp AS SELECT '"S rc"'::regnamespace AS n}, 'no commit holds yet', undef, 'DROP VIEW "S rc".r_nsp' ],
	[ q{CREATE VIEW r_nsp2 AS SELECT 'public'::regnamespace AS n}, 'no commit holds yet', undef, 'DROP VIEW "S rc".r_nsp2' ],
	[ q{CREATE VIEW r_values (x, y) AS VALUES (1, 2)}, 'a VALUES list', undef, 'DROP VIEW "S rc".r_values' ],
	[ q{CREATE VIEW r_self AS SELECT 1 AS a; CREATE OR REPLACE VIEW r_self AS SELECT a FROM r_self}, 'needs itself', undef, 'DROP VIEW "S rc".r_self' ],
	[ q{CREATE VIEW r_c1 AS SELECT 1 AS a; CREATE VIEW r_c2 AS SELECT a FROM r_c1; CREATE OR REPLACE VIEW r_c1 AS SELECT a FROM r_c2}, 'a cycle', undef, 'DROP VIEW "S rc".r_c1, "S rc".r_c2 CASCADE' ],
	[ q{CREATE MATERIALIZED VIEW r_store AS SELECT b FROM t; ALTER MATERIALIZED VIEW r_store ALTER COLUMN b SET STORAGE EXTERNAL}, 'stored its own way', undef, 'DROP MATERIALIZED VIEW "S rc".r_store' ],
	[ q{CREATE MATERIALIZED VIEW r_comp AS SELECT b FROM t; ALTER MATERIALIZED VIEW r_comp ALTER COLUMN b SET COMPRESSION pglz}, 'stored its own way', undef, 'DROP MATERIALIZED VIEW "S rc".r_comp' ],
	[ q{CREATE MATERIALIZED VIEW r_toast AS SELECT b FROM t; ALTER MATERIALIZED VIEW r_toast SET (toast.autovacuum_enabled = false)}, 'its TOAST table', undef, 'DROP MATERIALIZED VIEW "S rc".r_toast' ],
	[ q{CREATE VIEW r_revoked AS SELECT a FROM t; REVOKE DELETE ON r_revoked FROM CURRENT_USER}, 'rights have been changed', undef, 'DROP VIEW "S rc".r_revoked' ],
	[ q{CREATE VIEW r_self0 AS SELECT 1 AS x; CREATE OR REPLACE VIEW r_self0 AS SELECT 1 AS x FROM r_self0}, 'needs itself', undef, 'DROP VIEW "S rc".r_self0' ],
	[ q{CREATE VIEW r_self1 AS SELECT 1 AS x; CREATE OR REPLACE VIEW r_self1 AS SELECT 1 AS x WHERE 'r_self1'::regclass::oid > 0}, 'needs itself', undef, 'DROP VIEW "S rc".r_self1' ],
	[ q{CREATE TABLE pg_type (x int); CREATE VIEW r_regclass AS SELECT '"S rc".pg_type'::regclass::oid AS o}, "say a schema's name", undef, 'DROP TABLE "S rc".pg_type CASCADE' ],
	[ q{COMMENT ON RULE "_RETURN" ON grouped IS 'on it'}, 'a comment on its rule', undef, q{COMMENT ON RULE "_RETURN" ON "S rc".grouped IS NULL} ],
	[ q{CREATE VIEW r_regarr AS SELECT '{t}'::regclass[] AS a}, 'an array of reg* names', undef, 'DROP VIEW "S rc".r_regarr' ],
	[ q{CREATE VIEW r_login AS SELECT a FROM t; GRANT SELECT ON r_login TO x_person}, 'which cannot travel', undef, 'DROP VIEW "S rc".r_login' ],
	[ q{CREATE VIEW r_pg AS SELECT a FROM t; GRANT SELECT ON r_pg TO pg_monitor}, 'which cannot travel', undef, 'DROP VIEW "S rc".r_pg' ],
);
for my $case (@refused)
{
	my ($sql, $why, $before, $undo) = @$case;
	$node->safe_psql('postgres', $before) if $before;
	$node->safe_psql('postgres', qq{SET ROLE g_owner; SET search_path = "S rc"; $sql});
	my ($rc2, $out2, $err2) = $node->psql('postgres', q{SELECT test_ws_obj_bytes('S rc')});
	isnt($rc2, 0, "refused: $sql");
	like($err2, qr/\Q$why\E/, "for why: $sql");
	$node->safe_psql('postgres', $undo);
}

# a view reads with its owner's rights, so one whose owner cannot travel is refused
for my $owner (['x_person', 'a login role'], ['g_power', 'a role with a power'], ['pg_monitor', 'a predefined role'])
{
	my ($role, $what) = @$owner;
	$node->safe_psql('postgres', 'CREATE ROLE g_power NOLOGIN CREATEDB') if $role eq 'g_power';
	$node->safe_psql('postgres', qq{SET search_path = "S rc"; CREATE VIEW by_other AS SELECT a FROM t; ALTER VIEW by_other OWNER TO $role});
	($rc, $out, $err) = $node->psql('postgres', q{SELECT test_ws_obj_bytes('S rc')});
	like($err, qr/"by_other" cannot be held in a commit: it is owned by a role that cannot travel/, "owned by $what is refused");
	$node->safe_psql('postgres', 'DROP VIEW "S rc".by_other');
}
is( $node->safe_psql('postgres', q{SELECT pg_get_userbyid(relowner) FROM pg_class WHERE oid = '"Dest ination".a_top'::regclass}),
	'g_owner', 'and one that can travel owns it after a restore');

# restoring a materialized view runs nothing
$node->safe_psql('postgres', q{SET ROLE g_owner; SET search_path = "S rc"; CREATE MATERIALIZED VIEW m_lock AS SELECT pg_try_advisory_lock(42) AS x WITH NO DATA});
$fresh->('D6');
is( $node->safe_psql('postgres', q{SELECT test_ws_obj_copy('S rc', 'D6'); SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND pid = pg_backend_pid()}),
	"\n0", 'a materialized view is restored without running what it would fill itself with');
$node->safe_psql('postgres', 'DROP MATERIALIZED VIEW "S rc".m_lock');

# in another encoding, the same bytes
$node->safe_psql('postgres', q{CREATE DATABASE latin TEMPLATE template0 ENCODING 'LATIN1' LOCALE 'C'});
$node->safe_psql('latin', q{CREATE EXTENSION test_prolly; CREATE SCHEMA s AUTHORIZATION g_owner; SET ROLE g_owner; SET search_path = s;
	CREATE TABLE t (a int); CREATE VIEW v AS SELECT a, 'caf} . "\xe9" . q{'::text AS note FROM t});
$node->safe_psql('postgres', q{CREATE SCHEMA enc AUTHORIZATION g_owner; SET ROLE g_owner; SET search_path = enc;
	CREATE TABLE t (a int); CREATE VIEW v AS SELECT a, 'café'::text AS note FROM t});
is( $node->safe_psql('latin', q{SELECT test_ws_obj_bytes('s')}),
	$node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('enc')}),
	'a definition is the same whatever the encoding it was read in');

# default privileges would add grants a commit does not hold
$fresh->('D3');
$node->safe_psql('postgres', q{ALTER DEFAULT PRIVILEGES IN SCHEMA "D3" GRANT SELECT ON TABLES TO g_read});
($rc, $out, $err) = $node->psql('postgres', q{SELECT test_ws_obj_copy('S rc', 'D3')});
like($err, qr/default privileges would change what is made/, 'default privileges refuse a restore');

# what an unqualified name could be taken for is refused, the catalogs coming first or not
$node->safe_psql('postgres', q{SET ROLE g_owner; SET search_path = "S rc"; CREATE VIEW uses_json AS SELECT to_json(1) AS j});
$fresh->('D4');
$node->safe_psql('postgres', q{CREATE FUNCTION "D4".to_json(integer) RETURNS text LANGUAGE sql AS $$SELECT 'shadow'$$});
($rc, $out, $err) = $node->psql('postgres', q{SELECT test_ws_obj_copy('S rc', 'D4')});
like($err, qr/schema "D4" holds a function named as one in the catalogs/, 'an overload in the destination refuses a restore');
$fresh->('D4b');
$node->safe_psql('postgres', q{CREATE FUNCTION "D4b".mine() RETURNS int LANGUAGE sql AS 'SELECT 1'; CREATE TYPE "D4b".ir AS RANGE (SUBTYPE = int4)});
$node->safe_psql('postgres', q{SELECT test_ws_obj_copy('S rc', 'D4b')});
is($node->safe_psql('postgres', q{SELECT count(*) FROM pg_class WHERE oid = '"D4b".uses_json'::regclass}), '1', 'and one no catalog has a name of is nothing to it');
$fresh->('D5');
$node->safe_psql('postgres', q{SELECT test_ws_obj_copy('S rc', 'D5')});
is( $node->safe_psql('postgres', q{SELECT format_type(atttypid, NULL) FROM pg_attribute WHERE attrelid = '"D5".uses_json'::regclass AND attname = 'j'}),
	'json', 'and without one it means what it did');

# a view over a system view needs nothing a commit holds
$node->safe_psql('postgres', q{SET ROLE g_owner; SET search_path = "S rc"; CREATE VIEW over_system AS SELECT rolname FROM pg_roles});
$fresh->('D7');
$node->safe_psql('postgres', q{SELECT test_ws_obj_copy('S rc', 'D7')});
is($node->safe_psql('postgres', q{SELECT count(*) FROM pg_class WHERE oid = '"D7".over_system'::regclass}), '1', 'a view over a system view is held');

# a destination's own types, which a commit's tables may need, are no matter
$fresh->('D8');
$node->safe_psql('postgres', q{CREATE TYPE "D8".mood AS ENUM ('ok'); CREATE COLLATION "D8".c2 FROM "C"});
$node->safe_psql('postgres', q{SELECT test_ws_obj_copy('S rc', 'D8')});
is($node->safe_psql('postgres', q{SELECT count(*) FROM pg_class WHERE oid = '"D8".a_top'::regclass}), '1', 'a restore goes on with types there');

# restoring as a role that is not a superuser, with a grant to that role
$node->safe_psql('postgres', q{CREATE ROLE g_rest NOLOGIN; GRANT g_owner TO g_rest;
	CREATE SCHEMA s3 AUTHORIZATION g_owner; SET ROLE g_owner; CREATE TABLE s3.t3 (a int); CREATE VIEW s3.v3 AS SELECT a FROM s3.t3; GRANT SELECT ON s3.v3 TO g_rest;
	RESET ROLE; CREATE SCHEMA d3 AUTHORIZATION g_rest; GRANT CREATE, USAGE ON SCHEMA d3 TO g_owner;
	SET ROLE g_rest; CREATE TABLE d3.t3 (a int)});
$node->safe_psql('postgres', q{SET ROLE g_rest; SELECT test_ws_obj_copy('s3', 'd3')});
is( $node->safe_psql('postgres', q{SELECT pg_get_userbyid(relowner), relacl::text LIKE '%g_rest=r/g_owner%' FROM pg_class WHERE oid = 'd3.v3'::regclass}),
	'g_owner|t', 'it restores, and a grant to the role restoring is not lost');
$node->safe_psql('postgres', q{DROP VIEW d3.v3; GRANT USAGE ON SCHEMA s3 TO g_rest; REVOKE g_owner FROM g_rest});
($rc, $out, $err) = $node->psql('postgres', q{SET ROLE g_rest; SELECT test_ws_obj_copy('s3', 'd3')});
like($err, qr/must be able to SET ROLE "g_owner"/, 'one that cannot become its owner is told so');
$node->safe_psql('postgres', q{REVOKE USAGE ON SCHEMA s3 FROM g_rest});
($rc, $out, $err) = $node->psql('postgres', q{SET ROLE g_rest; SELECT test_ws_obj_bytes('s3')});
like($err, qr/permission denied for schema s3/, 'one with no USAGE on the schema is told that');

# in a versioned schema
{
	my $branch = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
	local $ENV{PGOPTIONS} = "-c catalog_branch=$branch";
	$node->safe_psql('postgres', q{CREATE SCHEMA vs AUTHORIZATION g_owner VERSIONED; CREATE SCHEMA vd AUTHORIZATION g_owner VERSIONED;
		SET ROLE g_owner; CREATE TABLE vs.t (a int); CREATE VIEW vs.v AS SELECT a FROM vs.t; CREATE TABLE vd.t (a int)});
	$node->safe_psql('postgres', q{SELECT test_ws_obj_copy('vs', 'vd')});
	is( $node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('vd')}),
		$node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('vs')}),
		'a versioned schema round-trips as any');
}

# an old snapshot with the schema changed since is one to try again, with tables
$node->safe_psql('postgres', q{SET ROLE g_owner; SET search_path = "S rc"; CREATE VIEW now_a AS SELECT a AS was FROM t});
my $s = $node->background_psql('postgres', on_error_stop => 0);
$s->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
$s->query_safe('SELECT count(*) FROM pg_class');
my $unchanged = $s->query_safe(q{SELECT test_ws_obj_bytes('S rc')});
is($unchanged, $node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('S rc')}), 'an old snapshot reads a schema not changed since');
$s->query_safe('COMMIT');
$s->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
$s->query_safe('SELECT count(*) FROM pg_class');
$node->safe_psql('postgres', q{SET ROLE g_owner; SET search_path = "S rc"; ALTER TABLE t RENAME COLUMN a TO a_new; CREATE VIEW later AS SELECT 1 AS x});
$s->{stderr} = '';
$s->query(q{SELECT test_ws_obj_bytes('S rc')});
like($s->{stderr}, qr/could not read schema "S rc": it has changed since this transaction's snapshot/, 'a schema changed since is to be tried again');
$s->query('ROLLBACK');

# a change that holds nothing new is no reason to try again; a rename of a role is
for my $case (['TRUNCATE "S rc".t', 0, 'a TRUNCATE'], ['VACUUM FULL "S rc".t', 0, 'a VACUUM FULL'],
	['ALTER ROLE g_read RENAME TO g_read2', 1, 'a grantee renamed'], ['ALTER ROLE g_read2 RENAME TO g_read', 1, 'a grantee renamed back'])
{
	my ($change, $fails, $what) = @$case;
	$s->{stderr} = '';
	$s->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
	$s->query_safe('SELECT count(*) FROM pg_class');
	$node->safe_psql('postgres', $change);
	$s->{stderr} = '';
	$s->query(q{SELECT test_ws_obj_bytes('S rc')});
	if ($fails) { like($s->{stderr}, qr/has changed since/, "$what is to be tried again"); }
	else { unlike($s->{stderr}, qr/ERROR/, "$what is no reason to try again"); }
	$s->query('ROLLBACK');
}

# what is read stays as it was read until the transaction ends
my $holder = $node->background_psql('postgres');
$holder->query_safe('BEGIN');
$holder->query_safe(q{SELECT test_ws_obj_bytes('S rc')});
($rc, $out, $err) = $node->psql('postgres', q{SET lock_timeout = '500ms'; ALTER TABLE "S rc".t RENAME COLUMN a_new TO a_mid});
like($err, qr/canceling statement due to lock timeout/, 'what a view needs is locked, so it cannot be renamed under the read');
$holder->query_safe('ROLLBACK');
$holder->quit;

# and in READ COMMITTED, a rename under way is waited for
my $renamer = $node->background_psql('postgres');
$renamer->query_safe('BEGIN');
$renamer->query_safe(q{ALTER TABLE "S rc".t RENAME COLUMN a_new TO a_rc});
$renamer->query_safe(q{COMMENT ON VIEW "S rc".now_a IS 'with the rename'});
my $reader = $node->background_psql('postgres');
$reader->query_until(qr/started/, "\\echo started\nSELECT test_ws_obj_bytes('S rc');\n\\echo done\n");
$node->poll_query_until('postgres', q{SELECT count(*) > 0 FROM pg_locks WHERE NOT granted}) or die 'it did not wait';
$renamer->query_safe('COMMIT');
$reader->{stdin} .= "\\echo finished\n";
$reader->{run}->pump until $reader->{stdout} =~ /finished/;
my $line = (grep { /^view\/now_a\t/ } split /\n/, $reader->{stdout})[0];
like(pack('H*', (split /\t/, $line)[1]), qr/a_rc AS was/, 'and read as it left things');
like(pack('H*', (split /\t/, $line)[1]), qr/with the rename/, 'all of them, not a new definition with an old comment');
$_->quit for ($renamer, $reader, $s);

# types: made before tables, which may use them, and needed by views
$node->safe_psql('postgres', q{
	CREATE ROLE g_tyowner NOLOGIN;
	CREATE ROLE g_tyuser NOLOGIN;
	CREATE SCHEMA ty AUTHORIZATION g_tyowner;
	SET ROLE g_tyowner;
	SET search_path = ty;
	CREATE TYPE z_mood AS ENUM ('sad', 'it''s @ ok', 'happy');
	ALTER TYPE z_mood ADD VALUE 'meh' BEFORE 'happy';
	CREATE DOMAIN y_pos AS integer DEFAULT 1 CONSTRAINT y_pos_ok CHECK (VALUE > 0) CONSTRAINT y_pos_nn NOT NULL;
	CREATE DOMAIN a_pos2 AS y_pos CONSTRAINT small CHECK (VALUE < 100);
	CREATE DOMAIN d_coll AS text COLLATE "C";
	CREATE TYPE x_pair AS (m z_mood, p a_pos2, t text COLLATE "C", "Odd" numeric(5, 2), arr y_pos[]);
	CREATE TYPE w_nest AS (inner_pair x_pair);
	CREATE TYPE v_range AS RANGE (SUBTYPE = float8, SUBTYPE_DIFF = float8mi);
	CREATE TYPE u_collrange AS RANGE (SUBTYPE = text, COLLATION = "C");
	COMMENT ON TYPE z_mood IS 'how it''s going';
	COMMENT ON COLUMN x_pair.m IS 'a mood';
	COMMENT ON DOMAIN y_pos IS 'positive';
	GRANT USAGE ON TYPE z_mood TO g_tyuser;
	GRANT USAGE ON DOMAIN y_pos TO g_tyuser WITH GRANT OPTION;
	CREATE TABLE b_uses (m z_mood, p y_pos, s v_range, ms v_multirange);
	CREATE VIEW over_types AS SELECT m, p::a_pos2 AS p2, ARRAY[m] AS ms, NULL::w_nest IS NULL AS n, 'sad'::z_mood AS c, s, NULL::v_multirange AS mr FROM b_uses;
	CREATE VIEW over_composite AS SELECT NULL::x_pair AS pair;
	CREATE VIEW fields AS SELECT (NULL::x_pair).m AS m, u_collrange('a', 'b') AS r;
	CREATE TYPE s_changed AS (keep integer, gone integer, was integer);
	ALTER TYPE s_changed DROP ATTRIBUTE gone;
	ALTER TYPE s_changed RENAME ATTRIBUTE was TO now_is;
	CREATE TYPE t_opc AS RANGE (SUBTYPE = text, SUBTYPE_OPCLASS = text_pattern_ops);
	CREATE DOMAIN q_field AS x_pair CHECK ((VALUE).t IS NOT NULL);
	CREATE TYPE u_ints AS RANGE (SUBTYPE = int4);
	CREATE DOMAIN p_ctor AS integer CHECK (VALUE <@ u_ints(0, 100));
	CREATE VIEW b_via_array AS SELECT 1 AS a;
	CREATE VIEW a_via_array AS SELECT NULL::b_via_array[] AS arr;
	CREATE TYPE r_range_range AS RANGE (SUBTYPE = int8);
});
my $tydump = sub {
	my ($schema) = @_;
	my $out = $node->safe_psql('postgres', qq{SELECT string_agg(
		format('%s %s owner=%s acl=%s comment=%s collation=%s default=%s base=%s enum=%s attrs=%s checks=%s range=%s',
			t.typtype, t.typname, pg_get_userbyid(t.typowner),
			(SELECT array_agg(x::text ORDER BY x::text) FROM unnest(t.typacl) x), obj_description(t.oid, 'pg_type'), t.typcollation::regcollation,
			pg_get_expr(t.typdefaultbin, 0), format_type(t.typbasetype, t.typtypmod),
			(SELECT string_agg(enumlabel, ',' ORDER BY enumsortorder) FROM pg_enum WHERE enumtypid = t.oid),
			(SELECT string_agg(format('%s:%s:%s:%s', a.attname, format_type(a.atttypid, a.atttypmod), a.attcollation::regcollation, col_description(t.typrelid, a.attnum)), ',' ORDER BY a.attnum)
			 FROM pg_attribute a WHERE a.attrelid = t.typrelid AND a.attnum > 0 AND NOT a.attisdropped),
			(SELECT string_agg(conname || ' ' || pg_get_constraintdef(oid), ',' ORDER BY conname) FROM pg_constraint WHERE contypid = t.oid),
			(SELECT format('%s %s %s %s %s %s', format_type(rngsubtype, NULL), rngcollation::regcollation, rngsubdiff, rngcanonical, rngsubopc, rngmultitypid::regtype) FROM pg_range WHERE rngtypid = t.oid)),
		E'\n' ORDER BY t.typname)
		FROM pg_type t WHERE t.typnamespace = '"$schema"'::regnamespace AND t.typtype IN ('e', 'd', 'r') OR
			t.typnamespace = '"$schema"'::regnamespace AND t.typtype = 'c' AND t.typrelid IN (SELECT oid FROM pg_class WHERE relkind = 'c')});
	$out =~ s/"?\Q$schema\E"?\.//g;
	return $out;
};
my $ty_held = $node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('ty')});
my %ty_keys = map { (split /\t/)[0] => 1 } split /\n/, $ty_held;
ok($ty_keys{$_}, "$_ is held") for qw(type/z_mood domain/y_pos domain/a_pos2 domain/d_coll type/x_pair type/w_nest type/v_range type/u_collrange view/over_types);
ok(!grep({ /multirange|_z_mood|^table|^type\/b_uses/ } keys %ty_keys), 'nor what comes of them: arrays, multiranges, row types');
my %ty_val = map { my ($k, $v) = split /\t/; $k => pack('H*', $v) } split /\n/, $ty_held;
like($ty_val{'type/x_pair'}, qr/domain\/a_pos2.*domain\/y_pos.*type\/z_mood/s, 'a composite says what it needs, an array by its element');
like($ty_val{'view/over_types'}, qr/domain\/a_pos2.*type\/v_range.*type\/w_nest.*type\/z_mood/s, 'and a view, a multirange by its range');
like($ty_val{'type/z_mood'}, qr/it''s @@ ok.*meh.*happy/s, 'an enum is in order, with its @ doubled');
unlike(join('', values %ty_val), qr/\bty\./, 'no name of its schema');
like($ty_val{'domain/q_field'}, qr/type\/x_pair/, 'a domain of a composite needs it');
like($ty_val{'domain/p_ctor'}, qr/type\/u_ints/, 'a domain using a constructor needs its range');
like($ty_val{'view/a_via_array'}, qr/view\/b_via_array/, 'a view of an array of a view needs that view');
like($ty_val{'type/t_opc'}, qr/SUBTYPE_OPCLASS = text_pattern_ops/, 'an opclass not the default is said');
like($ty_val{'view/fields'}, qr/type\/u_collrange.*type\/x_pair/s, 'a field and a constructor say whose they are');

$node->safe_psql('postgres', q{CREATE SCHEMA ty2 AUTHORIZATION g_tyowner});
$node->safe_psql('postgres', q{SELECT test_ws_obj_copy('ty', 'ty2', 'before')});
is($node->safe_psql('postgres', q{SELECT count(*) FROM pg_class WHERE relnamespace = 'ty2'::regnamespace AND relkind = 'v'}), '0', 'views wait for tables');
$node->safe_psql('postgres', q{SET ROLE g_tyowner; SET search_path = ty2; CREATE TABLE b_uses (m z_mood, p y_pos, s v_range, ms v_multirange)});
$node->safe_psql('postgres', q{SELECT test_ws_obj_copy('ty', 'ty2', 'after')});
is($tydump->('ty2'), $tydump->('ty'), 'the same by the catalogs');
is($node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('ty2')}), $ty_held, 'and the same bytes');
is( $node->safe_psql('postgres', q{SET search_path = ty2; INSERT INTO b_uses (m) VALUES ('meh'); SELECT p, m > 'sad', m < 'happy' FROM b_uses}),
	'1|t|t', 'a default and an order as they were');

# under an old snapshot, a type's change or its roles' is one to try again
for my $change (q{ALTER TYPE ty.z_mood ADD VALUE 'later'}, q{ALTER DOMAIN ty.y_pos DROP CONSTRAINT y_pos_nn},
	q{ALTER ROLE g_tyuser RENAME TO g_tyuser2}, q{ALTER ROLE g_tyowner RENAME TO g_tyowner2})
{
	my $old = $node->background_psql('postgres', on_error_stop => 0);
	$old->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
	$old->query_safe('SELECT count(*) FROM pg_class');
	$node->safe_psql('postgres', $change);
	$old->{stderr} = '';
	$old->query(q{SELECT test_ws_obj_bytes('ty')});
	like($old->{stderr}, qr/has changed since/, "to try again after: $change");
	$old->quit;
}
$node->safe_psql('postgres', q{ALTER ROLE g_tyuser2 RENAME TO g_tyuser; ALTER ROLE g_tyowner2 RENAME TO g_tyowner});

# default privileges on functions, which a range's constructors take, are no refusal
$node->safe_psql('postgres', q{CREATE SCHEMA tyd; ALTER DEFAULT PRIVILEGES IN SCHEMA tyd GRANT EXECUTE ON FUNCTIONS TO g_tyuser; CREATE TYPE tyd.r AS RANGE (SUBTYPE = int4)});
like($node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('tyd')}), qr/^type\/r\t/, 'a range under default privileges on functions is held');
$node->safe_psql('postgres', q{ALTER DEFAULT PRIVILEGES IN SCHEMA tyd REVOKE EXECUTE ON FUNCTIONS FROM g_tyuser; DROP SCHEMA tyd CASCADE});

# a type's owner changed takes its range's functions with it
$node->safe_psql('postgres', q{CREATE SCHEMA tyo; CREATE TYPE tyo.r AS RANGE (SUBTYPE = int4); ALTER TYPE tyo.r OWNER TO g_tyowner});
like($node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('tyo')}), qr/^type\/r\t/, 'a range given another owner is held');

# a name too long for a commit, though not for this database
$node->safe_psql('postgres', q{CREATE DATABASE tylatin TEMPLATE template0 ENCODING 'LATIN1' LOCALE 'C'});
$node->safe_psql('tylatin', q{CREATE EXTENSION test_prolly; CREATE SCHEMA s; CREATE TYPE s.c AS ("} . ("\xe9" x 40) . q{" integer)});
($rc, $out, $err) = $node->psql('tylatin', q{SELECT test_ws_obj_bytes('s')});
like($err, qr/too long|longer than/, 'an attribute too long in UTF8 is refused');

# a schema's functions do not take the catalogs' place in reading its objects, nor run
$node->safe_psql('postgres', q{
	CREATE SCHEMA tyc;
	CREATE TABLE tyc.ran (a integer);
	CREATE FUNCTION tyc.quote_ident(name) RETURNS text LANGUAGE sql AS $$ INSERT INTO tyc.ran VALUES (1) RETURNING 'captured' $$;
	CREATE FUNCTION tyc.quote_literal(name) RETURNS text LANGUAGE sql AS $$ INSERT INTO tyc.ran VALUES (2) RETURNING 'captured' $$;
	CREATE TYPE tyc.e AS ENUM ('a');
	CREATE TYPE tyc.c AS (x integer, y tyc.e);
	CREATE ROLE g_tyc NOLOGIN;
	ALTER SCHEMA tyc OWNER TO g_tyc;
	ALTER TABLE tyc.ran OWNER TO g_tyc;
	CREATE VIEW tyc.v AS SELECT a FROM tyc.ran;
	ALTER VIEW tyc.v OWNER TO g_tyc;
});
($rc, $out, $err) = $node->psql('postgres', q{SELECT test_ws_obj_bytes('tyc')});
like($err, qr/it is named as one in the catalogs/, 'a function named as a catalog function is refused, not taken for it');
is($node->safe_psql('postgres', 'SELECT count(*) FROM tyc.ran'), '0', 'and runs none of them');

# a domain whose constant could hold names says so; one naming a schema is refused
for my $case (
	[ q{CREATE DOMAIN tyh.dn AS regnamespace DEFAULT 'tyh'::regnamespace}, "say a schema's name" ],
	[ q{CREATE TYPE tyh.holds AS (r regclass); CREATE DOMAIN tyh.dh AS tyh.holds DEFAULT '(pg_class)'}, 'may hold names' ])
{
	my ($sql, $why) = @$case;
	$node->safe_psql('postgres', qq{DROP SCHEMA IF EXISTS tyh CASCADE; CREATE SCHEMA tyh; $sql});
	my ($rc3, $out3, $err3) = $node->psql('postgres', q{SELECT test_ws_obj_bytes('tyh')});
	like($err3, qr/\Q$why\E/, "refused: $sql");
}

# a long name's multirange is as CREATE TYPE names it, and held
for my $long ('b' x 55, 'c' x 63)
{
	$node->safe_psql('postgres', qq{DROP SCHEMA IF EXISTS tyl CASCADE; CREATE SCHEMA tyl; CREATE TYPE tyl.$long AS RANGE (SUBTYPE = int4)});
	like($node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('tyl')}), qr/^type\/\Q$long\E\t/, 'a range of a long name is held');
}

# a comment since a snapshot is one to try again, on a view or a type
for my $change (q{COMMENT ON TYPE ty.z_mood IS 'later'}, q{COMMENT ON VIEW ty.over_composite IS 'later'})
{
	my $old = $node->background_psql('postgres', on_error_stop => 0);
	$old->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
	$old->query_safe('SELECT count(*) FROM pg_class');
	$node->safe_psql('postgres', $change);
	$old->{stderr} = '';
	$old->query(q{SELECT test_ws_obj_bytes('ty')});
	like($old->{stderr}, qr/has changed since/, "to try again after: $change");
	$old->quit;
}

# a type renamed while being read takes no lock: the read says so, to try again
if ($ENV{enable_injection_points} eq 'yes' && $node->check_extension('injection_points'))
{
	$node->safe_psql('postgres', q{CREATE EXTENSION IF NOT EXISTS injection_points;
		SELECT injection_points_attach('ws-obj-read-end', 'wait')});
	my $reader2 = $node->background_psql('postgres', on_error_stop => 0);
	$reader2->query_until(qr/reading/, "\\echo reading\nSELECT test_ws_obj_bytes('ty');\n\\echo done\n");
	$node->poll_query_until('postgres', q{SELECT count(*) > 0 FROM pg_stat_activity WHERE wait_event = 'ws-obj-read-end'}) or die 'no reader';
	$node->safe_psql('postgres', q{ALTER TYPE ty.z_mood RENAME TO z_mood2;
		SELECT injection_points_wakeup('ws-obj-read-end');
		SELECT injection_points_detach('ws-obj-read-end');});
	$reader2->query_until(qr/finished/, "\\echo finished\n");
	like($reader2->{stderr}, qr/it changed while it was being read/, 'a read overtaken by a rename is to be tried again');
	$reader2->quit;
	$node->safe_psql('postgres', q{ALTER TYPE ty.z_mood2 RENAME TO z_mood});

	# a constraint gone by when it is printed: not a crash, a read to try again
	$node->safe_psql('postgres', q{CREATE SCHEMA tyg; CREATE DOMAIN tyg.d AS integer CONSTRAINT zz CHECK (VALUE > 0);
		SELECT injection_points_attach('ws-obj-print-constraint', 'wait')});
	my $reader3 = $node->background_psql('postgres', on_error_stop => 0);
	$reader3->query_until(qr/reading/, "\\echo reading\nSELECT test_ws_obj_bytes('tyg');\n\\echo done\n");
	$node->poll_query_until('postgres', q{SELECT count(*) > 0 FROM pg_stat_activity WHERE wait_event = 'ws-obj-print-constraint'}) or die 'no reader';
	$node->safe_psql('postgres', q{ALTER DOMAIN tyg.d DROP CONSTRAINT zz;
		SELECT injection_points_wakeup('ws-obj-print-constraint');
		SELECT injection_points_detach('ws-obj-print-constraint');});
	$reader3->query_until(qr/finished/, "\\echo finished\n");
	like($reader3->{stderr}, qr/it changed while it was being read/, 'a constraint dropped mid-read is a read to try again');
	$reader3->quit;
	is($node->safe_psql('postgres', 'SELECT 1'), '1', 'and the server is still up');
}

# a type whose owner cannot travel goes to who restores it, unlike a view
$node->safe_psql('postgres', q{CREATE SCHEMA ty3; CREATE TYPE ty3.login_owned AS ENUM ('a'); ALTER TYPE ty3.login_owned OWNER TO x_person; CREATE SCHEMA ty4});
$node->safe_psql('postgres', q{SELECT test_ws_obj_copy('ty3', 'ty4')});
is( $node->safe_psql('postgres', q{SELECT pg_get_userbyid(typowner) = current_user FROM pg_type WHERE oid = 'ty4.login_owned'::regtype}),
	't', 'a type of a login role goes to who restores it');

# default privileges on types refuse
$node->safe_psql('postgres', q{CREATE SCHEMA ty5; ALTER DEFAULT PRIVILEGES IN SCHEMA ty5 GRANT USAGE ON TYPES TO g_tyuser});
($rc, $out, $err) = $node->psql('postgres', q{SELECT test_ws_obj_copy('ty3', 'ty5')});
like($err, qr/default privileges would change what is made/, 'default privileges on types refuse a restore');

# what a commit cannot hold of a type, by name
$node->safe_psql('postgres', q{CREATE TYPE public.elsewhere_c AS ENUM ('a')});
my @ty_refused = (
	[ q{CREATE TYPE r_shell}, 'no definition yet' ],
	[ q{CREATE TABLE rt (a int); CREATE TYPE r_rowtype AS (r rt)}, 'a table' ],
	[ q{CREATE TABLE rt2 (a int); CREATE TYPE r_rowtype2 AS (r rt2[])}, 'a table' ],
	[ q{CREATE VIEW vv AS SELECT 1 AS a; CREATE DOMAIN r_viewdom AS vv[]}, 'a view' ],
	[ q{CREATE DOMAIN r_self AS int; ALTER DOMAIN r_self SET DEFAULT (5::r_self)}, 'needs itself' ],
	[ q{CREATE TABLE r_cast4 (a int); CREATE CAST (r_cast4 AS integer) WITH INOUT}, 'row type' ],
	[ q{CREATE TYPE r_cast5 AS ENUM ('a'); CREATE CAST (r_cast5 AS public.elsewhere_c) WITH INOUT}, 'another schema' ],
	[ q{CREATE DOMAIN r_self2 AS int; ALTER DOMAIN r_self2 ADD CONSTRAINT k CHECK ('{}'::r_self2[] IS NOT NULL)}, 'needs itself' ],
	[ q{CREATE TYPE range AS RANGE (SUBTYPE = int4)}, 'named as one in the catalogs' ],
	[ q{CREATE TYPE r_ren AS RANGE (SUBTYPE = int4); ALTER FUNCTION r_ren(int4, int4) RENAME TO r_ren2}, 'has been changed' ],
	[ q{CREATE TYPE r_own AS RANGE (SUBTYPE = int4); ALTER FUNCTION r_own(int4, int4) OWNER TO g_tyowner}, 'has been changed' ],
	[ q{CREATE TYPE r_vol AS RANGE (SUBTYPE = int4); ALTER FUNCTION r_vol(int4, int4) VOLATILE}, 'has been changed' ],
	[ q{CREATE TYPE r_set AS RANGE (SUBTYPE = int4); ALTER FUNCTION r_set(int4, int4) SET work_mem = '1MB'}, 'has been changed' ],
	[ q{CREATE TYPE r_cm AS RANGE (SUBTYPE = int4); COMMENT ON FUNCTION r_cm(int4, int4) IS 'x'}, 'its own' ],
	[ q{CREATE ROLE g_mid2 NOLOGIN; CREATE TYPE r_pub AS ENUM ('a'); REVOKE USAGE ON TYPE r_pub FROM PUBLIC; GRANT USAGE ON TYPE r_pub TO g_mid2 WITH GRANT OPTION; SET ROLE g_mid2; GRANT USAGE ON TYPE r_pub TO PUBLIC; RESET ROLE}, 'a grant by a role that does not own it' ],
	[ q{CREATE TYPE lower AS RANGE (SUBTYPE = int4)}, 'named as one in the catalogs' ],
	[ q{CREATE TYPE r_arrc AS ENUM ('a'); COMMENT ON TYPE _r_arrc IS 'x'}, 'its array type' ],
	[ q{CREATE TYPE r_ctor AS RANGE (SUBTYPE = int4); REVOKE EXECUTE ON FUNCTION r_ctor(int4, int4) FROM PUBLIC}, 'a function of it' ],
	[ q{CREATE TYPE r_mrc AS RANGE (SUBTYPE = int4); COMMENT ON TYPE r_mrc_multirange IS 'x'}, 'its multirange' ],
	[ q{CREATE DOMAIN r_nv AS int; ALTER DOMAIN r_nv ADD CONSTRAINT k CHECK (VALUE > 0) NOT VALID}, 'not validated' ],
	[ q{CREATE DOMAIN r_cc AS int CONSTRAINT k CHECK (VALUE > 0); COMMENT ON CONSTRAINT k ON DOMAIN r_cc IS 'x'}, 'a comment on a constraint' ],
	[ q{CREATE TYPE r_public AS ENUM ('a'); REVOKE USAGE ON TYPE r_public FROM PUBLIC}, 'revoked from PUBLIC' ],
	[ q{CREATE TYPE r_mr AS RANGE (SUBTYPE = int4, MULTIRANGE_TYPE_NAME = r_other)}, 'its multirange' ],
	[ "CREATE TYPE " . ('a' x 60) . " AS RANGE (SUBTYPE = int4, MULTIRANGE_TYPE_NAME = " . ('a' x 60) . "_mu)", 'its multirange' ],
	[ q{CREATE TYPE r_login AS ENUM ('a'); GRANT USAGE ON TYPE r_login TO x_person}, 'which cannot travel' ],
	[ q{CREATE TYPE public.elsewhere AS ENUM ('a'); CREATE DOMAIN r_other_schema AS public.elsewhere}, 'another schema' ],
	[ q{CREATE TYPE "text" AS ENUM ('a'); CREATE TYPE r_shadow AS (t ty6."text")}, "say a schema's name" ],
);
for my $i (0 .. $#ty_refused)
{
	my ($sql, $why) = @{ $ty_refused[$i] };
	my $sch = 'ty6';
	$node->safe_psql('postgres', qq{DROP SCHEMA IF EXISTS $sch CASCADE; DROP ROLE IF EXISTS g_mid2; DROP TYPE IF EXISTS public.elsewhere CASCADE; CREATE SCHEMA $sch; GRANT USAGE ON SCHEMA $sch TO PUBLIC; SET search_path = $sch, public; $sql});
	my ($rc2, $out2, $err2) = $node->psql('postgres', qq{SELECT test_ws_obj_bytes('$sch')});
	isnt($rc2, 0, "refused: $sql");
	like($err2, qr/\Q$why\E/, "for why: $sql");
}

# functions and procedures: most made before tables, which may use them
$node->safe_psql('postgres', q{
	CREATE ROLE g_fnowner NOLOGIN;
	CREATE ROLE g_fnuser NOLOGIN;
	CREATE SCHEMA fn AUTHORIZATION g_fnowner;
	SET ROLE g_fnowner;
	SET search_path = fn;
	CREATE TYPE fmood AS ENUM ('a', 'b');
	CREATE FUNCTION overload(integer) RETURNS integer LANGUAGE sql IMMUTABLE AS 'SELECT $1 + 1';
	CREATE FUNCTION overload(text, fmood DEFAULT 'a') RETURNS text LANGUAGE sql STABLE COST 7 AS $$ SELECT $1 || ' it''s @ here' $$;
	CREATE FUNCTION "Odd name(1"() RETURNS void LANGUAGE plpgsql AS $$ BEGIN PERFORM overload(1); END $$;
	CREATE FUNCTION outs(a integer, OUT b integer, OUT c text) RETURNS record LANGUAGE sql AS 'SELECT $1, $1::text';
	CREATE PROCEDURE proc(integer) LANGUAGE sql AS 'SELECT $1';
	CREATE FUNCTION std_return(a integer) RETURNS integer LANGUAGE sql RETURN a * 2;
	CREATE FUNCTION small_cost() RETURNS integer LANGUAGE sql COST 0.1 AS 'SELECT 1';
	CREATE FUNCTION std_atomic(a integer) RETURNS integer LANGUAGE sql BEGIN ATOMIC SELECT 1; RETURN a; END;
	CREATE FUNCTION fdiff(float8, float8) RETURNS float8 LANGUAGE sql IMMUTABLE AS 'SELECT $1 - $2';
	CREATE TYPE fspan_range AS RANGE (SUBTYPE = float8, SUBTYPE_DIFF = fdiff);
	CREATE FUNCTION definer() RETURNS integer LANGUAGE sql SECURITY DEFINER SET work_mem = '1MB' AS 'SELECT 1';
	CREATE FUNCTION revoked() RETURNS integer LANGUAGE sql AS 'SELECT 1';
	REVOKE ALL ON FUNCTION revoked() FROM PUBLIC;
	GRANT EXECUTE ON FUNCTION revoked() TO g_fnuser;
	CREATE FUNCTION granted() RETURNS integer LANGUAGE sql AS 'SELECT 1';
	GRANT EXECUTE ON FUNCTION granted() TO g_fnuser WITH GRANT OPTION;
	COMMENT ON FUNCTION overload(integer) IS 'it''s one';
	COMMENT ON PROCEDURE proc(integer) IS 'a procedure';
	CREATE DOMAIN fpos AS integer CHECK (overload(VALUE) > 0);
	CREATE TABLE uses (a integer DEFAULT overload(1), p fpos, CHECK (overload(a) > 0));
	CREATE FUNCTION names_later() RETURNS bigint LANGUAGE sql AS 'SELECT count(*) FROM uses';
	CREATE FUNCTION atomic_body() RETURNS bigint LANGUAGE sql BEGIN ATOMIC SELECT count(*) FROM uses; END;
	CREATE FUNCTION of_row(uses) RETURNS integer LANGUAGE sql AS 'SELECT 1';
	CREATE FUNCTION needs_after() RETURNS bigint LANGUAGE sql BEGIN ATOMIC SELECT atomic_body(); END;
	CREATE VIEW calls AS SELECT overload(1) AS o, atomic_body() AS n;
});
my $fn_held = $node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('fn')});
my %fn_val = map { my ($k, $v) = split /\t/; $k => pack('H*', $v) } split /\n/, $fn_held;
like($fn_val{'type/fspan_range'}, qr/SUBTYPE_DIFF = fdiff.*function\/fdiff/s, 'a range of its own subtype_diff says it bare, and needs it');
ok($fn_val{$_}, "$_ is held") for ('function/std_return(integer)', 'function/std_atomic(integer)', 'function/names_later()', 'function/overload(integer)', 'function/overload(text, fmood)', 'function/Odd name(1()', 'function/outs(integer)', 'procedure/proc(integer)', 'function/of_row(uses)');
unlike(join('', values %fn_val), qr/\bfn\./, 'no definition names its schema');
like($fn_val{'function/overload(text, fmood)'}, qr/it''s @@ here/, 'an @ of a body is doubled');
like($fn_val{'function/overload(text, fmood)'}, qr/type\/fmood/, 'a function says what type it needs');
like($fn_val{'domain/fpos'}, qr/function\/overload\(integer\)/, 'a domain says what function it needs');
like($fn_val{'view/calls'}, qr/function\/atomic_body\(\).*function\/overload\(integer\)/s, 'and a view');

$node->safe_psql('postgres', q{CREATE SCHEMA fn2 AUTHORIZATION g_fnowner});
$node->safe_psql('postgres', q{SELECT test_ws_obj_copy('fn', 'fn2', 'before')});
is( $node->safe_psql('postgres', q{SELECT string_agg(proname::text, ' ' ORDER BY proname) FROM pg_proc WHERE pronamespace = 'fn2'::regnamespace AND proname IN ('overload', 'atomic_body', 'of_row', 'needs_after')}),
	'overload overload', 'before tables are those that need none');
$node->safe_psql('postgres', q{SET ROLE g_fnowner; SET search_path = fn2; CREATE TABLE uses (a integer DEFAULT overload(1), p fpos, CHECK (overload(a) > 0))});
$node->safe_psql('postgres', q{SELECT test_ws_obj_copy('fn', 'fn2', 'after')});
is($node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('fn2')}), $fn_held, 'the same bytes restored');
my $fn_dump = sub {
	my ($s) = @_;
	my $out = $node->safe_psql('postgres', qq{SELECT string_agg(format('%s(%s) %s %s owner=%s acl=%s comment=%s',
		proname, oidvectortypes(proargtypes), prokind, prosecdef, pg_get_userbyid(proowner),
		(SELECT array_agg(x::text ORDER BY x::text) FROM unnest(proacl) x), obj_description(oid, 'pg_proc')), E'\\n' ORDER BY proname, oidvectortypes(proargtypes))
		FROM pg_proc WHERE pronamespace = '$s'::regnamespace});
	$out =~ s/\b\Q$s\E\.//g;
	return $out;
};
is($fn_dump->('fn2'), $fn_dump->('fn'), 'the same by the catalogs, grants and all');
is( $node->safe_psql('postgres', q{SELECT has_function_privilege('g_tyuser', 'fn2.revoked()', 'EXECUTE'), has_function_privilege('g_fnuser', 'fn2.revoked()', 'EXECUTE')}),
	'f|t', 'and what was revoked from PUBLIC stays so');
is($node->safe_psql('postgres', q{SET search_path = fn2; SELECT overload('x')}), "x it's @ here", 'and it runs');

# a function whose owner cannot travel goes to who restores it, unless it acts as them
$node->safe_psql('postgres', q{CREATE SCHEMA fn3; CREATE FUNCTION fn3.mine() RETURNS integer LANGUAGE sql AS 'SELECT 1'; ALTER FUNCTION fn3.mine() OWNER TO x_person; CREATE SCHEMA fn4});
$node->safe_psql('postgres', q{SELECT test_ws_obj_copy('fn3', 'fn4')});
is( $node->safe_psql('postgres', q{SELECT pg_get_userbyid(proowner) = current_user FROM pg_proc WHERE oid = 'fn4.mine()'::regprocedure}), 't', 'a function of a login role goes to who restores it');

my @fn_refused = (
	[ q{CREATE FUNCTION r() RETURNS int LANGUAGE sql SECURITY DEFINER AS 'SELECT 1'; ALTER FUNCTION r() OWNER TO x_person}, 'SECURITY DEFINER, and owned by a role that cannot travel' ],
	[ q{CREATE FUNCTION now() RETURNS int LANGUAGE sql AS 'SELECT 1'}, 'named as one in the catalogs' ],
	[ q{CREATE FUNCTION r_self(a int) RETURNS int LANGUAGE sql AS 'SELECT 1'; CREATE OR REPLACE FUNCTION r_self(a int) RETURNS int LANGUAGE sql BEGIN ATOMIC SELECT r_self(a); END}, 'needs itself' ],
	[ q{CREATE FUNCTION r_cost() RETURNS int LANGUAGE sql COST 1234567 AS 'SELECT 1'}, 'more digits' ],
	[ q{CREATE FUNCTION r_rows() RETURNS SETOF int LANGUAGE sql ROWS 1234567 AS 'SELECT 1'}, 'more digits' ],
	[ q{CREATE FUNCTION r_inf() RETURNS int LANGUAGE sql COST 1e39 AS 'SELECT 1'}, 'more than a definition can say' ],
	[ q{CREATE TYPE h AS (r regclass); CREATE FUNCTION r_h(a h DEFAULT '(pg_class)') RETURNS int LANGUAGE sql AS 'SELECT 1'}, 'may hold names' ],
	[ q{CREATE TABLE z (a int); CREATE FUNCTION r_p(int) RETURNS int LANGUAGE sql IMMUTABLE BEGIN ATOMIC SELECT count(*)::int FROM z; END; CREATE TABLE zp (a int) PARTITION BY LIST (r_p(a))}, 'a table needs it, and it needs a table' ],
	[ q{CREATE FUNCTION r_c() RETURNS int LANGUAGE internal AS 'int4pl'}, 'a language' ],
	[ q{CREATE FUNCTION r_sp() RETURNS int LANGUAGE sql SET search_path = fn5 AS 'SELECT 1'}, 'a search path that names its schema' ],
	[ q{CREATE TYPE public.fn_else AS ENUM ('a'); CREATE FUNCTION r_o(public.fn_else) RETURNS int LANGUAGE sql AS 'SELECT 1'}, 'another schema' ],
	[ q{CREATE TABLE t (a int); CREATE FUNCTION r_t() RETURNS bigint LANGUAGE sql BEGIN ATOMIC SELECT count(*) FROM t; END; ALTER TABLE t ADD CHECK (r_t() >= 0)}, 'a table needs it, and it needs a table' ],
	[ q{CREATE TABLE t (a int); CREATE FUNCTION r_t() RETURNS bigint LANGUAGE sql BEGIN ATOMIC SELECT count(*) FROM t; END; CREATE DOMAIN r_d AS bigint CHECK (VALUE > r_t())}, 'which needs a table' ],
	[ q{CREATE FUNCTION r_g() RETURNS int LANGUAGE sql AS 'SELECT 1'; GRANT EXECUTE ON FUNCTION r_g() TO x_person}, 'which cannot travel' ],
);
for my $case (@fn_refused)
{
	my ($sql, $why) = @$case;
	$node->safe_psql('postgres', qq{DROP SCHEMA IF EXISTS fn5 CASCADE; DROP TYPE IF EXISTS public.fn_else CASCADE; CREATE SCHEMA fn5; SET search_path = fn5, public; $sql});
	my ($rc4, $out4, $err4) = $node->psql('postgres', q{SELECT test_ws_obj_bytes('fn5')});
	isnt($rc4, 0, "refused: $sql");
	like($err4, qr/\Q$why\E/, "for why: $sql");
}

# an argument of a type holding names is no constant; a comment no client can show stops nothing
$node->safe_psql('postgres', q{DROP SCHEMA IF EXISTS fn9 CASCADE; CREATE SCHEMA fn9; CREATE TYPE fn9.holds AS (r regclass, n int);
	CREATE FUNCTION fn9.f(h fn9.holds, i int DEFAULT 1) RETURNS int LANGUAGE sql AS 'SELECT 1';
	CREATE FUNCTION fn9.g(h fn9.holds) RETURNS int LANGUAGE sql RETURN (h).n;
	COMMENT ON FUNCTION fn9.g(fn9.holds) IS U&'\\65E5\\672C'});
isnt($node->safe_psql('postgres', q{SET client_encoding = 'LATIN1'; SELECT length(test_ws_obj_bytes('fn9'))}), '0', 'a type holding names as an argument, under a client that cannot show a comment, is held');

# what restoring its grants takes, or a function meant to be closed is left open
$node->safe_psql('postgres', q{CREATE SCHEMA fn7 AUTHORIZATION g_fnowner; CREATE ROLE g_rs_noinherit NOLOGIN CREATEROLE; GRANT g_fnowner TO g_rs_noinherit WITH INHERIT FALSE, SET TRUE;
	GRANT CREATE, USAGE ON SCHEMA fn7 TO g_rs_noinherit; CREATE SCHEMA fn8 AUTHORIZATION g_fnowner;
	CREATE FUNCTION fn8.closed() RETURNS integer LANGUAGE sql AS 'SELECT 1'; ALTER FUNCTION fn8.closed() OWNER TO g_fnowner; REVOKE ALL ON FUNCTION fn8.closed() FROM PUBLIC;
	GRANT USAGE ON SCHEMA fn8 TO g_rs_noinherit});
($rc, $out, $err) = $node->psql('postgres', q{SET ROLE g_rs_noinherit; SELECT test_ws_obj_copy('fn8', 'fn7')});
like($err, qr/cannot restore the grants of "closed": this role does not have the privileges of its owner/, 'with no INHERIT on its owner, a restore of what is revoked refuses');
is($node->safe_psql('postgres', q{SELECT count(*) FROM pg_proc WHERE pronamespace = 'fn7'::regnamespace}), '0', 'and leaves nothing open');

# a function renamed while being read is one to try again, by key or by need
for my $change (q{ALTER FUNCTION fn.granted() OWNER TO g_tyowner}, q{ALTER FUNCTION fn.granted() OWNER TO g_fnowner}, q{COMMENT ON FUNCTION fn.granted() IS 'later'}, q{ALTER ROLE g_fnuser RENAME TO g_fnuser2}, q{ALTER ROLE g_fnuser2 RENAME TO g_fnuser},
	q{ALTER FUNCTION fn.granted() RENAME TO granted2}, q{DROP FUNCTION fn.granted2()})
{
	my $old = $node->background_psql('postgres', on_error_stop => 0);
	$old->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
	$old->query_safe('SELECT count(*) FROM pg_class');
	$node->safe_psql('postgres', $change);
	$old->{stderr} = '';
	$old->query(q{SELECT test_ws_obj_bytes('fn')});
	like($old->{stderr}, qr/has changed since/, "to try again after: $change");
	$old->quit;
}

# default privileges on functions refuse
$node->safe_psql('postgres', q{CREATE SCHEMA fn6; ALTER DEFAULT PRIVILEGES IN SCHEMA fn6 GRANT EXECUTE ON FUNCTIONS TO g_fnuser});
($rc, $out, $err) = $node->psql('postgres', q{SELECT test_ws_obj_copy('fn3', 'fn6')});
like($err, qr/default privileges would change what is made/, 'default privileges on functions refuse a restore');

# sequences: made before tables, owned by a column after them, with where they are
$node->safe_psql('postgres', q{
	CREATE ROLE g_sqowner NOLOGIN;
	CREATE ROLE g_squser NOLOGIN;
	CREATE SCHEMA sq AUTHORIZATION g_sqowner;
	SET ROLE g_sqowner;
	SET search_path = sq;
	CREATE SEQUENCE plain;
	CREATE SEQUENCE odd AS smallint INCREMENT BY -2 MINVALUE -100 MAXVALUE 50 START WITH 40 CACHE 3 CYCLE;
	CREATE SEQUENCE "Odd name";
	CREATE TABLE ser (id serial, other integer DEFAULT nextval('plain'), big bigserial);
	CREATE TABLE idt (i integer GENERATED ALWAYS AS IDENTITY);
	INSERT INTO idt DEFAULT VALUES;
	INSERT INTO idt DEFAULT VALUES;
	CREATE FUNCTION nexts() RETURNS bigint LANGUAGE sql BEGIN ATOMIC SELECT nextval('plain'::regclass); END;
	CREATE VIEW nv AS SELECT nextval('plain'::regclass) AS n;
	CREATE VIEW over_ident AS SELECT nextval('idt_i_seq'::regclass) AS n;
	CREATE FUNCTION next_ident() RETURNS bigint LANGUAGE sql BEGIN ATOMIC SELECT nextval('idt_i_seq'::regclass); END;
	COMMENT ON SEQUENCE odd IS 'it''s odd';
	GRANT USAGE, SELECT ON SEQUENCE plain TO g_squser;
	SELECT setval('plain', 41);
	SELECT setval('odd', 30, false);
});
my $sq_held = $node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('sq')});
my %sq_val = map { my ($k, $v) = split /\t/; $k => pack('H*', $v) } split /\n/, $sq_held;
ok(!$sq_val{'sequence/idt_i_seq'}, 'an identity sequence is its table\'s, not an object');
unlike($sq_val{'view/over_ident'}, qr/sequence\//, 'nor a need, being there with its table');
ok($sq_val{$_}, "$_ is held") for ('sequence/plain', 'sequence/odd', 'sequence/Odd name', 'sequence/ser_id_seq', 'sequence/ser_big_seq');
like($sq_val{'sequence/odd'}, qr/AS smallint INCREMENT BY -2 MINVALUE -100 MAXVALUE 50 START WITH 40 CACHE 3 CYCLE/, 'all of a sequence is said');
like($sq_val{'sequence/ser_id_seq'}, qr/ser.*id/s, 'and what column owns it');
like($sq_val{'view/nv'}, qr/sequence\/plain/, 'a view says it needs one');
like($sq_val{'function/nexts()'}, qr/sequence\/plain/, 'and a function');

$node->safe_psql('postgres', q{CREATE SCHEMA sq2 AUTHORIZATION g_sqowner});
$node->safe_psql('postgres', q{SELECT test_ws_obj_copy('sq', 'sq2', 'before')});
is($node->safe_psql('postgres', q{SELECT count(*) FROM pg_depend d JOIN pg_class c ON c.oid = d.objid WHERE c.relnamespace = 'sq2'::regnamespace AND c.relkind = 'S' AND d.deptype = 'a'}),
	'0', 'not yet owned, its column not being there');
$node->safe_psql('postgres', q{SET ROLE g_sqowner; CREATE TABLE sq2.ser (id integer NOT NULL DEFAULT nextval('sq2.ser_id_seq'), other integer DEFAULT nextval('sq2.plain'), big bigint NOT NULL DEFAULT nextval('sq2.ser_big_seq'));
	CREATE TABLE sq2.idt (i integer GENERATED ALWAYS AS IDENTITY)});
$node->safe_psql('postgres', q{SELECT test_ws_obj_copy('sq', 'sq2', 'after')});
is($node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('sq2')}), $sq_held, 'the same bytes restored');
is( $node->safe_psql('postgres', q{SELECT pg_get_serial_sequence('sq2.ser', 'id')}), 'sq2.ser_id_seq', 'owned by its column');
is( $node->safe_psql('postgres', q{SELECT count(*) FROM pg_proc WHERE oid = 'sq2.next_ident()'::regprocedure}), '1', 'a function needing an identity sequence is made after its table');
my $positions = $node->safe_psql('postgres', q{SELECT test_ws_seq_copy('sq', 'sq2')});
like($positions, qr/^plain 41 1$/m, 'where a sequence is, is read');
like($positions, qr/^odd 30 0$/m, 'and whether it has been called');
like($positions, qr/^idt_i_seq 2 1$/m, 'and an identity sequence\'s too');
is($node->safe_psql('postgres', q{INSERT INTO sq2.idt DEFAULT VALUES RETURNING i}), '3', 'so it goes on from there');
is($node->safe_psql('postgres', q{SELECT last_value, is_called FROM sq2.plain}), '41|t', 'and set');
is($node->safe_psql('postgres', q{SELECT nextval('sq2.odd')}), '30', 'so the next is the next');

# a table with a serial column is held, and names no schema
{
	my $branch = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
	local $ENV{PGOPTIONS} = "-c catalog_branch=$branch";
	$node->safe_psql('postgres', q{CREATE SCHEMA sv VERSIONED; CREATE TABLE sv.t (id serial PRIMARY KEY, n bigserial); CREATE SEQUENCE sv.free; CREATE TABLE sv.u (a bigint DEFAULT nextval('sv.free')); CREATE SCHEMA sv2 VERSIONED});
	my $t_bytes = pack('H*', unpack('H*', $node->safe_psql('postgres', q{SELECT encode(test_ws_schema_bytes('sv', 't'), 'escape')})));
	unlike($t_bytes, qr/sv\./, 'a table of serial columns does not name its schema');
	like($t_bytes, qr/t_id_seq/, 'and says its sequence');
	$node->safe_psql('postgres', q{SELECT test_ws_obj_copy('sv', 'sv2', 'before')});
	$node->safe_psql('postgres', q{SELECT test_ws_schema_create(test_ws_schema_bytes('sv', 't'), 'sv2'); SELECT test_ws_schema_create(test_ws_schema_bytes('sv', 'u'), 'sv2')});
	$node->safe_psql('postgres', q{SELECT test_ws_obj_copy('sv', 'sv2', 'after')});
	is($node->safe_psql('postgres', q{SELECT pg_get_serial_sequence('sv2.t', 'id')}), 'sv2.t_id_seq', 'restored, serial as it was');
	is($node->safe_psql('postgres', q{SELECT test_ws_schema_bytes('sv2', 't') = test_ws_schema_bytes('sv', 't')}), 't', 'and the same bytes');
}

my @sq_refused = (
	[ q{CREATE UNLOGGED SEQUENCE r_u}, 'not logged' ],
	[ q{CREATE SEQUENCE r_g; GRANT USAGE ON SEQUENCE r_g TO x_person}, 'which cannot travel' ],
	[ q{CREATE VIEW r_txt AS SELECT nextval('sq5.x'::text) AS n}, "say a schema's name" ],
	[ q{CREATE TABLE t (i integer GENERATED ALWAYS AS IDENTITY); CREATE DOMAIN r_dm AS bigint DEFAULT nextval('t_i_seq'::regclass)}, 'no commit holds yet' ],
	[ q{CREATE SEQUENCE r_c; GRANT SELECT (last_value) ON TABLE r_c TO g_squser}, 'a grant on a column' ],
	[ q{CREATE SEQUENCE r_r; REVOKE UPDATE ON SEQUENCE r_r FROM CURRENT_USER}, 'rights have been changed' ],
	[ q{CREATE FOREIGN DATA WRAPPER fdw_sq; CREATE SERVER srv_sq FOREIGN DATA WRAPPER fdw_sq; CREATE FOREIGN TABLE ft (a int) SERVER srv_sq; CREATE SEQUENCE r_f OWNED BY ft.a}, 'what no commit holds' ],
);
for my $case (@sq_refused)
{
	my ($sql, $why) = @$case;
	$node->safe_psql('postgres', qq{DROP SCHEMA IF EXISTS sq5 CASCADE; DROP SERVER IF EXISTS srv_sq CASCADE; DROP FOREIGN DATA WRAPPER IF EXISTS fdw_sq CASCADE; CREATE SCHEMA sq5; SET search_path = sq5, public; $sql});
	my ($rc5, $out5, $err5) = $node->psql('postgres', q{SELECT test_ws_obj_bytes('sq5')});
	isnt($rc5, 0, "refused: $sql");
	like($err5, qr/\Q$why\E/, "for why: $sql");
}

# triggers, rules and policies: of their tables, made after them
$node->safe_psql('postgres', q{
	CREATE ROLE g_trowner NOLOGIN;
	CREATE ROLE g_trread NOLOGIN;
	CREATE SCHEMA tr AUTHORIZATION g_trowner;
	SET ROLE g_trowner;
	SET search_path = tr;
	CREATE FUNCTION trf() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN RETURN NEW; END $$;
	CREATE TABLE t (a integer PRIMARY KEY, b text);
	CREATE TABLE audit (a integer);
	CREATE TABLE p (a integer) PARTITION BY RANGE (a);
	CREATE TABLE p1 PARTITION OF p FOR VALUES FROM (0) TO (10);
	CREATE VIEW v AS SELECT a, b FROM t;
	CREATE TRIGGER before_ins BEFORE INSERT OR UPDATE OF b ON t FOR EACH ROW WHEN (NEW.a > 0) EXECUTE FUNCTION trf();
	CREATE TRIGGER "Odd name" AFTER DELETE ON t REFERENCING OLD TABLE AS gone FOR EACH STATEMENT EXECUTE FUNCTION trf('x', 'it''s @');
	CREATE CONSTRAINT TRIGGER con AFTER INSERT ON t DEFERRABLE INITIALLY DEFERRED FOR EACH ROW EXECUTE FUNCTION trf();
	CREATE TRIGGER on_view INSTEAD OF INSERT ON v FOR EACH ROW EXECUTE FUNCTION trf();
	CREATE TRIGGER on_parted BEFORE INSERT ON p FOR EACH ROW EXECUTE FUNCTION trf();
	CREATE TRIGGER off_for_now BEFORE INSERT ON t FOR EACH ROW EXECUTE FUNCTION trf();
	CREATE TRIGGER off_parted BEFORE INSERT ON p FOR EACH ROW EXECUTE FUNCTION trf();
	ALTER TABLE p DISABLE TRIGGER off_parted;
	CREATE TRIGGER replica_only BEFORE INSERT ON t FOR EACH ROW EXECUTE FUNCTION trf();
	CREATE TRIGGER always_on BEFORE INSERT ON t FOR EACH ROW EXECUTE FUNCTION trf();
	CREATE TRIGGER " ON tr.t x" BEFORE UPDATE ON t FOR EACH ROW EXECUTE FUNCTION trf();
	CREATE RULE off_rule AS ON DELETE TO t DO ALSO NOTIFY watchers;
	CREATE RULE " TO tr.t y" AS ON INSERT TO t DO ALSO NOTIFY watchers;
	ALTER TABLE t DISABLE TRIGGER off_for_now;
	ALTER TABLE t ENABLE REPLICA TRIGGER replica_only;
	ALTER TABLE t ENABLE ALWAYS TRIGGER always_on;
	ALTER TABLE t DISABLE RULE off_rule;
	CREATE RULE log_it AS ON UPDATE TO t WHERE NEW.a > 1 DO ALSO INSERT INTO audit VALUES (NEW.a);
	CREATE RULE noop AS ON DELETE TO audit DO INSTEAD NOTHING;
	CREATE RULE view_rule AS ON DELETE TO v DO INSTEAD DELETE FROM t WHERE a = OLD.a;
	CREATE RULE notifies AS ON INSERT TO audit DO ALSO NOTIFY watchers;
	ALTER TABLE t ENABLE ROW LEVEL SECURITY;
	CREATE POLICY mine ON t USING (a > 0);
	CREATE POLICY read_it ON t AS RESTRICTIVE FOR SELECT TO g_trread, g_trowner USING (b <> 'x') ;
	CREATE POLICY ins ON t FOR INSERT TO g_trread WITH CHECK (a < 100);
	COMMENT ON TRIGGER before_ins ON t IS 'a trigger';
	COMMENT ON RULE noop ON audit IS 'a rule';
	COMMENT ON POLICY mine ON t IS 'a policy';
});
my $tr_held = $node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('tr')});
my %tr_val = map { my ($k, $v) = split /\t/; $k => pack('H*', $v) } split /\n/, $tr_held;
ok($tr_val{$_}, "$_ is held") for ('trigger/before_ins(t)', 'trigger/Odd name(t)', 'trigger/con(t)', 'trigger/on_view(v)', 'trigger/on_parted(p)',
	'rule/log_it(t)', 'rule/noop(audit)', 'rule/view_rule(v)', 'rule/notifies(audit)', 'policy/mine(t)', 'policy/read_it(t)', 'policy/ins(t)');
ok(!$tr_val{'trigger/on_parted(p1)'}, 'a partition\'s clone is its table\'s');
like($tr_val{'trigger/ ON tr.t x(t)'}, qr/x" BEFORE UPDATE ON t FOR/, 'a name like the text of its table is left, and its table is bare');
like($tr_val{'rule/ TO tr.t y(t)'}, qr/ON INSERT TO t DO/, 'as for a rule');
unlike(join('', map { $tr_val{$_} } grep { !/ tr\.t / } keys %tr_val), qr/\btr\./, 'none else names its schema');
like($tr_val{'trigger/before_ins(t)'}, qr/function\/trf\(\)/, 'a trigger says its function is needed');
like($tr_val{'trigger/on_view(v)'}, qr/view\/v/, 'and what view it is on');
like($tr_val{'rule/view_rule(v)'}, qr/view\/v/, 'as a rule does');

$node->safe_psql('postgres', q{CREATE SCHEMA tr2 AUTHORIZATION g_trowner;
	SET ROLE g_trowner; SET search_path = tr2; CREATE TABLE t (a integer PRIMARY KEY, b text); CREATE TABLE audit (a integer);
	CREATE TABLE p (a integer) PARTITION BY RANGE (a); CREATE TABLE p1 PARTITION OF p FOR VALUES FROM (0) TO (10);
	ALTER TABLE t ENABLE ROW LEVEL SECURITY});
$node->safe_psql('postgres', q{SELECT test_ws_obj_copy('tr', 'tr2')});
is($node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('tr2')}), $tr_held, 'the same bytes restored');
my $tr_dump = sub {
	my ($s) = @_;
	my $out = $node->safe_psql('postgres', qq{
		SELECT string_agg(x, E'\\n' ORDER BY x) FROM (
			SELECT pg_get_triggerdef(g.oid) AS x FROM pg_trigger g JOIN pg_class c ON c.oid = g.tgrelid WHERE c.relnamespace = '$s'::regnamespace AND NOT g.tgisinternal
			UNION ALL SELECT pg_get_ruledef(w.oid) FROM pg_rewrite w JOIN pg_class c ON c.oid = w.ev_class WHERE c.relnamespace = '$s'::regnamespace AND w.rulename <> '_RETURN'
			UNION ALL SELECT format('%s %s %s %s %s %s %s', policyname, tablename, permissive, roles, cmd, qual, with_check) FROM pg_policies WHERE schemaname = '$s'
			UNION ALL SELECT obj_description(g.oid, 'pg_trigger') FROM pg_trigger g JOIN pg_class c ON c.oid = g.tgrelid WHERE c.relnamespace = '$s'::regnamespace AND obj_description(g.oid, 'pg_trigger') IS NOT NULL) y});
	$out =~ s/ ON tr\.t x/NAME1/g;
	$out =~ s/ TO tr\.t y/NAME2/g;
	$out =~ s/\b\Q$s\E\.//g;
	return $out;
};
is($tr_dump->('tr2'), $tr_dump->('tr'), 'the same by the catalogs');
is( $node->safe_psql('postgres', q{SELECT count(*) FROM pg_trigger g WHERE g.tgrelid = 'tr2.p1'::regclass AND tgname = 'on_parted'}), '1', 'a partition has its clone');
is( $node->safe_psql('postgres', q{SELECT string_agg(tgname || ' ' || tgenabled::text, ', ' ORDER BY tgname) FROM pg_trigger WHERE tgrelid = 'tr2.t'::regclass AND tgname IN ('off_for_now', 'replica_only', 'always_on')}),
	'always_on A, off_for_now D, replica_only R', 'a trigger fires as it did');
is($node->safe_psql('postgres', q{SELECT ev_enabled FROM pg_rewrite WHERE rulename = 'off_rule' AND ev_class = 'tr2.t'::regclass}), 'D', 'and a rule');
is($node->safe_psql('postgres', q{SELECT tgenabled FROM pg_trigger WHERE tgrelid = 'tr2.p1'::regclass AND tgname = 'off_parted'}), 'D', 'and a partition\'s clone goes with its table\'s');

my @tr_refused = (
	[ q{CREATE TABLE pg_stats (a int); CREATE RULE r AS ON DELETE TO tr5.pg_stats DO INSTEAD NOTHING}, 'named as one in the catalogs' ],
	[ q{CREATE FUNCTION f() RETURNS trigger LANGUAGE plpgsql AS 'BEGIN RETURN NEW; END'; CREATE TABLE t (a int); CREATE VIEW v AS SELECT a FROM t; CREATE CONSTRAINT TRIGGER g AFTER INSERT ON t FROM v FOR EACH ROW EXECUTE FUNCTION f()}, 'from what is no table' ],
	[ q{CREATE FUNCTION f() RETURNS trigger LANGUAGE plpgsql AS 'BEGIN RETURN NEW; END'; CREATE TABLE p (a int) PARTITION BY RANGE (a); CREATE TABLE p1 PARTITION OF p FOR VALUES FROM (0) TO (9); CREATE TRIGGER g BEFORE INSERT ON p FOR EACH ROW EXECUTE FUNCTION f(); COMMENT ON TRIGGER g ON p1 IS 'x'}, 'a comment of its own' ],
	[ q{CREATE TABLE pg_class (a int); CREATE POLICY y ON tr5.pg_class USING (true)}, 'named as one in the catalogs' ],
	[ q{CREATE FUNCTION f() RETURNS trigger LANGUAGE plpgsql AS 'BEGIN RETURN NEW; END'; CREATE TABLE p (a int) PARTITION BY RANGE (a); CREATE TABLE p1 PARTITION OF p FOR VALUES FROM (0) TO (9); CREATE TRIGGER g BEFORE INSERT ON p FOR EACH ROW EXECUTE FUNCTION f(); ALTER TABLE p1 DISABLE TRIGGER g}, 'fires otherwise than its table' ],
	[ q{CREATE FUNCTION f() RETURNS trigger LANGUAGE plpgsql AS 'BEGIN RETURN NEW; END'; CREATE FOREIGN DATA WRAPPER fdw_tr; CREATE SERVER srv_tr FOREIGN DATA WRAPPER fdw_tr; CREATE FOREIGN TABLE ft (a int) SERVER srv_tr; CREATE TRIGGER g BEFORE INSERT ON ft FOR EACH ROW EXECUTE FUNCTION f()}, 'what no commit holds' ],
	[ q{CREATE TYPE "text" AS ENUM ('a'); CREATE TABLE t (a int); CREATE POLICY y ON t USING (NULL::tr5."text" IS NULL)}, "say a schema's name" ],
	[ q{CREATE TYPE "text" AS ENUM ('a'); CREATE TABLE t (a int); CREATE RULE r AS ON DELETE TO t WHERE NULL::tr5."text" IS NULL DO ALSO DELETE FROM t}, "say a schema's name" ],
	[ q{CREATE TABLE t (a int); CREATE POLICY y ON t TO x_person USING (true)}, 'a role that cannot travel' ],
	[ q{CREATE TABLE t (a int); CREATE TRIGGER g BEFORE INSERT ON t FOR EACH ROW EXECUTE FUNCTION public.tr_else()}, 'another schema' ],
	[ q{CREATE TABLE t (a int); CREATE POLICY y ON t USING (a > 0 AND public.tr_pred())}, 'another schema' ],
	[ q{CREATE TABLE t (a int); CREATE RULE r AS ON DELETE TO t DO ALSO DELETE FROM public.tr_tab}, 'another schema' ],
);
$node->safe_psql('postgres', q{CREATE FUNCTION public.tr_else() RETURNS trigger LANGUAGE plpgsql AS 'BEGIN RETURN NEW; END'; CREATE FUNCTION public.tr_pred() RETURNS boolean LANGUAGE sql AS 'SELECT true'; CREATE TABLE public.tr_tab (a int)});
for my $case (@tr_refused)
{
	my ($sql, $why) = @$case;
	$node->safe_psql('postgres', qq{DROP SCHEMA IF EXISTS tr5 CASCADE; DROP SERVER IF EXISTS srv_tr CASCADE; DROP FOREIGN DATA WRAPPER IF EXISTS fdw_tr CASCADE; CREATE SCHEMA tr5; SET search_path = tr5; $sql});
	my ($rc6, $out6, $err6) = $node->psql('postgres', q{SELECT test_ws_obj_bytes('tr5')});
	isnt($rc6, 0, "refused: $sql");
	like($err6, qr/\Q$why\E/, "for why: $sql");
}

# a policy's role whose name another encoding on this cluster may read otherwise
$node->safe_psql('postgres', q{CREATE DATABASE trlatin TEMPLATE template0 ENCODING 'LATIN1' LOCALE 'C'});
$node->safe_psql('postgres', q{CREATE ROLE "g_é" NOLOGIN; CREATE SCHEMA tr9; CREATE TABLE tr9.t (a int); CREATE POLICY y ON tr9.t TO "g_é" USING (true)});
($rc, $out, $err) = $node->psql('postgres', q{SELECT test_ws_obj_bytes('tr9')});
like($err, qr/names a role not in ASCII/, 'a policy\'s role not in ASCII, in a cluster of mixed encodings, is refused');
$node->safe_psql('postgres', q{DROP SCHEMA tr9 CASCADE; DROP ROLE "g_é"; DROP DATABASE trlatin});

# since a snapshot, one to try again
for my $change (q{COMMENT ON POLICY mine ON tr.t IS 'later'}, q{COMMENT ON TRIGGER before_ins ON tr.t IS 'later'}, q{COMMENT ON RULE noop ON tr.audit IS 'later'},
	q{ALTER POLICY ins ON tr.t RENAME TO ins2}, q{ALTER ROLE g_trread RENAME TO g_trread2}, q{ALTER ROLE g_trread2 RENAME TO g_trread}, q{ALTER TRIGGER con ON tr.t RENAME TO con2})
{
	my $old = $node->background_psql('postgres', on_error_stop => 0);
	$old->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
	$old->query_safe('SELECT count(*) FROM pg_class');
	$node->safe_psql('postgres', $change);
	$old->{stderr} = '';
	$old->query(q{SELECT test_ws_obj_bytes('tr')});
	like($old->{stderr}, qr/has changed since/, "to try again after: $change");
	$old->quit;
}

# a materialized view's indexes and a view's column defaults
$node->safe_psql('postgres', q{
	CREATE ROLE g_veowner NOLOGIN;
	CREATE SCHEMA ve AUTHORIZATION g_veowner;
	SET ROLE g_veowner;
	SET search_path = ve;
	CREATE FUNCTION vf(integer) RETURNS integer LANGUAGE sql IMMUTABLE AS 'SELECT $1 + 1';
	CREATE TABLE t (a integer, b text);
	CREATE MATERIALIZED VIEW m AS SELECT a, b FROM t WITH NO DATA;
	CREATE UNIQUE INDEX m_a ON m (a);
	CREATE INDEX "Odd ix" ON m USING btree (b COLLATE "C" text_pattern_ops DESC NULLS LAST) INCLUDE (a) WITH (fillfactor = 50) WHERE a > 0;
	CREATE INDEX m_expr ON m (vf(a));
	COMMENT ON INDEX m_a IS 'an index';
	CREATE VIEW v AS SELECT a, b FROM t;
	ALTER VIEW v ALTER COLUMN a SET DEFAULT 7;
	ALTER VIEW v ALTER COLUMN b SET DEFAULT 'it''s @ here';
});
my $ve_held = $node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('ve')});
my %ve_val = map { my ($k, $v) = split /\t/; $k => pack('H*', $v) } split /\n/, $ve_held;
ok($ve_val{$_}, "$_ is held") for ('index/m_a(m)', 'index/Odd ix(m)', 'index/m_expr(m)', 'default/a(v)', 'default/b(v)');
unlike(join('', values %ve_val), qr/\bve\./, 'none names its schema');
like($ve_val{'index/m_a(m)'}, qr/matview\/m/, 'an index needs its materialized view');
like($ve_val{'index/m_expr(m)'}, qr/function\/vf\(integer\)/, 'and what it calls');
like($ve_val{'default/a(v)'}, qr/view\/v/, 'and a default its view');
like($ve_val{'index/m_a(m)'}, qr/an index/, 'an index has its comment');
like($ve_val{'default/b(v)'}, qr/it''s @@ here/, 'with its @ doubled');

$node->safe_psql('postgres', q{CREATE SCHEMA ve2 AUTHORIZATION g_veowner; SET ROLE g_veowner; CREATE TABLE ve2.t (a integer, b text)});
$node->safe_psql('postgres', q{SELECT test_ws_obj_copy('ve', 've2')});
is($node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('ve2')}), $ve_held, 'the same bytes restored');
my $ve_dump = sub {
	my ($s) = @_;
	my $out = $node->safe_psql('postgres', qq{SELECT string_agg(x, E'\\n' ORDER BY x) FROM (
		SELECT pg_get_indexdef(i.indexrelid) AS x FROM pg_index i JOIN pg_class c ON c.oid = i.indrelid WHERE c.relnamespace = '$s'::regnamespace AND c.relkind = 'm'
		UNION ALL SELECT a.attname || ' ' || pg_get_expr(d.adbin, d.adrelid) FROM pg_attrdef d JOIN pg_class c ON c.oid = d.adrelid
			JOIN pg_attribute a ON a.attrelid = d.adrelid AND a.attnum = d.adnum WHERE c.relnamespace = '$s'::regnamespace AND c.relkind = 'v') y});
	$out =~ s/\b\Q$s\E\.//g;
	return $out;
};
is($ve_dump->('ve2'), $ve_dump->('ve'), 'the same by the catalogs');

# a view default on a function needing a view is no table's need
$node->safe_psql('postgres', q{CREATE SCHEMA ve6 AUTHORIZATION g_veowner; SET ROLE g_veowner; SET search_path = ve6;
	CREATE TABLE t (a integer); CREATE VIEW v AS SELECT a FROM t; CREATE VIEW w AS SELECT 1 AS x;
	CREATE FUNCTION f() RETURNS integer LANGUAGE sql BEGIN ATOMIC SELECT x FROM w; END;
	ALTER VIEW v ALTER COLUMN a SET DEFAULT f()});
like($node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('ve6')}), qr/^default\/a\(v\)\t/m, 'a default on a function that needs a view is held');

# comments go before owners: who restores may not own an index by the time of its own
$node->safe_psql('postgres', q{CREATE ROLE g_ve_noinherit NOLOGIN; GRANT g_veowner TO g_ve_noinherit WITH INHERIT FALSE, SET TRUE;
	CREATE SCHEMA ve7 AUTHORIZATION g_veowner; GRANT CREATE, USAGE ON SCHEMA ve7 TO g_ve_noinherit;
	CREATE SCHEMA ve8 AUTHORIZATION g_veowner; GRANT USAGE ON SCHEMA ve8 TO g_ve_noinherit;
	SET ROLE g_veowner; CREATE TABLE ve8.t (a integer); CREATE MATERIALIZED VIEW ve8.m AS SELECT a FROM ve8.t WITH NO DATA; CREATE INDEX i ON ve8.m (a); COMMENT ON INDEX ve8.i IS 'on it';
	GRANT SELECT ON ve8.t TO g_ve_noinherit; CREATE TABLE ve7.t (a integer); GRANT SELECT ON ve7.t TO g_ve_noinherit});
is($node->psql('postgres', q{SET ROLE g_ve_noinherit; SELECT test_ws_obj_copy('ve8', 've7')}), 0, 'a restorer with SET and no INHERIT on the owner can comment on an index');
is($node->safe_psql('postgres', q{SELECT obj_description('ve7.i'::regclass, 'pg_class')}), 'on it', 'and the comment arrives');

# since a snapshot, one to try again
for my $change (q{ALTER VIEW ve.v ALTER COLUMN a SET DEFAULT 8}, q{ALTER INDEX ve.m_a SET (fillfactor = 60)}, q{COMMENT ON INDEX ve.m_a IS 'later'})
{
	my $old = $node->background_psql('postgres', on_error_stop => 0);
	$old->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
	$old->query_safe('SELECT count(*) FROM pg_class');
	$node->safe_psql('postgres', $change);
	$old->{stderr} = '';
	$old->query(q{SELECT test_ws_obj_bytes('ve')});
	like($old->{stderr}, qr/has changed since/, "to try again after: $change");
	$old->quit;
}

my @ve_refused = (
	[ q{CREATE TABLE t (a int); CREATE MATERIALIZED VIEW m AS SELECT a FROM t; CREATE UNIQUE INDEX ix ON m (a); CLUSTER m USING ix}, 'clustered' ],
	[ q{CREATE TABLE t (a int); CREATE MATERIALIZED VIEW m AS SELECT a FROM t; CREATE INDEX ix ON m (public.ve_else(a))}, 'another schema' ],
	[ q{CREATE TABLE t (a int); CREATE VIEW v AS SELECT a FROM t; ALTER VIEW v ALTER COLUMN a SET DEFAULT public.ve_else(1)}, 'another schema' ],
	[ q{CREATE TABLE t (a int); CREATE MATERIALIZED VIEW m AS SELECT a FROM t; CREATE INDEX ix ON m ((a + 1)); ALTER INDEX ix ALTER COLUMN 1 SET STATISTICS 5}, 'its own statistics' ],
	[ q{CREATE TABLE t (a int); CREATE MATERIALIZED VIEW m AS SELECT a FROM t; CREATE INDEX ix ON m (a) TABLESPACE ve_ts}, 'in a tablespace' ],
	[ q{CREATE TYPE "date" AS ENUM ('a'); CREATE TABLE t (a int); CREATE MATERIALIZED VIEW m AS SELECT a FROM t; CREATE INDEX ix ON m (a) WHERE 'a'::ve5."date" IS NOT NULL}, "say a schema's name" ],
	[ q{CREATE TYPE "date" AS ENUM ('a'); CREATE TABLE t (a int); CREATE VIEW v AS SELECT a FROM t; ALTER VIEW v ALTER COLUMN a SET DEFAULT ('a'::ve5."date" IS NOT NULL)::int}, "say a schema's name" ],
);
$node->safe_psql('postgres', q{CREATE FUNCTION public.ve_else(integer) RETURNS integer LANGUAGE sql IMMUTABLE AS 'SELECT $1'});
{
	my $ts_dir = PostgreSQL::Test::Utils::tempdir();
	$node->safe_psql('postgres', qq{CREATE TABLESPACE ve_ts LOCATION '$ts_dir'});
}
for my $case (@ve_refused)
{
	my ($sql, $why) = @$case;
	$node->safe_psql('postgres', qq{DROP SCHEMA IF EXISTS ve5 CASCADE; CREATE SCHEMA ve5; CREATE ROLE g_ve5 NOLOGIN; ALTER SCHEMA ve5 OWNER TO g_ve5; SET search_path = ve5; $sql; ALTER MATERIALIZED VIEW IF EXISTS ve5.m OWNER TO g_ve5; ALTER VIEW IF EXISTS ve5.v OWNER TO g_ve5});
	my ($rc7, $out7, $err7) = $node->psql('postgres', q{SELECT test_ws_obj_bytes('ve5')});
	isnt($rc7, 0, "refused: $sql");
	like($err7, qr/\Q$why\E/, "for why: $sql");
	$node->safe_psql('postgres', q{DROP SCHEMA ve5 CASCADE; DROP ROLE g_ve5});
}

# collations, casts and conversions
$node->safe_psql('postgres', q{
	CREATE ROLE g_coowner NOLOGIN;
	CREATE SCHEMA co AUTHORIZATION g_coowner;
	CREATE COLLATION co.mine (PROVIDER = libc, LC_COLLATE = 'C', LC_CTYPE = 'C');
	CREATE COLLATION co."Odd name" FROM "POSIX";
	ALTER COLLATION co.mine OWNER TO g_coowner;
	COMMENT ON COLLATION co.mine IS 'it''s mine';
	CREATE TYPE co.mood AS ENUM ('a', 'b');
	CREATE DOMAIN co.dom AS text COLLATE co.mine;
	CREATE TYPE co.coll_range AS RANGE (SUBTYPE = text, COLLATION = co.mine);
	CREATE TYPE co.pair AS (t text COLLATE co.mine);
	CREATE FUNCTION co.m2t(co.mood) RETURNS text LANGUAGE sql IMMUTABLE AS 'SELECT $1::text';
	CREATE CAST (co.mood AS text) WITH FUNCTION co.m2t(co.mood) AS ASSIGNMENT;
	CREATE CAST (co.mood AS integer) WITH INOUT;
	CREATE CAST (co.mood[] AS text[]) WITH INOUT AS IMPLICIT;
	COMMENT ON CAST (co.mood AS integer) IS 'in and out';
	CREATE CONVERSION co.conv FOR 'LATIN1' TO 'UTF8' FROM iso8859_1_to_utf8;
	ALTER CONVERSION co.conv OWNER TO g_coowner;
	CREATE FUNCTION co.fc(a text) RETURNS text LANGUAGE sql IMMUTABLE BEGIN ATOMIC SELECT a COLLATE co.mine AS x; END;
	COMMENT ON CONVERSION co.conv IS 'a conversion';
	CREATE TABLE co.t (a text COLLATE co.mine, m co.mood);
	CREATE VIEW co.v AS SELECT a COLLATE co.mine AS c, m::integer AS i FROM co.t;
	ALTER VIEW co.v OWNER TO g_coowner;
	ALTER TABLE co.t OWNER TO g_coowner;
	CREATE MATERIALIZED VIEW co.m AS SELECT a FROM co.t WITH NO DATA;
	ALTER MATERIALIZED VIEW co.m OWNER TO g_coowner;
	CREATE INDEX mi ON co.m (a COLLATE co.mine);
});
my $co_held = $node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('co')});
my %co_val = map { my ($k, $v) = split /\t/; $k => pack('H*', $v) } split /\n/, $co_held;
ok($co_val{$_}, "$_ is held") for ('collation/mine', 'collation/Odd name', 'cast/mood(text)', 'cast/mood(integer)', 'cast/mood[](text[])', 'conversion/conv', 'domain/dom', 'index/mi(m)');
unlike(join('', values %co_val), qr/\bco\./, 'none names its schema');
like($co_val{'collation/mine'}, qr/PROVIDER = libc, LC_COLLATE = 'C', LC_CTYPE = 'C'/, 'a collation is said');
like($co_val{'domain/dom'}, qr/collation\/mine/, 'a domain needs its collation');
like($co_val{'function/fc(text)'}, qr/collation\/mine/, 'and a function');
like($co_val{'collation/mine'}, qr/g_coowner/, 'a collation has its owner');
like($co_val{'conversion/conv'}, qr/g_coowner/, 'and a conversion');
like($co_val{'index/mi(m)'}, qr/collation\/mine/, 'and an index');
like($co_val{'cast/mood(text)'}, qr/WITH FUNCTION m2t\(mood\) AS ASSIGNMENT.*function\/m2t\(mood\).*type\/mood/s, 'a cast says its function, and needs it');

$node->safe_psql('postgres', q{CREATE SCHEMA co2 AUTHORIZATION g_coowner});
$co_held = $node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('co')});
$node->safe_psql('postgres', q{SELECT test_ws_obj_copy('co', 'co2', 'before')});
$node->safe_psql('postgres', q{SET ROLE g_coowner; CREATE TABLE co2.t (a text COLLATE co2.mine, m co2.mood)});
$node->safe_psql('postgres', q{SELECT test_ws_obj_copy('co', 'co2', 'after')});
is($node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('co2')}), $co_held, 'the same bytes restored, with collations and a conversion');
is($node->safe_psql('postgres', q{SELECT (i IS NULL)::text FROM co2.v}), '', 'a view on a cast nothing records its need of is made, the cast being earlier');

# a cast's name is a type's as printed, which may be longer than any name
{
	my $long = 'X' x 63;
	$node->safe_psql('postgres', qq{CREATE SCHEMA cl; CREATE SCHEMA cl2; CREATE TYPE cl."$long" AS ENUM ('a');
		CREATE CAST (cl."$long"[] AS text) WITH INOUT; COMMENT ON CAST (cl."$long"[] AS text) IS 'long'});
	my $cl_held = $node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('cl')});
	$node->safe_psql('postgres', q{SELECT test_ws_obj_copy('cl', 'cl2')});
	is($node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('cl2')}), $cl_held, 'a cast of a long type\'s array is held');
}

# what uses a cast with no record of it is made after it, whatever its name
$node->safe_psql('postgres', q{CREATE SCHEMA cz; CREATE SCHEMA cz2; CREATE TYPE cz.z AS ENUM ('1');
	CREATE CAST (cz.z AS integer) WITH INOUT;
	CREATE DOMAIN cz.a AS integer CHECK (VALUE <> ('1'::cz.z)::integer);
	CREATE FUNCTION cz.a2() RETURNS integer LANGUAGE sql BEGIN ATOMIC SELECT ('1'::cz.z)::integer; END});
my $cz_held = $node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('cz')});
$node->safe_psql('postgres', q{SELECT test_ws_obj_copy('cz', 'cz2')});
is($node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('cz2')}), $cz_held, 'a domain and a function using a cast are made after it');

# a cast restores in another schema's name, where its type is another type
$node->safe_psql('postgres', q{CREATE SCHEMA ca; CREATE TYPE ca.e AS ENUM ('a'); CREATE CAST (ca.e AS integer) WITH INOUT;
	CREATE FUNCTION ca.f(ca.e) RETURNS bigint LANGUAGE sql IMMUTABLE AS 'SELECT 1'; CREATE CAST (ca.e AS bigint) WITH FUNCTION ca.f(ca.e) AS IMPLICIT;
	COMMENT ON CAST (ca.e AS integer) IS 'x'; CREATE SCHEMA ca2});
my $ca_held = $node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('ca')});
$node->safe_psql('postgres', q{SELECT test_ws_obj_copy('ca', 'ca2')});
is($node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('ca2')}), $ca_held, 'a cast is the same bytes restored');
is( $node->safe_psql('postgres', q{SELECT castcontext::text, castmethod::text FROM pg_cast WHERE castsource = 'ca2.e'::regtype AND casttarget = 'bigint'::regtype}),
	'i|f', 'and as it was');
is($node->safe_psql('postgres', q{SELECT obj_description(oid, 'pg_cast') FROM pg_cast WHERE castsource = 'ca2.e'::regtype AND casttarget = 'integer'::regtype}), 'x', 'with its comment');

# who holds which collations is each server's, so what capture found free may not be free
$node->safe_psql('postgres', q{CREATE SCHEMA cf; SELECT test_ws_obj_restore_collation('here_only', 'cf')});
is($node->safe_psql('postgres', q{SELECT count(*) FROM pg_collation WHERE collname = 'here_only' AND collnamespace = 'cf'::regnamespace}), '1', 'a collation arrives where its name is free');
$node->safe_psql('postgres', q{DROP COLLATION cf.here_only; SET allow_system_table_mods = on; CREATE COLLATION pg_catalog.here_only FROM "C"});
($rc, $out, $err) = $node->psql('postgres', q{SELECT test_ws_obj_restore_collation('here_only', 'cf')});
like($err, qr/named as one in this server's catalogs/, 'and is refused where the catalogs have it');
is($node->safe_psql('postgres', q{SELECT count(*) FROM pg_collation WHERE collnamespace = 'cf'::regnamespace}), '0', 'with nothing made');
# one in the catalogs for another encoding is no name this database can mean
$node->safe_psql('postgres', q{SET allow_system_table_mods = on; UPDATE pg_collation SET collencoding = CASE WHEN getdatabaseencoding() = 'LATIN1' THEN 6 ELSE 8 END
	WHERE collname = 'here_only' AND collnamespace = 'pg_catalog'::regnamespace});
$node->safe_psql('postgres', q{SELECT test_ws_obj_restore_collation('here_only', 'cf')});
like($node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('cf')}), qr/^collation\/here_only\t/m, 'a name the catalogs have only for another encoding is free');
$node->safe_psql('postgres', q{DROP COLLATION cf.here_only; SET allow_system_table_mods = on;
	UPDATE pg_collation SET collencoding = -1 WHERE collname = 'here_only' AND collnamespace = 'pg_catalog'::regnamespace});
$node->safe_psql('postgres', q{SET allow_system_table_mods = on; DROP COLLATION pg_catalog.here_only; DROP SCHEMA cf});

# every provider's say is the same read back
{
	my $icu = $node->safe_psql('postgres', q{SELECT count(*) FROM pg_collation WHERE collprovider = 'i'}) > 0;
	$node->safe_psql('postgres', q{CREATE SCHEMA cp; CREATE SCHEMA cp2; CREATE COLLATION cp.b (PROVIDER = builtin, LOCALE = 'C')});
	$node->safe_psql('postgres', q{CREATE COLLATION cp.i (PROVIDER = icu, LOCALE = 'und'); CREATE COLLATION cp.n (PROVIDER = icu, LOCALE = 'und', DETERMINISTIC = false);
		CREATE COLLATION cp.r (PROVIDER = icu, LOCALE = 'und', RULES = '&a < b')}) if $icu;
	my $cp_held = $node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('cp')});
	$node->safe_psql('postgres', q{SELECT test_ws_obj_copy('cp', 'cp2')});
	is($node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('cp2')}), $cp_held, 'a collation of each provider is the same bytes restored');
	like($cp_held, qr/^collation\/b\t/m, 'a builtin one is held');
	if ($icu)
	{
		is( $node->safe_psql('postgres', q{SELECT string_agg(format('%s %s %s', collname, collisdeterministic, collicurules), ', ' ORDER BY collname) FROM pg_collation WHERE collnamespace = 'cp2'::regnamespace AND collprovider = 'i'}),
			'i t , n f , r t &a < b', 'and ICU\'s as they were');
	}
}

# what belongs to an extension is the extension's to bring
for my $x (
	[ q{CREATE COLLATION cx.c FROM "C"}, 'COLLATION cx.c' ],
	[ q{CREATE CONVERSION cx.c FOR 'LATIN1' TO 'UTF8' FROM iso8859_1_to_utf8}, 'CONVERSION cx.c' ],
	[ q{CREATE TYPE cx.e AS ENUM ('a'); CREATE CAST (cx.e AS integer) WITH INOUT}, 'CAST (cx.e AS integer)' ])
{
	my ($make, $member) = @$x;
	$node->safe_psql('postgres', qq{CREATE SCHEMA cx; $make; ALTER EXTENSION test_prolly ADD $member});
	my ($rcx, $outx, $errx) = $node->psql('postgres', q{SELECT test_ws_obj_bytes('cx')});
	like($errx, qr/belongs to an extension/, "refused: $member");
	$node->safe_psql('postgres', qq{ALTER EXTENSION test_prolly DROP $member; DROP SCHEMA cx CASCADE});
}

my @co_refused = (
	[ q{CREATE FUNCTION pt(point) RETURNS text LANGUAGE sql IMMUTABLE AS 'SELECT 1'; CREATE CAST (point AS text) WITH FUNCTION pt(point)}, 'between types in the catalogs' ],
	[ q{CREATE TYPE rr AS RANGE (SUBTYPE = int4); COMMENT ON CAST (co5.rr AS co5.rr_multirange) IS 'x'}, 'a range\'s own' ],
	[ q{CREATE TYPE "date" AS ENUM ('a'); CREATE CAST (co5."date" AS integer) WITH INOUT}, "say a schema's name" ],
	[ q{CREATE COLLATION c FROM "C"; SET allow_system_table_mods = on; UPDATE pg_collation SET collencoding = CASE WHEN getdatabaseencoding() = 'LATIN1' THEN 6 ELSE 8 END WHERE collname = 'c' AND collnamespace = 'co5'::regnamespace}, 'another encoding' ],
	[ q{CREATE FUNCTION f() RETURNS text LANGUAGE sql BEGIN ATOMIC SELECT 'a'; END}, 'Name the column with AS' ],
	[ q{CREATE FUNCTION f() RETURNS text LANGUAGE sql RETURN (SELECT 'a')}, 'Name the column with AS' ],
	[ q{CREATE COLLATION m FROM "C"; CREATE FUNCTION f() RETURNS text LANGUAGE sql BEGIN ATOMIC SELECT 'a' COLLATE m; END}, 'Name the column with AS' ],
	[ q{CREATE FUNCTION f() RETURNS SETOF text LANGUAGE sql BEGIN ATOMIC SELECT x FROM (SELECT 'a' UNION ALL SELECT 'b') s(x) ; END}, 'Name the column with AS' ],
	[ q{CREATE COLLATION "C" FROM "POSIX"}, 'named as one in the catalogs' ],
	[ q{CREATE DEFAULT CONVERSION c FOR 'LATIN1' TO 'UTF8' FROM iso8859_1_to_utf8}, 'a default conversion' ],
	[ q{CREATE FUNCTION public.co_conv(integer, integer, cstring, internal, integer, boolean) RETURNS integer LANGUAGE c AS '$libdir/utf8_and_iso8859_1', 'iso8859_1_to_utf8'; CREATE CONVERSION c FOR 'LATIN1' TO 'UTF8' FROM public.co_conv}, 'its function is not one in the catalogs' ],
);
for my $case (@co_refused)
{
	my ($sql, $why) = @$case;
	$node->safe_psql('postgres', qq{DROP SCHEMA IF EXISTS co5 CASCADE; CREATE SCHEMA co5; SET search_path = co5; $sql});
	my ($rc8, $out8, $err8) = $node->psql('postgres', q{SELECT test_ws_obj_bytes('co5')});
	isnt($rc8, 0, "refused: $sql");
	like($err8, qr/\Q$why\E/, "for why: $sql");
}

# since a snapshot, one to try again
for my $change (q{COMMENT ON COLLATION co.mine IS 'later'}, q{COMMENT ON CONVERSION co.conv IS 'later'}, q{ALTER CONVERSION co.conv RENAME TO conv2},
	q{COMMENT ON CAST (ca.e AS integer) IS 'later'}, q{DROP CAST (ca.e AS bigint); CREATE CAST (ca.e AS bigint) WITH FUNCTION ca.f(ca.e) AS ASSIGNMENT})
{
	my $where = $change =~ /ca\.e/ ? 'ca' : 'co';
	my $old = $node->background_psql('postgres', on_error_stop => 0);
	$old->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
	$old->query_safe('SELECT count(*) FROM pg_class');
	$node->safe_psql('postgres', $change);
	$old->{stderr} = '';
	$old->query(qq{SELECT test_ws_obj_bytes('$where')});
	like($old->{stderr}, qr/has changed since/, "to try again after: $change");
	$old->quit;
}

# operators, their classes and families
$node->safe_psql('postgres', q{
	CREATE ROLE g_opowner NOLOGIN;
	CREATE SCHEMA op AUTHORIZATION g_opowner;
	CREATE SCHEMA op2 AUTHORIZATION g_opowner;
	SET search_path = op;
	CREATE TYPE m AS ENUM ('a', 'b');
	CREATE FUNCTION mlt(m, m) RETURNS boolean LANGUAGE sql IMMUTABLE AS 'SELECT $1::text < $2::text';
	CREATE FUNCTION mle(m, m) RETURNS boolean LANGUAGE sql IMMUTABLE AS 'SELECT $1::text <= $2::text';
	CREATE FUNCTION meq(m, m) RETURNS boolean LANGUAGE sql IMMUTABLE AS 'SELECT $1::text = $2::text';
	CREATE FUNCTION mne(m, m) RETURNS boolean LANGUAGE sql IMMUTABLE AS 'SELECT $1::text <> $2::text';
	CREATE FUNCTION mge(m, m) RETURNS boolean LANGUAGE sql IMMUTABLE AS 'SELECT $1::text >= $2::text';
	CREATE FUNCTION mgt(m, m) RETURNS boolean LANGUAGE sql IMMUTABLE AS 'SELECT $1::text > $2::text';
	CREATE FUNCTION mcmp(m, m) RETURNS integer LANGUAGE sql IMMUTABLE AS 'SELECT bttextcmp($1::text, $2::text)';
	CREATE FUNCTION mcmp2(m, integer) RETURNS integer LANGUAGE sql IMMUTABLE AS 'SELECT 0';
	CREATE FUNCTION mnot(m) RETURNS boolean LANGUAGE sql IMMUTABLE AS 'SELECT true';
	CREATE OPERATOR <<< (LEFTARG = m, RIGHTARG = m, FUNCTION = mlt, COMMUTATOR = >>>, NEGATOR = >>==, RESTRICT = scalarltsel, JOIN = scalarltjoinsel);
	CREATE OPERATOR <<== (LEFTARG = m, RIGHTARG = m, FUNCTION = mle, COMMUTATOR = >>==, NEGATOR = >>>);
	CREATE OPERATOR === (LEFTARG = m, RIGHTARG = m, FUNCTION = meq, COMMUTATOR = ===, NEGATOR = !==, HASHES, MERGES);
	CREATE OPERATOR !== (LEFTARG = m, RIGHTARG = m, FUNCTION = mne, COMMUTATOR = !==, NEGATOR = ===);
	CREATE OPERATOR >>== (LEFTARG = m, RIGHTARG = m, FUNCTION = mge, COMMUTATOR = <<==, NEGATOR = <<<);
	CREATE OPERATOR >>> (LEFTARG = m, RIGHTARG = m, FUNCTION = mgt, COMMUTATOR = <<<, NEGATOR = <<==);
	CREATE OPERATOR !!! (RIGHTARG = m, FUNCTION = mnot);
	COMMENT ON OPERATOR !!! (NONE, m) IS 'a prefix';
	ALTER OPERATOR !!! (NONE, m) OWNER TO g_opowner;
	CREATE OPERATOR CLASS "Odd Ops" DEFAULT FOR TYPE m USING btree AS
		OPERATOR 1 <<<, OPERATOR 2 <<==, OPERATOR 3 ===, OPERATOR 4 >>==, OPERATOR 5 >>>, FUNCTION 1 mcmp(m, m);
	COMMENT ON OPERATOR CLASS "Odd Ops" USING btree IS 'a class';
	ALTER OPERATOR CLASS "Odd Ops" USING btree OWNER TO g_opowner;
	CREATE OPERATOR FAMILY fam USING btree;
	COMMENT ON OPERATOR FAMILY fam USING btree IS 'a family';
	ALTER OPERATOR FAMILY fam USING btree OWNER TO g_opowner;
	CREATE OPERATOR CLASS in_fam FOR TYPE m USING btree FAMILY fam AS
		OPERATOR 1 <<<, OPERATOR 2 <<==, OPERATOR 3 ===, OPERATOR 4 >>==, OPERATOR 5 >>>, FUNCTION 1 mcmp(m, m);
	ALTER OPERATOR FAMILY fam USING btree ADD FUNCTION 1 (m, integer) mcmp2(m, integer);
	CREATE FUNCTION mhash(m) RETURNS integer LANGUAGE sql IMMUTABLE AS 'SELECT 1';
	CREATE OPERATOR CLASS hash_ops2 FOR TYPE m USING hash AS OPERATOR 1 ===, FUNCTION 1 mhash(m);
	-- required members of a family made only by adding them, tied to the family
	CREATE OPERATOR FAMILY g_fam USING gist;
	CREATE OPERATOR CLASS g_ops FOR TYPE point USING gist FAMILY g_fam AS STORAGE box,
		OPERATOR 1 <<(point, point), OPERATOR 15 <->(point, point) FOR ORDER BY pg_catalog.float_ops,
		FUNCTION 1 gist_point_consistent(internal, point, smallint, oid, internal),
		FUNCTION 2 gist_box_union(internal, internal);
	ALTER OPERATOR FAMILY g_fam USING gist ADD
		FUNCTION 3 (point, point) gist_point_compress(internal), FUNCTION 5 (point, point) gist_box_penalty(internal, internal, internal),
		FUNCTION 6 (point, point) gist_box_picksplit(internal, internal), FUNCTION 7 (point, point) gist_box_same(box, box, internal),
		FUNCTION 8 (point, point) gist_point_distance(internal, point, smallint, oid, internal);
	-- what a family holds before its class is there stays the family's
	CREATE OPERATOR FAMILY early USING btree;
	ALTER OPERATOR FAMILY early USING btree ADD OPERATOR 3 =(integer, integer);
	CREATE OPERATOR CLASS early_ops FOR TYPE integer USING btree FAMILY early AS OPERATOR 1 <(integer, integer), FUNCTION 1 btint4cmp(integer, integer);
	-- a family another sorts by holds nothing of it
	CREATE OPERATOR FAMILY sortf USING btree;
	CREATE OPERATOR FAMILY g2 USING gist;
	ALTER OPERATOR FAMILY g2 USING gist ADD OPERATOR 15 <->(point, point) FOR ORDER BY sortf;
	-- a class with no members of its own, as they went to its family
	CREATE TYPE m2 AS ENUM ('a');
	CREATE OPERATOR CLASS only_fam FOR TYPE m2 USING btree AS FUNCTION 1 enum_cmp(anyenum, anyenum);
	-- a member of a range on the class of its family: the class is before the range, and the member after
	CREATE TYPE rg AS RANGE (SUBTYPE = m, SUBTYPE_OPCLASS = "Odd Ops");
	ALTER OPERATOR FAMILY "Odd Ops" USING btree ADD FUNCTION 4 (rg, rg) btequalimage(oid);
	-- a type a member says only in its row, with a name after what would hold it
	CREATE OPERATOR FAMILY a_fam USING btree;
	CREATE TYPE zz AS ENUM ('a');
	ALTER OPERATOR FAMILY a_fam USING btree ADD FUNCTION 4 (zz, zz) btequalimage(oid);
	CREATE TABLE t (a m);
	CREATE VIEW v AS SELECT a FROM t WHERE a <<< 'b' AND a === 'a';
	CREATE MATERIALIZED VIEW mv AS SELECT a FROM t WITH NO DATA;
	ALTER TABLE t OWNER TO g_opowner;
	ALTER VIEW v OWNER TO g_opowner;
	ALTER MATERIALIZED VIEW mv OWNER TO g_opowner;
	CREATE INDEX mvi ON mv (a in_fam);
});
my $op_held = $node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('op')});
my %op_val = map { my ($k, $v) = split /\t/; $k => pack('H*', $v) } split /\n/, $op_held;
ok($op_val{$_}, "$_ is held") for ('operator/<<<(m, m)', 'operator/!!!(NONE, m)', 'oplink/<<<(m, m)', 'oplink/===(m, m)',
	'opclass/Odd Ops(btree)', 'opfamily/Odd Ops(btree)', 'opclass/in_fam(btree)', 'opfamily/fam(btree)', 'opmembers/fam(btree)');
unlike(join('', values %op_val), qr/\bop\./, 'none names its schema');
like($op_val{'oplink/<<<(m, m)'}, qr/COMMUTATOR = >>>, NEGATOR = >>==/, 'a link says both');
like($op_val{'oplink/<<<(m, m)'}, qr/operator\/>>==\(m, m\).*operator\/>>>\(m, m\)/s, 'and needs them');
like($op_val{'opclass/in_fam(btree)'}, qr/FAMILY fam AS OPERATOR 1 <<<\(m, m\)/, 'a class says its family and items');
like($op_val{'opclass/in_fam(btree)'}, qr/opfamily\/fam\(btree\)/, 'and needs its family');
like($op_val{'opclass/in_fam(btree)'}, qr/operator\/===\(m, m\)/, 'and its operators');
like($op_val{'opmembers/fam(btree)'}, qr/ADD FUNCTION 1 \(m, integer\) mcmp2\(m, integer\)/, 'what a family holds outside a class');
like($op_val{'view/v'}, qr/operator\/<<<\(m, m\)/, 'a view needs its operator');
like($op_val{'index/mvi(mv)'}, qr/opclass\/in_fam\(btree\)/, 'an index its class');

$node->safe_psql('postgres', q{SELECT test_ws_obj_copy('op', 'op2', 'before'); SET ROLE g_opowner; CREATE TABLE op2.t (a op2.m)});
$node->safe_psql('postgres', q{SELECT test_ws_obj_copy('op', 'op2', 'after')});
is($node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('op2')}), $op_held, 'the same bytes restored');
my $op_cat = q{SELECT string_agg(format('%s %s %s %s %s %s', o.oprname, o.oprcanhash, o.oprcanmerge, o.oprrest, c.oprname, x.oprname), ', ' ORDER BY o.oprname)
	FROM pg_operator o LEFT JOIN pg_operator c ON c.oid = o.oprcom LEFT JOIN pg_operator x ON x.oid = o.oprnegate WHERE o.oprnamespace = '%S'::regnamespace};
my $cat_of = sub { my $s = shift; (my $sql = $op_cat) =~ s/%S/$s/; $node->safe_psql('postgres', $sql) };
is($cat_of->('op2'), $cat_of->('op'), 'the catalogs say the same of its operators');
is( $node->safe_psql('postgres', q{SELECT (SELECT count(*) FROM pg_amop a JOIN pg_opfamily f ON f.oid = a.amopfamily WHERE f.opfnamespace = 'op2'::regnamespace) || ' ' ||
		(SELECT count(*) FROM pg_amproc a JOIN pg_opfamily f ON f.oid = a.amprocfamily WHERE f.opfnamespace = 'op2'::regnamespace) || ' ' ||
		(SELECT opcdefault FROM pg_opclass WHERE opcname = 'Odd Ops' AND opcnamespace = 'op2'::regnamespace)}),
	$node->safe_psql('postgres', q{SELECT (SELECT count(*) FROM pg_amop a JOIN pg_opfamily f ON f.oid = a.amopfamily WHERE f.opfnamespace = 'op'::regnamespace) || ' ' ||
		(SELECT count(*) FROM pg_amproc a JOIN pg_opfamily f ON f.oid = a.amprocfamily WHERE f.opfnamespace = 'op'::regnamespace) || ' ' ||
		(SELECT opcdefault FROM pg_opclass WHERE opcname = 'Odd Ops' AND opcnamespace = 'op'::regnamespace)}), 'and of its classes and families');
is($node->safe_psql('postgres', q{SELECT obj_description(oid, 'pg_opfamily') FROM pg_opfamily WHERE opfname = 'fam' AND opfnamespace = 'op2'::regnamespace}), 'a family', 'a family has its comment');
# the same bytes cannot see a member lost on both sides, nor whose it is
my $op_members = sub {
	my $s = shift;
	return $node->safe_psql('postgres', qq{SELECT string_agg(x, ', ' ORDER BY x) FROM (
		SELECT format('op %s %s %s %s', f.opfname, a.amopstrategy, d.refclassid::regclass, d.deptype) x FROM pg_amop a JOIN pg_opfamily f ON f.oid = a.amopfamily
			JOIN pg_depend d ON d.classid = 'pg_amop'::regclass AND d.objid = a.oid AND d.refclassid IN ('pg_opclass'::regclass, 'pg_opfamily'::regclass) WHERE f.opfnamespace = '$s'::regnamespace
		UNION ALL SELECT format('fn %s %s %s %s', f.opfname, a.amprocnum, d.refclassid::regclass, d.deptype) FROM pg_amproc a JOIN pg_opfamily f ON f.oid = a.amprocfamily
			JOIN pg_depend d ON d.classid = 'pg_amproc'::regclass AND d.objid = a.oid AND d.refclassid IN ('pg_opclass'::regclass, 'pg_opfamily'::regclass) WHERE f.opfnamespace = '$s'::regnamespace) y});
};
is($op_members->('op2'), $op_members->('op'), 'every member is tied as it was');
like($op_members->('op'), qr/fn g_fam 5 pg_opfamily i/, 'a required member is on its family');
like($op_members->('op'), qr/op early 3 pg_opfamily a/, 'and what was there before its class');
like($op_val{'opmembers/g_fam(gist)'}, qr/FUNCTION 5 /, 'which is held');
like($op_val{'opmembers/g_fam(gist)'}, qr/FOR ORDER BY float_ops/, 'an ordering operator says its family');
like($op_val{'opclass/g_ops(gist)'}, qr/STORAGE box/, 'a class its storage');
like($op_val{'opclass/g_ops(gist)'}, qr/opmembers\/g_fam\(gist\)/, 'and needs what its family holds outside it');
like($op_val{'opmembers/a_fam(btree)'}, qr/type\/zz/, 'a member needs a type only its row says');
like($op_val{'opmembers/g2(gist)'}, qr/FOR ORDER BY sortf/, 'an ordering operator is its family\'s');
like($op_val{'opmembers/g2(gist)'}, qr/opfamily\/sortf\(btree\)/, 'and needs what it sorts by');
ok(!exists $op_val{'opmembers/sortf(btree)'}, 'which holds nothing of it');
like($op_val{'opclass/only_fam(btree)'}, qr/AS STORAGE m2/, 'a class with no members of its own has a statement to make it');
$node->safe_psql('postgres', q{CREATE TABLE op2.pts (p point); CREATE INDEX pts_p ON op2.pts USING gist (p op2.g_ops)});
pass('an index can be made on what was restored');
is($node->safe_psql('postgres', q{SELECT opfowner::regrole FROM pg_opfamily WHERE opfname = 'fam' AND opfnamespace = 'op2'::regnamespace}), 'g_opowner', 'and its owner');

my @op_refused = (
	[ q{CREATE FUNCTION f(integer, integer) RETURNS boolean LANGUAGE sql AS 'SELECT true'; CREATE OPERATOR < (LEFTARG = integer, RIGHTARG = integer, FUNCTION = f)}, 'as one in the catalogs' ],
	[ q{CREATE OPERATOR CLASS integer_ops FOR TYPE integer USING btree AS OPERATOR 1 <, FUNCTION 1 btint4cmp(integer, integer)}, 'as one in the catalogs' ],
	[ q{CREATE OPERATOR FAMILY integer_ops USING btree}, 'as one in the catalogs' ],
	[ q{CREATE FUNCTION f(integer, integer) RETURNS boolean LANGUAGE sql AS 'SELECT true'; CREATE OPERATOR <<< (LEFTARG = integer, RIGHTARG = integer, FUNCTION = f, COMMUTATOR = >>>)}, 'a shell' ],
	[ q{CREATE FUNCTION f(integer, integer) RETURNS boolean LANGUAGE sql AS 'SELECT true'; CREATE OPERATOR <<< (LEFTARG = integer, RIGHTARG = integer, FUNCTION = f, COMMUTATOR = OPERATOR(public.>>>)); CREATE OPERATOR public.>>> (LEFTARG = integer, RIGHTARG = integer, FUNCTION = op5.f)}, 'in another schema' ],
	[ q{CREATE OPERATOR <<< (LEFTARG = integer, RIGHTARG = integer, FUNCTION = public.op_else)}, 'another schema' ],
	[ q{CREATE OPERATOR FAMILY f USING btree; CREATE OPERATOR CLASS public.oc_else FOR TYPE integer USING btree FAMILY op5.f AS OPERATOR 1 <, FUNCTION 1 btint4cmp(integer, integer)}, 'a class of it is in another schema' ],
	[ q{CREATE OPERATOR CLASS c DEFAULT FOR TYPE lseg USING hash AS FUNCTION 1 hashtext(text)}, 'a default for a type not of this schema' ],
	[ q{CREATE TABLE tq (a int); CREATE OPERATOR CLASS c FOR TYPE tq USING gist AS STORAGE box}, 'row type' ],
	[ q{CREATE TABLE tq (a int); CREATE OPERATOR FAMILY f USING btree; ALTER OPERATOR FAMILY f USING btree ADD FUNCTION 4 (tq, tq) btequalimage(oid)}, 'row type' ],
	[ q{CREATE TABLE tq (a int); CREATE FUNCTION f(tq) RETURNS boolean LANGUAGE sql AS 'SELECT true'; CREATE OPERATOR ### (RIGHTARG = tq, FUNCTION = f)}, 'row type' ],
	[ q{CREATE TYPE "date" AS ENUM ('a'); CREATE OPERATOR CLASS c FOR TYPE op5."date" USING gist AS STORAGE box}, "say a schema's name" ],
	[ q{CREATE TYPE "date" AS ENUM ('a'); CREATE OPERATOR FAMILY f USING btree; ALTER OPERATOR FAMILY f USING btree ADD FUNCTION 4 (op5."date", op5."date") btequalimage(oid)}, "say a schema's name" ],
	[ q{CREATE FUNCTION f(integer, integer) RETURNS boolean LANGUAGE sql AS 'SELECT true'; CREATE OPERATOR <<< (LEFTARG = integer, RIGHTARG = integer, FUNCTION = f, COMMUTATOR = >>>); CREATE OPERATOR >>> (LEFTARG = integer, RIGHTARG = integer, FUNCTION = f)}, 'does not say it back' ],
	[ q{CREATE FUNCTION f(integer, integer) RETURNS boolean LANGUAGE sql AS 'SELECT true'; CREATE OPERATOR <<< (LEFTARG = integer, RIGHTARG = integer, FUNCTION = f, NEGATOR = >>>); CREATE OPERATOR >>> (LEFTARG = integer, RIGHTARG = integer, FUNCTION = f)}, 'does not say it back' ],
	[ q{CREATE OPERATOR CLASS c FOR TYPE integer USING btree FAMILY public.fam_else AS OPERATOR 1 <, FUNCTION 1 btint4cmp(integer, integer)}, 'family is in another schema' ],
	[ q{CREATE FUNCTION f(integer, bigint) RETURNS integer LANGUAGE sql AS 'SELECT 0'; ALTER OPERATOR FAMILY public.fam_else USING btree ADD FUNCTION 1 (integer, bigint) f(integer, bigint)}, 'a family in another schema' ],
	[ q{CREATE OPERATOR FAMILY fam USING btree; ALTER OPERATOR FAMILY fam USING btree ADD FUNCTION 1 (integer, bigint) public.fam_cmp(integer, bigint)}, 'another schema' ],
	[ q{CREATE TABLE q (a int); CREATE VIEW w AS SELECT a FROM q; ALTER VIEW w OWNER TO g_opowner; CREATE FUNCTION f(integer, integer) RETURNS boolean LANGUAGE sql BEGIN ATOMIC SELECT true FROM w; END; CREATE OPERATOR <<< (LEFTARG = integer, RIGHTARG = integer, FUNCTION = f)}, 'which needs a table' ],
	[ q{CREATE TYPE "date" AS ENUM ('a'); CREATE FUNCTION f(op5."date") RETURNS boolean LANGUAGE sql AS 'SELECT true'; CREATE OPERATOR !!! (RIGHTARG = op5."date", FUNCTION = f)}, "say a schema's name" ],
);
$node->safe_psql('postgres', q{CREATE FUNCTION public.op_else(integer, integer) RETURNS boolean LANGUAGE sql AS 'SELECT true'; CREATE OPERATOR FAMILY public.fam_else USING btree;
	CREATE FUNCTION public.fam_cmp(integer, bigint) RETURNS integer LANGUAGE sql AS 'SELECT 0'});
for my $case (@op_refused)
{
	my ($sql, $why) = @$case;
	$node->safe_psql('postgres', qq{DROP OPERATOR CLASS IF EXISTS public.oc_else USING btree; DROP SCHEMA IF EXISTS op5 CASCADE; DROP OPERATOR IF EXISTS public.>>> (integer, integer); CREATE SCHEMA op5; SET search_path = op5; $sql});
	my ($rc9, $out9, $err9) = $node->psql('postgres', q{SELECT test_ws_obj_bytes('op5')});
	isnt($rc9, 0, "refused: $sql");
	like($err9, qr/\Q$why\E/, "for why: $sql");
}
$node->safe_psql('postgres', q{DROP OPERATOR CLASS IF EXISTS public.oc_else USING btree; DROP SCHEMA op5 CASCADE; DROP OPERATOR IF EXISTS public.>>> (integer, integer); DROP OPERATOR FAMILY public.fam_else USING btree; DROP FUNCTION public.op_else(integer, integer); DROP FUNCTION public.fam_cmp(integer, bigint)});

# since a snapshot, one to try again
for my $change (q{COMMENT ON OPERATOR op.!!! (NONE, op.m) IS 'later'}, q{ALTER OPERATOR op.!!! (NONE, op.m) OWNER TO CURRENT_USER},
	q{COMMENT ON OPERATOR CLASS op."Odd Ops" USING btree IS 'later'}, q{COMMENT ON OPERATOR FAMILY op.fam USING btree IS 'later'},
	q{ALTER OPERATOR FAMILY op.fam USING btree DROP FUNCTION 1 (op.m, integer)})
{
	my $old = $node->background_psql('postgres', on_error_stop => 0);
	$old->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
	$old->query_safe('SELECT count(*) FROM pg_class');
	$node->safe_psql('postgres', $change);
	$old->{stderr} = '';
	$old->query(q{SELECT test_ws_obj_bytes('op')});
	like($old->{stderr}, qr/has changed since/, "to try again after: $change");
	$old->quit;
}

# what the catalogs hold is each server's, so what capture found free may not be free
$node->safe_psql('postgres', q{CREATE SCHEMA ff; SELECT test_ws_obj_restore_family('here_only', 'ff')});
is($node->safe_psql('postgres', q{SELECT count(*) FROM pg_opfamily WHERE opfnamespace = 'ff'::regnamespace}), '1', 'a family arrives where its name is free');
$node->safe_psql('postgres', q{DROP OPERATOR FAMILY ff.here_only USING btree; SET allow_system_table_mods = on; CREATE OPERATOR FAMILY pg_catalog.here_only USING btree});
($rc, $out, $err) = $node->psql('postgres', q{SELECT test_ws_obj_restore_family('here_only', 'ff')});
like($err, qr/named as one in this server's catalogs/, 'and is refused where the catalogs have it');
$node->safe_psql('postgres', q{SET allow_system_table_mods = on; DROP OPERATOR FAMILY pg_catalog.here_only USING btree; DROP SCHEMA ff});

$node->safe_psql('postgres', q{CREATE SCHEMA ff; SET allow_system_table_mods = on; CREATE OPERATOR CLASS pg_catalog.here_only FOR TYPE integer USING btree AS STORAGE integer});
($rc, $out, $err) = $node->psql('postgres', q{SELECT test_ws_obj_restore_opclass('here_only', 'ff')});
like($err, qr/operator class "here_only" of schema "ff" is named as one in this server's catalogs/, 'as is a class');
$node->safe_psql('postgres', q{SET allow_system_table_mods = on; DROP OPERATOR FAMILY pg_catalog.here_only USING btree; DROP SCHEMA ff});

# a catalog table's row type is no table of this schema, which is what comes after
$node->safe_psql('postgres', q{CREATE SCHEMA cr; CREATE FUNCTION cr.f(pg_class) RETURNS boolean LANGUAGE sql AS 'SELECT true'; CREATE OPERATOR cr.### (RIGHTARG = pg_class, FUNCTION = cr.f)});
like($node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('cr')}), qr/^operator\/###\(NONE, pg_class\)\t/m, 'an operator on a catalog table\'s row type is held');
$node->safe_psql('postgres', q{DROP SCHEMA cr CASCADE});

# a method's name in another encoding than a record's
$node->safe_psql('postgres', q{CREATE DATABASE oplat TEMPLATE template0 ENCODING 'LATIN1' LOCALE 'C'});
$node->safe_psql('oplat', q{CREATE EXTENSION test_prolly});
$node->safe_psql('oplat', qq{SET client_encoding = 'UTF8'; CREATE ACCESS METHOD "b\xc3\xa9" TYPE INDEX HANDLER bthandler;
	CREATE SCHEMA la; CREATE SCHEMA la2; CREATE OPERATOR FAMILY la.f USING "b\xc3\xa9"; COMMENT ON OPERATOR FAMILY la.f USING "b\xc3\xa9" IS 'x';});
$node->safe_psql('oplat', q{SELECT test_ws_obj_copy('la', 'la2')});
is($node->safe_psql('oplat', q{SELECT obj_description(oid, 'pg_opfamily') FROM pg_opfamily WHERE opfnamespace = 'la2'::regnamespace}), 'x', 'a family of a method not in ASCII has its comment');

# aggregates
$node->safe_psql('postgres', q{
	CREATE ROLE g_agowner NOLOGIN; CREATE ROLE g_agread NOLOGIN;
	CREATE SCHEMA ag AUTHORIZATION g_agowner; CREATE SCHEMA ag2 AUTHORIZATION g_agowner;
	SET search_path = ag;
	CREATE FUNCTION sfn(integer, integer) RETURNS integer LANGUAGE sql IMMUTABLE AS 'SELECT $1 + $2';
	CREATE FUNCTION ffn(integer) RETURNS integer LANGUAGE sql IMMUTABLE AS 'SELECT $1';
	CREATE FUNCTION lt(integer, integer) RETURNS boolean LANGUAGE sql IMMUTABLE AS 'SELECT $1 < $2';
	CREATE OPERATOR <<< (LEFTARG = integer, RIGHTARG = integer, FUNCTION = lt);
	CREATE AGGREGATE full_one(integer) (SFUNC = sfn, STYPE = integer, SSPACE = 16, INITCOND = '0', FINALFUNC = ffn, FINALFUNC_MODIFY = READ_WRITE,
		COMBINEFUNC = sfn, SORTOP = <<<, PARALLEL = SAFE);
	CREATE AGGREGATE moving(integer) (SFUNC = sfn, STYPE = integer, MSFUNC = sfn, MINVFUNC = sfn, MSTYPE = integer, MSSPACE = 8, MFINALFUNC = ffn, MFINALFUNC_MODIFY = SHAREABLE, MINITCOND = '0', PARALLEL = RESTRICTED);
	CREATE AGGREGATE quoted(text) (SFUNC = textcat, STYPE = text, INITCOND = 'it''s');
	CREATE AGGREGATE star(*) (SFUNC = int8inc, STYPE = bigint, INITCOND = '0');
	CREATE AGGREGATE poly(anycompatible) (SFUNC = array_append, STYPE = anycompatiblearray, INITCOND = '{}');
	CREATE AGGREGATE ordered(float8 ORDER BY anyelement) (SFUNC = ordered_set_transition, STYPE = internal, FINALFUNC = percentile_disc_final, FINALFUNC_EXTRA, FINALFUNC_MODIFY = READ_WRITE);
	CREATE AGGREGATE hypo(VARIADIC "any" ORDER BY VARIADIC "any") (SFUNC = ordered_set_transition_multi, STYPE = internal, FINALFUNC = rank_final, FINALFUNC_EXTRA, FINALFUNC_MODIFY = READ_WRITE, HYPOTHETICAL);
	COMMENT ON AGGREGATE full_one(integer) IS 'an aggregate';
	COMMENT ON AGGREGATE ordered(float8 ORDER BY anyelement) IS 'ordered';
	ALTER AGGREGATE full_one(integer) OWNER TO g_agowner;
	REVOKE ALL ON FUNCTION full_one(integer) FROM PUBLIC;
	GRANT EXECUTE ON FUNCTION full_one(integer) TO g_agread;
	CREATE TABLE t (a integer); ALTER TABLE t OWNER TO g_agowner;
	CREATE VIEW v AS SELECT full_one(a) AS x FROM t; ALTER VIEW v OWNER TO g_agowner;
	CREATE FUNCTION uses() RETURNS integer LANGUAGE sql BEGIN ATOMIC SELECT moving(1) AS y; END;
});
my $ag_held = $node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('ag')});
my %ag_val = map { my ($k, $v) = split /\t/; $k => pack('H*', $v) } split /\n/, $ag_held;
ok($ag_val{$_}, "$_ is held") for ('aggregate/full_one(integer)', 'aggregate/moving(integer)', 'aggregate/star()', 'aggregate/poly(anycompatible)',
	'aggregate/ordered(double precision, anyelement)', 'aggregate/hypo("any")');
like($ag_val{'aggregate/star()'}, qr/\(\*\) \(SFUNC = int8inc/, 'an aggregate of no arguments says so');
like($ag_val{'aggregate/ordered(double precision, anyelement)'}, qr/ORDER BY anyelement/, 'and one of order');
like($ag_val{'aggregate/hypo("any")'}, qr/HYPOTHETICAL/, 'and a hypothetical one');
like($ag_val{'aggregate/full_one(integer)'}, qr/operator\/<<<\(integer, integer\)/, 'it needs its operator');
like($ag_val{'aggregate/full_one(integer)'}, qr/function\/sfn\(integer, integer\)/, 'and its function');
like($ag_val{'view/v'}, qr/aggregate\/full_one\(integer\)/, 'a view needs it');
like($ag_val{'function/uses()'}, qr/aggregate\/moving\(integer\)/, 'as does a function');
unlike(join('', values %ag_val), qr/\bag\./, 'none names its schema');
$node->safe_psql('postgres', q{SELECT test_ws_obj_copy('ag', 'ag2', 'before'); SET ROLE g_agowner; CREATE TABLE ag2.t (a integer)});
$node->safe_psql('postgres', q{SELECT test_ws_obj_copy('ag', 'ag2', 'after')});
is($node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('ag2')}), $ag_held, 'aggregates are the same bytes restored');
my $ag_cat = sub {
	my $s = shift;
	return $node->safe_psql('postgres', qq{SELECT string_agg(format('%s %s %s %s %s %s %s %s %s %s %s %s %s %s %s %s', p.proname, a.aggkind, a.aggnumdirectargs, a.aggfinalextra, a.aggmfinalextra, a.aggfinalmodify, a.aggmfinalmodify,
		a.aggtransspace, a.aggmtransspace, a.agginitval, a.aggminitval, p.proparallel, a.aggcombinefn::oid <> 0, a.aggsortop <> 0, a.aggtranstype::regtype::text LIKE '%int%', a.aggminvtransfn::oid <> 0), ', ' ORDER BY p.proname)
		FROM pg_aggregate a JOIN pg_proc p ON p.oid = a.aggfnoid WHERE p.pronamespace = '$s'::regnamespace});
};
is($ag_cat->('ag2'), $ag_cat->('ag'), 'and as they were');
is($node->safe_psql('postgres', q{SELECT has_function_privilege('g_agread', 'ag2.full_one(integer)', 'EXECUTE') AND NOT has_function_privilege('public', 'ag2.full_one(integer)', 'EXECUTE')}), 't', 'with their grants');
is($node->safe_psql('postgres', q{SELECT obj_description('ag2.ordered(float8, anyelement)'::regprocedure, 'pg_proc')}), 'ordered', 'and comments');

# text search
$node->safe_psql('postgres', q{
	CREATE ROLE g_tsowner NOLOGIN;
	CREATE SCHEMA ts AUTHORIZATION g_tsowner; CREATE SCHEMA ts2 AUTHORIZATION g_tsowner;
	SET search_path = ts;
	CREATE TEXT SEARCH PARSER prs (START = prsd_start, GETTOKEN = prsd_nexttoken, END = prsd_end, LEXTYPES = prsd_lextype, HEADLINE = prsd_headline);
	CREATE TEXT SEARCH PARSER "Odd Prs" (START = prsd_start, GETTOKEN = prsd_nexttoken, END = prsd_end, LEXTYPES = prsd_lextype);
	CREATE TEXT SEARCH TEMPLATE tmp (INIT = dsimple_init, LEXIZE = dsimple_lexize);
	CREATE TEXT SEARCH TEMPLATE no_init (LEXIZE = dsimple_lexize);
	CREATE TEXT SEARCH DICTIONARY dict (TEMPLATE = tmp, stopwords = 'english', accept = false);
	CREATE TEXT SEARCH DICTIONARY "Odd Dict" (TEMPLATE = snowball, language = 'english');
	CREATE TEXT SEARCH DICTIONARY bare (TEMPLATE = no_init);
	ALTER TEXT SEARCH DICTIONARY dict OWNER TO g_tsowner;
	CREATE TEXT SEARCH CONFIGURATION cfg (PARSER = prs);
	ALTER TEXT SEARCH CONFIGURATION cfg ADD MAPPING FOR asciiword, word WITH dict, "Odd Dict";
	ALTER TEXT SEARCH CONFIGURATION cfg ADD MAPPING FOR host WITH simple;
	ALTER TEXT SEARCH CONFIGURATION cfg OWNER TO g_tsowner;
	CREATE TEXT SEARCH CONFIGURATION "Odd Cfg" (PARSER = "default");
	COMMENT ON TEXT SEARCH PARSER prs IS 'p'; COMMENT ON TEXT SEARCH TEMPLATE tmp IS 't';
	COMMENT ON TEXT SEARCH DICTIONARY dict IS 'd'; COMMENT ON TEXT SEARCH CONFIGURATION cfg IS 'c';
});
my $ts_held = $node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('ts')});
my %ts_val = map { my ($k, $v) = split /\t/; $k => pack('H*', $v) } split /\n/, $ts_held;
ok($ts_val{$_}, "$_ is held") for ('tsparser/prs', 'tstemplate/tmp', 'tsdict/dict', 'tsconfig/cfg', 'tsmap/cfg(asciiword)', 'tsmap/cfg(host)', 'tsconfig/Odd Cfg');
like($ts_val{'tsdict/dict'}, qr/TEMPLATE = tmp, stopwords = 'english', accept = 'false'/, 'a dictionary says its options');
like($ts_val{'tsdict/dict'}, qr/tstemplate\/tmp/, 'and needs its template');
like($ts_val{'tsconfig/cfg'}, qr/tsparser\/prs/, 'a configuration needs its parser');
like($ts_val{'tsconfig/cfg'}, qr/tsdict\/dict/, 'and its dictionaries');
like($ts_val{'tsmap/cfg(asciiword)'}, qr/ADD MAPPING FOR asciiword WITH dict, "Odd Dict"/, 'a mapping says them in order');
like($ts_val{'tsmap/cfg(host)'}, qr/WITH simple/, 'and those of the catalogs');
unlike(join('', values %ts_val), qr/\bts\./, 'none names its schema');
$node->safe_psql('postgres', q{SELECT test_ws_obj_copy('ts', 'ts2')});
is($node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('ts2')}), $ts_held, 'text search objects are the same bytes restored');
is( $node->safe_psql('postgres', q{SELECT to_tsvector('ts2.cfg', 'The quick host.example.com') = to_tsvector('ts.cfg', 'The quick host.example.com')}), 't', 'and work as they did');
is($node->safe_psql('postgres', q{SELECT cfgowner::regrole FROM pg_ts_config WHERE cfgname = 'cfg' AND cfgnamespace = 'ts2'::regnamespace}), 'g_tsowner', 'with their owners');
is($node->safe_psql('postgres', q{SELECT obj_description(oid, 'pg_ts_parser') FROM pg_ts_parser WHERE prsname = 'prs' AND prsnamespace = 'ts2'::regnamespace}), 'p', 'and comments');

# statistics objects
$node->safe_psql('postgres', q{
	CREATE ROLE g_stowner NOLOGIN;
	CREATE SCHEMA sx AUTHORIZATION g_stowner; CREATE SCHEMA sx2 AUTHORIZATION g_stowner;
	SET ROLE g_stowner; SET search_path = sx;
	CREATE TABLE t (a integer, "Odd B" integer, c text);
	CREATE MATERIALIZED VIEW mv AS SELECT a, c FROM t WITH NO DATA;
	CREATE STATISTICS st (ndistinct) ON a, "Odd B" FROM t;
	CREATE STATISTICS "Odd St" ON a, c FROM t;
	CREATE STATISTICS ex ON (a + 1), ("Odd B" * 2) FROM t;
	CREATE STATISTICS on_mv (dependencies, mcv) ON a, c FROM mv;
	COMMENT ON STATISTICS st IS 'a statistics object';
});
my $st_held = $node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('sx')});
my %st_val = map { my ($k, $v) = split /\t/; $k => pack('H*', $v) } split /\n/, $st_held;
ok($st_val{$_}, "$_ is held") for ('statistics/st', 'statistics/Odd St', 'statistics/ex', 'statistics/on_mv');
like($st_val{'statistics/st'}, qr/\(ndistinct\) ON a, "Odd B" FROM t/, 'it says its table bare');
like($st_val{'statistics/on_mv'}, qr/matview\/mv/, 'and needs a materialized view');
like($st_val{'statistics/st'}, qr/g_stowner/, 'and has its owner');
unlike(join('', values %st_val), qr/\bsx\./, 'none names its schema');
$node->safe_psql('postgres', q{SELECT test_ws_obj_copy('sx', 'sx2', 'before'); SET ROLE g_stowner; CREATE TABLE sx2.t (a integer, "Odd B" integer, c text)});
$node->safe_psql('postgres', q{SELECT test_ws_obj_copy('sx', 'sx2', 'after')});
is($node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('sx2')}), $st_held, 'statistics objects are the same bytes restored');
is( $node->safe_psql('postgres', q{SELECT string_agg(stxname || ' ' || stxkind::text, ', ' ORDER BY stxname) FROM pg_statistic_ext WHERE stxnamespace = 'sx2'::regnamespace}),
	$node->safe_psql('postgres', q{SELECT string_agg(stxname || ' ' || stxkind::text, ', ' ORDER BY stxname) FROM pg_statistic_ext WHERE stxnamespace = 'sx'::regnamespace}), 'and of the same kinds');
is($node->safe_psql('postgres', q{SELECT obj_description(oid, 'pg_statistic_ext') FROM pg_statistic_ext WHERE stxname = 'st' AND stxnamespace = 'sx2'::regnamespace}), 'a statistics object', 'with a comment');

$node->safe_psql('postgres', q{CREATE FUNCTION public.ag_else(integer, integer) RETURNS integer LANGUAGE sql AS 'SELECT 1'; CREATE TABLE public.st_else (a integer, b integer);
	CREATE TEXT SEARCH TEMPLATE public.tmp_else (LEXIZE = dsimple_lexize); CREATE TEXT SEARCH DICTIONARY public.dict_else (TEMPLATE = simple);
	CREATE FOREIGN DATA WRAPPER fdw_st; CREATE SERVER srv_st FOREIGN DATA WRAPPER fdw_st});
for my $case (
	[ q{CREATE AGGREGATE a(integer) (SFUNC = public.ag_else, STYPE = integer)}, 'another schema' ],
	[ q{CREATE TYPE "date" AS ENUM ('a'); CREATE AGGREGATE a(ka5."date") (SFUNC = enum_larger, STYPE = ka5."date")}, "say a schema's name" ],
	[ q{CREATE TEXT SEARCH DICTIONARY simple (TEMPLATE = simple)}, 'as one in the catalogs' ],
	[ q{CREATE TEXT SEARCH CONFIGURATION english (PARSER = "default")}, 'as one in the catalogs' ],
	[ q{CREATE TEXT SEARCH PARSER "default" (START = prsd_start, GETTOKEN = prsd_nexttoken, END = prsd_end, LEXTYPES = prsd_lextype)}, 'as one in the catalogs' ],
	[ q{CREATE TEXT SEARCH TEMPLATE snowball (LEXIZE = dsimple_lexize)}, 'as one in the catalogs' ],
	[ q{CREATE TEXT SEARCH DICTIONARY d (TEMPLATE = public.tmp_else)}, 'another schema' ],
	[ q{CREATE TEXT SEARCH CONFIGURATION c (PARSER = "default"); ALTER TEXT SEARCH CONFIGURATION c ADD MAPPING FOR word WITH public.dict_else}, 'another schema' ],
	[ q{CREATE TABLE t (a integer, b integer); CREATE STATISTICS public.st_here ON a, b FROM ka5.t}, 'another schema than its table' ],
	[ q{CREATE STATISTICS s ON a, b FROM public.st_else}, 'another schema than its table' ],
	[ q{CREATE TABLE t (a integer, b integer); CREATE STATISTICS s ON a, b FROM t; ALTER STATISTICS s SET STATISTICS 10}, 'a statistics target' ],
	[ q{CREATE TABLE pg_class (a integer, b integer); CREATE STATISTICS s ON a, b FROM ka5.pg_class}, "say a schema's name" ],
	[ q{CREATE FOREIGN TABLE ft (a integer, b integer) SERVER srv_st; CREATE STATISTICS s ON a, b FROM ft}, 'what no commit holds' ],
	[ q{CREATE TYPE "date" AS ENUM ('a'); CREATE TABLE t (a integer, b integer); CREATE STATISTICS s ON (('a'::ka5."date")::text || a), b FROM t}, "say a schema's name" ])
{
	my ($sql, $why) = @$case;
	$node->safe_psql('postgres', qq{DROP STATISTICS IF EXISTS public.st_here; DROP SCHEMA IF EXISTS ka5 CASCADE; DROP STATISTICS IF EXISTS public.s; CREATE SCHEMA ka5; SET search_path = ka5; $sql});
	my ($rck, $outk, $errk) = $node->psql('postgres', q{SELECT test_ws_obj_bytes('ka5')});
	isnt($rck, 0, "refused: $sql");
	like($errk, qr/\Q$why\E/, "for why: $sql");
}
$node->safe_psql('postgres', q{DROP STATISTICS IF EXISTS public.st_here; DROP SCHEMA ka5 CASCADE; DROP STATISTICS IF EXISTS public.s; DROP FUNCTION public.ag_else(integer, integer); DROP TABLE public.st_else;
	DROP TEXT SEARCH DICTIONARY public.dict_else; DROP TEXT SEARCH TEMPLATE public.tmp_else; DROP SERVER srv_st; DROP FOREIGN DATA WRAPPER fdw_st});

for my $x (
	[ q{CREATE TEXT SEARCH TEMPLATE cx.c (LEXIZE = dsimple_lexize)}, 'TEXT SEARCH TEMPLATE cx.c' ],
	[ q{CREATE TEXT SEARCH DICTIONARY cx.c (TEMPLATE = simple)}, 'TEXT SEARCH DICTIONARY cx.c' ],
	[ q{CREATE TEXT SEARCH PARSER cx.c (START = prsd_start, GETTOKEN = prsd_nexttoken, END = prsd_end, LEXTYPES = prsd_lextype)}, 'TEXT SEARCH PARSER cx.c' ],
	[ q{CREATE TEXT SEARCH CONFIGURATION cx.c (PARSER = "default")}, 'TEXT SEARCH CONFIGURATION cx.c' ],
	[ q{CREATE AGGREGATE cx.c(integer) (SFUNC = int4pl, STYPE = integer)}, 'AGGREGATE cx.c(integer)' ])
{
	my ($make, $member) = @$x;
	$node->safe_psql('postgres', qq{CREATE SCHEMA cx; $make; ALTER EXTENSION test_prolly ADD $member});
	my ($rcx, $outx, $errx) = $node->psql('postgres', q{SELECT test_ws_obj_bytes('cx')});
	like($errx, qr/belongs to an extension/, "refused: $member");
	$node->safe_psql('postgres', qq{ALTER EXTENSION test_prolly DROP $member; DROP SCHEMA cx CASCADE});
}

# since a snapshot, one to try again
for my $change ([ 'ts', q{COMMENT ON TEXT SEARCH PARSER ts.prs IS 'later'} ], [ 'ts', q{COMMENT ON TEXT SEARCH TEMPLATE ts.tmp IS 'later'} ],
	[ 'ts', q{ALTER TEXT SEARCH DICTIONARY ts.dict (accept = true)} ], [ 'ts', q{ALTER TEXT SEARCH CONFIGURATION ts.cfg ALTER MAPPING FOR host WITH ts.dict} ],
	[ 'ts', q{COMMENT ON TEXT SEARCH CONFIGURATION ts.cfg IS 'later'} ], [ 'sx', q{ALTER STATISTICS sx.ex RENAME TO ex2} ], [ 'sx', q{COMMENT ON STATISTICS sx.st IS 'later'} ],
	[ 'ag', q{COMMENT ON AGGREGATE ag.star(*) IS 'later'} ])
{
	my ($where, $sql) = @$change;
	my $old = $node->background_psql('postgres', on_error_stop => 0);
	$old->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
	$old->query_safe('SELECT count(*) FROM pg_class');
	$node->safe_psql('postgres', $sql);
	$old->{stderr} = '';
	$old->query(qq{SELECT test_ws_obj_bytes('$where')});
	like($old->{stderr}, qr/has changed since/, "to try again after: $sql");
	$old->quit;
}

$node->stop;
done_testing();
