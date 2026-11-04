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
	[ q{CREATE FUNCTION f() RETURNS int LANGUAGE sql AS 'SELECT 1'; CREATE VIEW r_func AS SELECT f()}, 'no commit holds yet', undef, 'DROP FUNCTION "S rc".f() CASCADE' ],
	[ q{CREATE SEQUENCE q; CREATE VIEW r_seq AS SELECT nextval('q'::regclass)}, 'no commit holds yet', undef, 'DROP SEQUENCE "S rc".q CASCADE' ],
	[ q{CREATE MATERIALIZED VIEW r_idx AS SELECT a FROM t; CREATE INDEX ON r_idx (a)}, 'an index', undef, 'DROP MATERIALIZED VIEW "S rc".r_idx' ],
	[ q{CREATE VIEW r_def AS SELECT a FROM t; ALTER VIEW r_def ALTER COLUMN a SET DEFAULT 1}, 'a default', undef, 'DROP VIEW "S rc".r_def' ],
	[ q{CREATE VIEW r_rule AS SELECT a FROM t; CREATE RULE r AS ON DELETE TO r_rule DO INSTEAD NOTHING}, 'a rule', undef, 'DROP VIEW "S rc".r_rule' ],
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
my @ty_refused = (
	[ q{CREATE TYPE r_shell}, 'no definition yet' ],
	[ q{CREATE FUNCTION rf(int) RETURNS boolean LANGUAGE sql AS 'SELECT true'; CREATE DOMAIN r_fn AS int CHECK (rf(VALUE))}, 'no commit holds yet' ],
	[ q{CREATE TABLE rt (a int); CREATE TYPE r_rowtype AS (r rt)}, 'a table' ],
	[ q{CREATE TABLE rt2 (a int); CREATE TYPE r_rowtype2 AS (r rt2[])}, 'a table' ],
	[ q{CREATE VIEW vv AS SELECT 1 AS a; CREATE DOMAIN r_viewdom AS vv[]}, 'a view' ],
	[ q{CREATE DOMAIN r_self AS int; ALTER DOMAIN r_self SET DEFAULT (5::r_self)}, 'needs itself' ],
	[ q{CREATE TYPE r_cast AS ENUM ('a'); CREATE CAST (r_cast AS integer) WITH INOUT}, 'a cast' ],
	[ q{CREATE TYPE r_cast2 AS ENUM ('a'); CREATE CAST (r_cast2[] AS text) WITH INOUT}, 'a cast' ],
	[ q{CREATE TYPE r_cast3 AS RANGE (SUBTYPE = int4); CREATE CAST (r_cast3_multirange AS integer) WITH INOUT}, 'a cast' ],
	[ q{CREATE TABLE r_cast4 (a int); CREATE CAST (r_cast4 AS integer) WITH INOUT}, 'a cast' ],
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
	[ q{CREATE COLLATION r_c FROM "C"; CREATE DOMAIN r_coll AS text COLLATE r_c}, 'no commit holds yet' ],
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

$node->stop;
done_testing();
