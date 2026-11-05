# Copyright (c) 2026, PostgreSQL Global Development Group
#
# What a commit holds must read the same once it has been restored, with nothing changed.
# Postgres's printing does not promise that of itself, so a restore remembers what it
# printed.  This holds a broad set of expressions in each place one can be, compares
# every object's bytes and what it returns, and then what a real change does.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Digest::SHA qw(sha1_hex);

my $node = PostgreSQL::Test::Cluster->new('stable');
$node->init;
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_prolly');

my @exprs = split /\n/, <<'EXPRS';
v IN ('a','b')
v NOT IN ('a','b')
v = ANY (ARRAY['a','b'])
v = ANY (ARRAY['a','b']::varchar[])
v::text = ANY (ARRAY['a','b']::text[])
i IN (1,2)
i NOT IN (1,2)
i = ANY (ARRAY[1,2])
b IN (1,2)
i::bigint IN (1,2)
n IN (1.5,2)
t IN ('a','b')
t IN ('a', v)
c IN ('a','b')
v LIKE 'a%'
v ILIKE 'a%'
v NOT LIKE 'a%'
t ~ 'a'
v SIMILAR TO 'a'
'a'
'a'::text
'a'::varchar
'a'::varchar(3)
'a'::char(3)
1
1::bigint
1::smallint
1.5
1.5::numeric(4,2)
1e10
true
NULL
NULL::int
NULL::text
'2020-01-01'::date
'2020-01-01'::timestamp
'1 day'::interval
interval '1 day'
date '2020-01-01'
timestamp '2020-01-01'
time '12:00'
'{1,2}'::int[]
ARRAY[1,2]
ARRAY['a','b']
ARRAY['a','b']::varchar[]
ARRAY[v,'b']
ARRAY[]::int[]
ARRAY[[1,2],[3,4]]
i + 1
i + b
i * 2 + 1
(i + 1) * 2
-i
- 1
i::text
v::text
t::varchar
t::varchar(3)
i::numeric(10,2)
n::int
CAST(i AS text)
i::text::int
v || 'x'
t || 'x'
v || t
t || i
CASE WHEN i > 0 THEN 'a' ELSE 'b' END
CASE i WHEN 1 THEN 'a' ELSE 'b' END
CASE WHEN i > 0 THEN 1 END
CASE v WHEN 'a' THEN 1 END
COALESCE(v,'x')
COALESCE(i,0)
NULLIF(i,0)
NULLIF(v,'x')
GREATEST(i,1)
LEAST(i,b)
i IS NULL
i IS NOT NULL
b2 IS TRUE
b2 IS NOT FALSE
b2 IS UNKNOWN
i IS DISTINCT FROM 1
i IS NOT DISTINCT FROM 1
ROW(i,b) IS NULL
ROW(i,b) = ROW(1,2)
ROW(i,b) < ROW(1,2)
ROW(i,b) IS DISTINCT FROM ROW(1,2)
ROW(v,t) = ROW('a','b')
i BETWEEN 1 AND 2
i NOT BETWEEN 1 AND 2
i BETWEEN SYMMETRIC 1 AND 2
v BETWEEN 'a' AND 'b'
b2 AND i > 0
b2 OR i > 0
NOT b2
upper(v)
upper(t)
length(v)
substring(v from 1 for 2)
substring(t, 1, 2)
position('a' in v)
overlay(t placing 'a' from 1)
trim(v)
trim(both 'x' from v)
trim(leading from t)
extract(year from d)
date_part('year', d)
date_trunc('day', ts)
ts AT TIME ZONE 'UTC'
tz AT LOCAL
current_date
current_time
current_timestamp
localtimestamp
localtime
current_timestamp(2)
current_user
session_user
current_role
current_schema
current_catalog
now()
t COLLATE "C"
v COLLATE "C"
'a' COLLATE "C"
(t COLLATE "C") < 'a'
t < 'a' COLLATE "C"
t IN ('a' COLLATE "C",'b')
j -> 'a'
j ->> 'a'
j #> '{a}'
jb @> '{}'
jb ? 'a'
jb -> 0
jb['a']
a[1]
a[1:2]
a[:2]
a[1:]
(p).x
(ROW(1,2)::pt).x
to_tsvector('english', t)
'a b'::tsvector
i::oid
now()::date
format('%s', i)
format('%s %s', i, v)
concat(i, v)
concat_ws(',', i, v)
num_nulls(VARIADIC ARRAY[1,2])
(SELECT 1)
(SELECT i)
(SELECT 'a')
(SELECT 'a' AS x)
(SELECT v)
(SELECT max(i) FROM s)
EXISTS (SELECT 1 FROM s)
i IN (SELECT i FROM s)
v IN (SELECT v FROM s)
v IN (SELECT t FROM s)
i = ANY (SELECT i FROM s)
i > ALL (SELECT i FROM s)
ARRAY(SELECT i FROM s)
ROW(i,b) IN (SELECT i,i FROM s)
json_object('a': 1)
json_array(1,2)
JSON_VALUE(jb, '$.a')
JSON_QUERY(jb, '$.a')
JSON_EXISTS(jb, '$.a')
jb IS JSON
t IS JSON OBJECT
JSON_OBJECT('a': v RETURNING text)
int4range(1,2)
'[1,2)'::int4range
int4range(1,2) @> i
pos(t)
t NOTNULL
i ISNULL
t IS NORMALIZED
normalize(t)
t IS NFC NORMALIZED
collation for (t)
'a' = 'a'
1 = 1
'a'::text = v
v = 'a'
v = 'a'::text
v = 'a'::varchar
c = 'a'
c = v
c = t
c::text = t
b = i
i = b
n = i
n = 1
f = 1
f = 1.5
f = i
d = '2020-01-01'
d = ts
d = tz
ts = tz
ts > now()
d > now()
d + 1
d + i
d - 1
d - d
ts + '1 day'
ts - interval '1 day'
tz + '1 day'::interval
i::bool
b2::int
i::bigint::int
v::varchar(3)
v::char(3)
c::varchar
c::text
c::char(2)
t::char
t::name
n2::text
n2 = 'a'
n2 = t
v = n2
n2 IN ('a','b')
c IN ('a','b')
c NOT IN ('a','b')
d IN ('2020-01-01','2020-01-02')
ts IN ('2020-01-01','2020-01-02')
n IN (1,2)
f IN (1,2)
b IN (1,i)
m IN ('a','b')
m = 'a'
m::text = 'a'
m::text IN ('a','b')
v::m
'a'::m
dm = 1
dm IN (1,2)
dm::int
1::dm
dv = 'a'
dv IN ('a','b')
dv::text
'a'::dv
dv || 'x'
ad[1]
'{1}'::ad
ARRAY[['a','b'],['c','d']]::varchar[]
ARRAY[ARRAY['a'::varchar]]::text[]
ARRAY['a','b']::varchar(3)[]
ARRAY['a'::varchar(5)]::varchar(3)[]
ARRAY[v,'b']::text[]
ARRAY[i,1]::text[]
ARRAY[i]::bigint[]
ARRAY[1,2]::bigint[]
ARRAY[1.5]::int[]
ARRAY[]::varchar[]::text[]
a::text[]
a::bigint[]
'{a}'::varchar[]::text[]
(ARRAY['a']::varchar[])[1]
v::text = ANY (ARRAY[v,t]::text[])
i::bigint = ANY (ARRAY[1,2]::bigint[])
m::text IN ('a',v)
ARRAY['a','b']::m[]
ARRAY[1]::ad
ARRAY['a']::dv[]
ARRAY[ROW(1,2)]::pt[]
i BETWEEN 1 AND 2 AND v = 'a'
i BETWEEN 1 AND 2 OR b2
i NOT BETWEEN 1 AND 2 OR b2
i NOT BETWEEN 1 AND 2 AND b2
ROW(i,b) = ROW(1,2) AND b2
ROW(i,b) <> ROW(1,2) OR b2
v IN ('a', t) AND b2
v NOT IN ('a', t) AND b2
v IN ('a', t) OR b2
(b2 AND i > 0) AND i < 5
b2 AND (i > 0 AND i < 5)
(b2 OR i > 0) OR i < 5
b2 OR (i > 0 OR i < 5)
(b2 OR i > 0) AND i < 5
(i BETWEEN 1 AND 2 AND i BETWEEN 0 AND 3) AND b2
NOT (i BETWEEN 1 AND 2 AND b2)
-1::int < i
-1::int
-1.5::numeric
-1.5::numeric < n
-(1::int)
-(1.5::numeric)
- 1.5
- 1.5 < n
+1
+1::int
- i
-1::bigint
-1::smallint
-1.5::float8
- 1e10
- - 1
-(-1)
(-1)::int
i - 1
i - -1
i - (-1::int)
n - -1.5
ARRAY['a']::varchar[]::text[]::varchar[]
ARRAY[1]::bigint[]::ad
ARRAY[1]::bigint[]::integer[]::bigint[]::integer[]::text[]
v IN ('a' COLLATE "C",'b')
v NOT IN ('a' COLLATE "C",'b')
ARRAY[a,a2]::int[]::text[]
ARRAY[a,a2]::text[]
ARRAY[a,a2]::int[]::bigint[]
ARRAY[a,a2]::bigint[]
t = ANY (ARRAY[va,vb])
t = ANY (ARRAY[ARRAY[v],vb])
ARRAY[ARRAY[]::int[]]::int[]::text[]
ARRAY[va,vb]::text[]
ARRAY[va]::text[]
ARRAY[[1,2],[3,4]]::text[]
ARRAY[[1,2],[3,4]]::bigint[]
ARRAY[ARRAY[1,2],a2]::text[]
ARRAY[]::int[]::text[]
ARRAY[]::varchar[]::dv[]
EXPRS

my $types = q{
	CREATE TYPE m AS ENUM ('a', 'b');
	CREATE TYPE pt AS (x integer, y integer);
	CREATE DOMAIN dm AS integer;
	CREATE DOMAIN dv AS varchar(10);
	CREATE DOMAIN ad AS integer[];
	CREATE FUNCTION pos(text) RETURNS integer LANGUAGE sql IMMUTABLE AS 'SELECT 1';
};
my $tables = q{
	CREATE TABLE s (i integer, v varchar, t text);
	CREATE TABLE t0 (i integer, b bigint, n numeric, f float8, v varchar(10), t text, c char(2), n2 name, b2 boolean,
		d date, ts timestamp, tz timestamptz, j json, jb jsonb, a integer[], p pt, u uuid, m m, dm dm, dv dv, ad ad,
		a2 integer[], va varchar[], vb varchar[]);
};

my $make = "CREATE ROLE g_stable NOLOGIN; CREATE SCHEMA sp AUTHORIZATION g_stable; CREATE SCHEMA sp2 AUTHORIZATION g_stable; SET ROLE g_stable; SET search_path = sp;\n"
  . $types . $tables;
my $k = 0;
for my $e (@exprs)
{
	$k++;
	$make .= "CREATE VIEW v$k AS SELECT $e AS col FROM t0;\n";
	$make .= "CREATE VIEW w$k AS SELECT 1 AS col FROM t0 WHERE ($e) IS NOT NULL;\n";
	$make .= "CREATE FUNCTION f$k() RETURNS void LANGUAGE sql BEGIN ATOMIC SELECT $e FROM t0; END;\n";
}
$node->safe_psql('postgres', $make);

# the other places an expression can be, and what has no column of a table to read
$node->safe_psql('postgres', q{SET ROLE g_stable; SET search_path = sp;
	CREATE DOMAIN dc AS varchar(10) CHECK (VALUE IN ('a', 'b'));
	CREATE DOMAIN dd AS varchar(10) DEFAULT 'a' CHECK (VALUE NOT IN ('c'));
	CREATE FUNCTION fd(x boolean DEFAULT ('a'::varchar IN ('a', 'b'))) RETURNS boolean LANGUAGE sql RETURN x;
	CREATE FUNCTION fp(p integer, q varchar) RETURNS void LANGUAGE sql BEGIN ATOMIC SELECT p; SELECT p FROM t0; SELECT q IN ('a', 'b'); SELECT 1 WHERE p IN (SELECT p); INSERT INTO s(i) SELECT p RETURNING p; END;
	CREATE FUNCTION fu() RETURNS void LANGUAGE sql BEGIN ATOMIC SELECT 'a' UNION ALL SELECT 'b'; SELECT x FROM (SELECT 'a' UNION ALL SELECT 'b') y(x); WITH z AS (SELECT 'a') SELECT * FROM z; SELECT * FROM (SELECT 'a') z; SELECT 'a', 'b', 1; SELECT ROW(1, 'a'); SELECT ROW(i, v) FROM t0; SELECT array_agg(i) OVER () FROM t0; SELECT sum(i) OVER (PARTITION BY v) FROM t0; SELECT rank() OVER (ORDER BY i) FROM t0; SELECT sum(i) OVER (ORDER BY i ROWS BETWEEN 1 PRECEDING AND CURRENT ROW) FROM t0; UPDATE s SET v = 'a' RETURNING 'a'; DELETE FROM s RETURNING 'a'; INSERT INTO s(v) VALUES ('a') RETURNING 'a'; END;
	CREATE FUNCTION fr() RETURNS text LANGUAGE sql RETURN (SELECT 'a');
	CREATE MATERIALIZED VIEW mv AS SELECT v FROM t0 WITH NO DATA;
	CREATE INDEX mvi ON mv (v) WHERE v IN ('a', 'b');
	CREATE INDEX mve ON mv ((v IN ('a', 'b')));
	CREATE VIEW dfl AS SELECT v FROM t0;
	ALTER VIEW dfl ALTER COLUMN v SET DEFAULT ('a'::varchar IN ('a'))::text;
	CREATE FUNCTION trf() RETURNS trigger LANGUAGE plpgsql AS 'BEGIN RETURN NEW; END';
	CREATE TRIGGER tg BEFORE UPDATE ON s FOR EACH ROW WHEN (NEW.v IN ('a', 'b')) EXECUTE FUNCTION trf();
	CREATE POLICY po ON s USING (v IN ('a', 'b')) WITH CHECK (v NOT IN ('c'));
	CREATE RULE ru AS ON DELETE TO s WHERE OLD.v IN ('a', 'b') DO ALSO DELETE FROM s WHERE v IN ('c', 'd');
});

my $held = $node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('sp')});
my %was = map { my ($key, $v) = split /\t/; $key => $v } split /\n/, $held;
cmp_ok(scalar(keys %was), '>', 3 * scalar(@exprs), 'every expression is held in each place');

$node->safe_psql('postgres', q{SELECT test_ws_obj_copy('sp', 'sp2', 'before')});
$node->safe_psql('postgres', "SET ROLE g_stable; SET search_path = sp2;\n" . $tables);
$node->safe_psql('postgres', q{SELECT test_ws_obj_copy('sp', 'sp2', 'after', true)});

# (all at once, so this says which, not only that one did)
my %is = map { my ($key, $v) = split /\t/; $key => $v } split /\n/, $node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('sp2')});
my @differ = grep { ($is{$_} // '') ne $was{$_} } sort keys %was;
for my $key (@differ[0 .. ($#differ < 9 ? $#differ : 9)])
{
	diag("$key:\n  was " . pack('H*', $was{$key}) . "\n  is  " . pack('H*', $is{$key} // ''));
}
is(scalar(@differ), 0, 'every object is the same bytes restored');
is(scalar(keys %is), scalar(keys %was), 'and there are no others');

# the same bytes cannot see a value that has changed, if it prints the same both times
my $row = q{INSERT INTO %s.t0 (i, b, n, f, v, t, c, n2, b2, d, ts, tz, j, jb, a, p, u, m, dm, dv, ad, a2, va, vb) VALUES
	(1, 2, 1.5, 2.5, 'a', '{a,b}', 'a', 'a', true, '2020-01-01', '2020-01-01', '2020-01-01 00:00+00', '{"a":1}', '{"a":1}', '{1,2}', ROW(1,2), NULL, 'a', 1, 'a', '{1}', '{3,4}', '{a,b}', '{c,d}'),
	(-1, NULL, NULL, NULL, 'z', 'c', NULL, NULL, false, NULL, NULL, NULL, NULL, NULL, '{1,2}', NULL, NULL, 'b', NULL, 'z', NULL, '{5,6}', '{x}', '{y}')};
$node->safe_psql('postgres', sprintf($row, $_)) for ('sp', 'sp2');
$node->safe_psql('postgres', q{CREATE PROCEDURE public.values_of(s text) LANGUAGE plpgsql AS $$
	DECLARE r record; got text;
	BEGIN
		CREATE TEMP TABLE IF NOT EXISTS seen (schema_name text, view_name text, got text);
		FOR r IN SELECT c.relname FROM pg_class c WHERE c.relnamespace = s::regnamespace AND c.relkind = 'v' AND c.relname ~ '^[vw][0-9]' LOOP
			BEGIN
				EXECUTE format('SELECT string_agg(col::text, %L) FROM %I.%I', '|', s, r.relname) INTO got;
			EXCEPTION WHEN OTHERS THEN
				got := 'ERROR ' || SQLSTATE;
			END;
			INSERT INTO seen VALUES (s, r.relname, got);
		END LOOP;
	END $$});
# (what depends on the moment is no matter of printing)
my @moving = map { $_ + 1 } grep { $exprs[$_] =~ /now\(\)|current_|localtime|session_user/ } 0 .. $#exprs;
my $moving = join(',', map { "'v$_','w$_'" } @moving);
my $differ = $node->safe_psql('postgres', qq{CALL public.values_of('sp'); CALL public.values_of('sp2');
	SELECT coalesce(string_agg(a.view_name || ' was ' || coalesce(a.got, 'NULL') || ' is ' || coalesce(b.got, 'NULL'), E'\\n'), '')
	FROM seen a JOIN seen b ON b.view_name = a.view_name AND b.schema_name = 'sp2' WHERE a.schema_name = 'sp' AND a.view_name NOT IN ($moving)
	AND a.got IS DISTINCT FROM b.got});
is($differ, '', 'every view has the same values restored');
my $looked = $node->safe_psql('postgres', qq{CALL public.values_of('sp'); SELECT count(*) > 0 AND count(*) FILTER (WHERE got NOT LIKE 'ERROR%') * 2 > count(*) FROM seen});
is($looked, 't', 'most of which could be read');

# a second round, as a form that only settled later would pass the first by chance
$node->safe_psql('postgres', q{CREATE SCHEMA sp3 AUTHORIZATION g_stable; SELECT test_ws_obj_copy('sp2', 'sp3', 'before')});
$node->safe_psql('postgres', "SET ROLE g_stable; SET search_path = sp3;\n" . $tables);
$node->safe_psql('postgres', q{SELECT test_ws_obj_copy('sp2', 'sp3', 'after', true)});
is($node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('sp3')}), $held, 'and again');

# only what printed otherwise is remembered
my $remembered = $node->safe_psql('postgres', q{SELECT count(*) FROM pg_ws_print WHERE prtnamespace = 'sp2'::regnamespace});
cmp_ok($remembered, '>', 0, 'what printed otherwise is remembered');
cmp_ok($remembered, '<', scalar(keys %was) / 2, 'and not most of it');
is($node->safe_psql('postgres', q{SELECT count(*) FROM pg_ws_print WHERE prtnamespace = 'sp'::regnamespace}), '0', 'and nothing where nothing was restored');
is($node->safe_psql('postgres', q{SELECT count(*) FROM pg_ws_print WHERE prtnamespace = 'sp2'::regnamespace AND prtprinted = prtheld}), '0', 'and no entry says nothing');

# a real change reads as one, on what was remembered as on what was not, and undone is as it was
$node->safe_psql('postgres', q{ALTER TABLE sp2.t0 RENAME COLUMN v TO v_renamed});
%is = map { my ($key, $v) = split /\t/; $key => $v } split /\n/, $node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('sp2')});
my @uses = grep { $_ =~ /^view\/[vw]/ && pack('H*', $was{$_}) =~ /\bv\b/ } sort keys %was;
cmp_ok(scalar(@uses), '>', 20, 'enough use the column');
my @missed = grep { $is{$_} eq $was{$_} } @uses;
is(scalar(@missed), 0, 'a rename reads as a change on all that use it') or diag(join(', ', @missed[0 .. ($#missed < 5 ? $#missed : 5)]));
$node->safe_psql('postgres', q{ALTER TABLE sp2.t0 RENAME COLUMN v_renamed TO v});
is($node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('sp2')}), $held, 'and undone reads as the commit again');

# made anew as it was first made, or as what the restore printed, it is as the commit
my ($stale) = grep { pack('H*', $was{$_}) =~ /ANY \(\(ARRAY/ } grep { /^view\/w/ } sort keys %was;
ok($stale, 'there is a view that printed otherwise');
is($node->safe_psql('postgres', qq{SELECT count(*) FROM pg_ws_print WHERE prtnamespace = 'sp2'::regnamespace AND prtkey = '$stale'}), '1', 'and is remembered');
(my $stale_name = $stale) =~ s/^view\///;
my $restored_def = $node->safe_psql('postgres', qq{SET search_path = pg_catalog, sp2; SELECT pg_get_viewdef('sp2.$stale_name'::regclass)});
$node->safe_psql('postgres', qq{SET ROLE g_stable; SET search_path = pg_catalog, sp2, public; DROP VIEW sp2.$stale_name; CREATE VIEW sp2.$stale_name AS $restored_def});
is($node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('sp2')}), $held, 'one made anew from what the restore printed reads as the commit');

# other real changes read as changes, and each is the fresh print, on what was remembered
for my $change (
	[ qq{CREATE OR REPLACE VIEW sp2.$stale_name AS SELECT 2 AS col FROM t0 WHERE v IN ('a', 'b')}, $stale ],
	[ q{CREATE DOMAIN sp2.dc2 AS varchar(10); ALTER DOMAIN sp2.dc DROP CONSTRAINT dc_check; ALTER DOMAIN sp2.dc ADD CONSTRAINT dc_check CHECK (VALUE IN ('a', 'c')); DROP DOMAIN sp2.dc2}, 'domain/dc' ],
	[ q{CREATE OR REPLACE FUNCTION sp2.fr() RETURNS text LANGUAGE sql RETURN (SELECT 'b')}, 'function/fr()' ],
	[ q{DROP RULE ru ON sp2.s; CREATE RULE ru AS ON DELETE TO sp2.s WHERE OLD.v IN ('a', 'c') DO ALSO DELETE FROM sp2.s WHERE v IN ('c', 'd')}, 'rule/ru(s)' ],
	[ q{DROP POLICY po ON sp2.s; CREATE POLICY po ON sp2.s USING (v IN ('a', 'c')) WITH CHECK (v NOT IN ('c'))}, 'policy/po(s)' ])
{
	my ($sql, $key) = @$change;
	$node->safe_psql('postgres', "SET search_path = pg_catalog, sp2, public; $sql");
	my %after = map { my ($k, $v) = split /\t/; $k => $v } split /\n/, $node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('sp2')});
	isnt($after{$key}, $was{$key}, "a change reads as one: $key");
}

# a restore over what was remembered of another commit replaces it
$node->safe_psql('postgres', q{CREATE SCHEMA twice AUTHORIZATION g_stable; CREATE SCHEMA twice2 AUTHORIZATION g_stable; SET ROLE g_stable; CREATE TABLE twice.t (v varchar(10));
	CREATE VIEW twice.w AS SELECT v FROM twice.t WHERE v IN ('a', 'b');
	CREATE TABLE twice2.t (v varchar(10))});
my $twice_first = $node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('twice')});
$node->safe_psql('postgres', q{SELECT test_ws_obj_copy('twice', 'twice2', 'both', true)});
is($node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('twice2')}), $twice_first, 'a schema restored once reads as it was');
$node->safe_psql('postgres', q{SET ROLE g_stable; DROP VIEW twice.w; CREATE VIEW twice.w AS SELECT v FROM twice.t WHERE v IN ('a', 'c'); DROP VIEW twice2.w});
my $twice_second = $node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('twice')});
$node->safe_psql('postgres', q{SELECT test_ws_obj_copy('twice', 'twice2', 'both', true)});
is($node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('twice2')}), $twice_second, 'and restored from another commit reads as that');
is($node->safe_psql('postgres', q{SELECT count(*) FROM pg_ws_print WHERE prtnamespace = 'twice2'::regnamespace}), '1', 'with one entry, not two');

# a long key is fine
{
	# (names that do not compress, or it would fit in an index anyway)
	my @names = map { 'd' . substr(sha1_hex($_) . sha1_hex("x$_"), 0, 60) } 1 .. 100;
	my $domains = join('; ', map { "CREATE DOMAIN lk.$_ AS integer" } @names);
	my $args = join(', ', map { "lk.$_" } @names);
	$node->safe_psql('postgres', qq{CREATE SCHEMA lk AUTHORIZATION g_stable; CREATE SCHEMA lk2 AUTHORIZATION g_stable; SET ROLE g_stable; $domains;
		CREATE FUNCTION lk.f($args) RETURNS text LANGUAGE sql BEGIN ATOMIC SELECT 'a'; END});
	my $lk = $node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('lk')});
	$node->safe_psql('postgres', q{SELECT test_ws_obj_copy('lk', 'lk2', 'both', true)});
	is($node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('lk2')}), $lk, 'a function whose key is too long for an index is remembered');
}

# one read while what is remembered changes is to try again
{
	my $old = $node->background_psql('postgres', on_error_stop => 0);
	$old->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
	$old->query_safe('SELECT count(*) FROM pg_class');
	$node->safe_psql('postgres', q{SET allow_system_table_mods = on; DELETE FROM pg_ws_print WHERE prtnamespace = 'twice2'::regnamespace});
	$old->{stderr} = '';
	$old->query(q{SELECT test_ws_obj_bytes('twice2')});
	like($old->{stderr}, qr/has changed since/, 'a read at a snapshot from before what is remembered changed tries again');
	$old->quit;
}

# lost, what it was for reads as changed, and nothing else; and it reads so from then on
my %before = map { my ($key, $v) = split /\t/; $key => $v } split /\n/, $held;
$node->safe_psql('postgres', q{SET allow_system_table_mods = on; DELETE FROM pg_ws_print WHERE prtnamespace = 'sp2'::regnamespace});
my %lost = map { my ($key, $v) = split /\t/; $key => $v } split /\n/, $node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('sp2')});
is(scalar(grep { ($lost{$_} // '') ne $before{$_} } keys %before), $remembered, 'lost, only what it remembered reads as changed');

# a schema dropped takes them with it
$node->safe_psql('postgres', q{DROP SCHEMA sp3 CASCADE});
is($node->safe_psql('postgres', q{SELECT count(*) FROM pg_ws_print p WHERE NOT EXISTS (SELECT 1 FROM pg_namespace n WHERE n.oid = p.prtnamespace)}), '0', 'a dropped schema leaves nothing');

# they go with a branch's catalogs: a branch of a restored schema reads as its commit, and
# no other branch sees them
my $from = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
my $to = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
$node->safe_psql('postgres', qq{SET catalog_branch = $from; CREATE SCHEMA bsrc VERSIONED; CREATE SCHEMA bdst VERSIONED;
	CREATE TABLE bsrc.t (v varchar(10)); CREATE TABLE bdst.t (v varchar(10));
	CREATE ROLE g_branch NOLOGIN; ALTER TABLE bsrc.t OWNER TO g_branch; ALTER TABLE bdst.t OWNER TO g_branch;
	CREATE VIEW bsrc.w AS SELECT v FROM bsrc.t WHERE v IN ('a', 'b'); ALTER VIEW bsrc.w OWNER TO g_branch});
my $branch_held = $node->safe_psql('postgres', qq{SET catalog_branch = $from; SELECT test_ws_obj_bytes('bsrc')});
$node->safe_psql('postgres', qq{SET catalog_branch = $from; SELECT test_ws_obj_copy('bsrc', 'bdst', 'both', true)});
is($node->safe_psql('postgres', qq{SET catalog_branch = $from; SELECT test_ws_obj_bytes('bdst')}), $branch_held, 'restored on a branch, it reads as its commit');
is($node->safe_psql('postgres', qq{SET catalog_branch = $from; SELECT count(*) FROM pg_ws_print p JOIN pg_namespace n ON n.oid = p.prtnamespace WHERE n.nspname = 'bdst'}), '1', 'with what it remembers');
$node->safe_psql('postgres', "SELECT pg_branch_catalog_copy($from, $to)");
is($node->safe_psql('postgres', qq{SET catalog_branch = $to; SELECT test_ws_obj_bytes('bdst')}), $branch_held, 'a branch of it reads so too');
is($node->safe_psql('postgres', q{SET catalog_branch = 0; SELECT count(*) FROM pg_ws_print p WHERE p.prtkey = 'view/w'}), '0', 'and what is on no branch sees nothing of it');
$node->safe_psql('postgres', qq{SET catalog_branch = $to; ALTER TABLE bdst.t RENAME COLUMN v TO v2});
isnt($node->safe_psql('postgres', qq{SET catalog_branch = $to; SELECT test_ws_obj_bytes('bdst')}), $branch_held, 'a change on the branch reads as one');
is($node->safe_psql('postgres', qq{SET catalog_branch = $from; SELECT test_ws_obj_bytes('bdst')}), $branch_held, 'and not on the other');

# what printing itself loses is refused, as no text held would make it again
my $vec = q{(ARRAY[1]::int2vector)::text <> ''};
for my $case (
	[ q{CREATE VIEW w AS SELECT ARRAY[1, 2]::int2vector AS x}, 'int2vector' ],
	[ q{CREATE VIEW w AS SELECT ARRAY[1]::oidvector AS x}, 'oidvector' ],
	[ q{CREATE TABLE t0 (i integer); CREATE VIEW w AS SELECT i FROM t0 WHERE (ARRAY[1]::oidvector)::text <> ''}, 'int2vector or oidvector' ],
	[ q{CREATE FUNCTION f() RETURNS int2vector LANGUAGE sql BEGIN ATOMIC SELECT ARRAY[1]::int2vector AS x; END}, 'int2vector' ],
	[ qq{CREATE FUNCTION f(a boolean DEFAULT $vec) RETURNS boolean LANGUAGE sql RETURN a}, 'int2vector' ],
	[ q{CREATE TABLE t0 (i integer, x text DEFAULT (ARRAY[1]::int2vector)::text)}, 'int2vector or oidvector' ],
	[ q{CREATE DOMAIN dd AS text DEFAULT ((ARRAY[1, 2]::int2vector))::text}, 'int2vector' ],
	[ qq{CREATE DOMAIN dd AS boolean CHECK ($vec)}, 'int2vector' ],
	[ qq{CREATE TABLE t0 (i integer, CONSTRAINT k CHECK ($vec))}, 'int2vector' ],
	[ q{CREATE TABLE t0 (i integer); CREATE INDEX k ON t0 (i) WHERE (ARRAY[i::oid]::oidvector) IS NOT NULL}, 'int2vector' ],
	[ q{CREATE TABLE t0 (i integer); CREATE INDEX k ON t0 ((ARRAY[i::oid]::oidvector))}, 'int2vector' ],
	[ q{CREATE TABLE t0 (i integer) PARTITION BY LIST ((ARRAY[i::oid]::oidvector))}, 'int2vector' ],
	[ q{CREATE TABLE t0 (i integer, b text); CREATE STATISTICS s ON ((ARRAY[i::oid]::oidvector) IS NULL), i FROM t0}, 'int2vector' ],
	[ qq{CREATE FUNCTION g() RETURNS trigger LANGUAGE plpgsql AS 'BEGIN RETURN NEW; END'; CREATE TABLE t0 (i integer); CREATE TRIGGER k BEFORE INSERT ON t0 FOR EACH ROW WHEN ($vec) EXECUTE FUNCTION g()}, 'int2vector' ],
	[ qq{CREATE TABLE t0 (i integer); CREATE POLICY k ON t0 USING ($vec)}, 'int2vector' ],
	[ qq{CREATE TABLE t0 (i integer); CREATE POLICY k ON t0 FOR INSERT WITH CHECK ($vec)}, 'int2vector' ],
	[ qq{CREATE TABLE t0 (i integer); CREATE RULE k AS ON DELETE TO t0 WHERE $vec DO ALSO DELETE FROM t0}, 'int2vector' ],
	[ q{CREATE TABLE t0 (i integer); CREATE VIEW w AS SELECT row_to_json(x) AS j FROM (SELECT i + 1, i + 2 FROM t0) x}, 'whole row' ],
	[ q{CREATE TABLE t0 (i integer, b integer); CREATE VIEW w AS SELECT row_to_json(x) AS j FROM (SELECT i AS a, b AS a FROM t0) x}, 'whole row' ],
	[ q{CREATE TABLE t0 (i integer); CREATE VIEW w AS SELECT row_to_json(x) AS j FROM (SELECT i, i FROM t0) x}, 'whole row' ],
	[ q{CREATE TABLE t0 (i integer); CREATE VIEW w AS WITH x AS (SELECT i, i FROM t0) SELECT row_to_json(x) AS j FROM x}, 'whole row' ],
	[ q{CREATE VIEW w AS SELECT row_to_json(x) AS j FROM (VALUES (1, 2)) x(a, a)}, 'whole row' ],
	[ q{CREATE TABLE t0 (i integer); CREATE FUNCTION f() RETURNS SETOF json LANGUAGE sql BEGIN ATOMIC SELECT row_to_json(x) AS j FROM (SELECT i, i FROM t0) x; END}, 'whole row' ],
	[ q{CREATE TABLE t0 (i integer); CREATE POLICY k ON t0 USING ((SELECT row_to_json(x)::text FROM (SELECT 1 AS a, 2 AS a) x) IS NOT NULL)}, 'whole row' ],
	[ q{CREATE TABLE t0 (i integer); CREATE VIEW w AS SELECT (SELECT row_to_json(x) FROM (SELECT 1) y) AS j FROM (SELECT i, i FROM t0) x}, 'whole row' ],
	[ q{CREATE TABLE t0 (i integer); CREATE VIEW w AS SELECT y.j FROM (SELECT i, i FROM t0) x, LATERAL (SELECT row_to_json(x) AS j) y}, 'whole row' ],
	[ q{CREATE TABLE t1 (i integer); CREATE TABLE t2 (i integer); CREATE VIEW w AS SELECT row_to_json(u) AS j FROM t1 a JOIN t2 b USING (i) AS u}, 'whole row of a join' ],
	[ q{CREATE TABLE t1 (i integer); CREATE TABLE t2 (i integer); CREATE VIEW w AS SELECT a.i FROM t1 a JOIN t2 b USING (i) AS u WHERE to_jsonb(u) ? 'i'}, 'whole row of a join' ],
	[ q{CREATE TABLE t1 (i integer); CREATE TABLE t2 (k integer); CREATE VIEW w AS SELECT row_to_json(j) AS r FROM (t1 JOIN t2 ON i = k) AS j}, 'whole row of a join' ],
	[ q{CREATE TABLE t1 (i integer); CREATE TABLE t2 (i integer); CREATE VIEW w AS SELECT row_to_json(x) AS j FROM (SELECT 1 AS i, 9 AS z) x, t1 a FULL JOIN t2 c USING (i)}, "named as a USING's" ],
	[ q{CREATE TABLE t1 (i integer); CREATE TABLE t2 (i bigint); CREATE VIEW w AS SELECT row_to_json(x) AS j FROM (SELECT 1 AS i, 9 AS z) x, t1 a LEFT JOIN t2 c USING (i)}, "named as a USING's" ],
	[ q{CREATE TABLE t1 (i integer); CREATE TABLE t3 (i integer); CREATE TABLE k1 (k integer); CREATE TABLE k2 (k integer); CREATE VIEW w AS SELECT row_to_json(x) AS j FROM (SELECT 1 AS k, 9 AS z) x, t1 a FULL JOIN t3 c USING (i), k1 JOIN k2 USING (k)}, "named as a USING's" ],
	[ q{CREATE VIEW w AS SELECT q.j FROM ((SELECT 1 AS i, 9 AS z) x JOIN LATERAL (SELECT row_to_json(x) AS j, 1 AS i) y USING (i)) AS q(r)}, "named as a USING's" ],
	[ q{CREATE TABLE t1 (i integer); CREATE VIEW w AS SELECT q.j FROM ((SELECT 1 AS i, 9 AS z) x CROSS JOIN LATERAL (SELECT row_to_json(x) AS j, 1 AS w) y) AS q(a, b, j, i) JOIN t1 USING (i)}, "named as a USING's" ],
	[ q{CREATE TABLE t1 (i integer); CREATE TABLE t3 (i integer); CREATE VIEW w AS SELECT row_to_json(x) AS j FROM (SELECT 1 AS i_1, 9 AS z) x, t1 a FULL JOIN t3 c USING (i), t1 d JOIN t3 e USING (i)}, "named as a USING's" ],
	[ q{CREATE TABLE t1 (aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa integer); CREATE TABLE t3 (aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa integer); CREATE VIEW w AS SELECT row_to_json(x) AS j FROM (SELECT 1 AS aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa_1, 9 AS z) x, t1 a FULL JOIN t3 c USING (aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa), t1 d JOIN t3 e USING (aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa)}, "named as a USING's" ],
	[ q{CREATE TABLE t1 (k integer); CREATE VIEW w AS SELECT q.j FROM (((SELECT 1 AS i, 9 AS z) x JOIN LATERAL (SELECT row_to_json(x) AS j, 1 AS i) y USING (i)) CROSS JOIN t1) AS q}, "named as a USING's" ],
	[ q{CREATE TABLE t1 (i integer); CREATE VIEW w AS SELECT q.j FROM t1 JOIN ((SELECT 1 AS i, 9 AS z) x CROSS JOIN LATERAL (SELECT row_to_json(x) AS j, 1 AS w) y) AS q(a, b, j, i) USING (i)}, "named as a USING's" ],
	[ q{CREATE TABLE t1 (i integer); CREATE FUNCTION tf() RETURNS SETOF t1 LANGUAGE sql AS 'SELECT 1'; CREATE VIEW w AS SELECT x.o FROM tf() WITH ORDINALITY x(a, o); ALTER TABLE t1 ADD n integer}, 'has changed since' ],
	[ q{CREATE TABLE t1 (i integer); CREATE FUNCTION tf() RETURNS SETOF t1 LANGUAGE sql AS 'SELECT 1'; CREATE VIEW w AS SELECT x.q FROM ROWS FROM (tf(), generate_series(7, 7)) x(a, q); ALTER TABLE t1 ADD n integer}, 'has changed since' ])
{
	my ($sql, $why) = @$case;
	$node->safe_psql('postgres', qq{DROP SCHEMA IF EXISTS lost CASCADE; CREATE SCHEMA lost AUTHORIZATION g_stable; SET ROLE g_stable; SET search_path = lost; $sql});
	my ($rcl, $outl, $errl) = $node->psql('postgres', q{SELECT test_ws_obj_bytes('lost')});
	isnt($rcl, 0, "refused: $sql");
	like($errl, qr/\Q$why\E/, "for why: $sql");
}
$node->safe_psql('postgres', q{DROP SCHEMA lost CASCADE});

# what looks like it but loses nothing is held
$node->safe_psql('postgres', q{CREATE SCHEMA kept AUTHORIZATION g_stable; SET ROLE g_stable; SET search_path = kept; CREATE TABLE t0 (i integer, b integer);
	CREATE VIEW a AS SELECT 1 AS k FROM (SELECT i + 1, i + 2 FROM t0) x;
	CREATE VIEW b AS SELECT 1 AS k FROM (SELECT i AS a, b AS a FROM t0) x;
	CREATE VIEW c AS SELECT row_to_json(x) AS j FROM (SELECT i AS "?column?1", b AS "?column?2" FROM t0) x;
	CREATE VIEW d AS SELECT row_to_json(t0) AS j FROM t0 JOIN t0 u USING (i);
	CREATE VIEW e AS SELECT row_to_json(ROW(1, 2)) AS r;
	CREATE TABLE gone (a integer, b integer, c integer, d integer); ALTER TABLE gone DROP COLUMN a; ALTER TABLE gone DROP COLUMN b;
	CREATE VIEW f AS SELECT row_to_json(gone) AS j FROM gone;
	CREATE VIEW g AS SELECT row_to_json(x) AS j FROM gone x(p);
	CREATE TYPE two AS (a integer, b integer); CREATE FUNCTION fr() RETURNS two LANGUAGE sql AS 'SELECT 1, 2';
	CREATE VIEW h AS SELECT row_to_json(x) AS j FROM fr() x(a, a);
	CREATE VIEW m AS SELECT row_to_json(x) AS j FROM (SELECT 1 AS id) x JOIN t0 ON id = i;
	CREATE TABLE f1 (id integer); CREATE TABLE f2 (id integer);
	CREATE VIEW n AS SELECT row_to_json(x) AS j FROM (SELECT 1 AS id) x JOIN f1 USING (id);
	CREATE VIEW o AS SELECT row_to_json(x) AS j FROM (SELECT 1 AS z) x, f1 FULL JOIN f2 USING (id);
	CREATE VIEW p AS SELECT x.ord FROM fr() WITH ORDINALITY x(a, b, ord);
	CREATE VIEW q AS SELECT x.c FROM ROWS FROM (fr(), generate_series(1, 2)) x(a, b, c);
	CREATE VIEW r AS SELECT y.j FROM f1 a FULL JOIN f2 c USING (id), LATERAL (SELECT row_to_json(x) AS j FROM (SELECT 1 AS id) x) y;
	CREATE VIEW s AS SELECT row_to_json(x) AS j FROM (SELECT 1 AS id, 9 AS z) x, (f1 FULL JOIN f2 USING (id)) AS q;
	CREATE VIEW u AS SELECT row_to_json(x) AS j FROM (SELECT 1 AS id, 9 AS z) x, f1 JOIN f2 USING (id);
	CREATE VIEW y AS SELECT row_to_json(x) AS j FROM (SELECT 1 AS id, 9 AS z) x JOIN f1 USING (id);
	CREATE VIEW y2 AS SELECT row_to_json(x) AS j FROM (SELECT 1 AS id_, 2 AS id_1a, 3 AS id_1_) x, f1 FULL JOIN f2 USING (id), f1 d JOIN f2 e USING (id);
	CREATE VIEW z AS SELECT q.j FROM ((SELECT 1 AS k, 9 AS z) x CROSS JOIN LATERAL (SELECT row_to_json(x) AS j) y) AS q;
	CREATE DOMAIN dtwo AS two; CREATE FUNCTION fd() RETURNS dtwo LANGUAGE sql AS 'SELECT ROW(1, 2)::two';
	CREATE VIEW v AS SELECT x.o FROM fd() WITH ORDINALITY x(a, b, o);
	CREATE TABLE grows (a integer); CREATE FUNCTION fg() RETURNS SETOF grows LANGUAGE sql AS 'SELECT 1';
	CREATE VIEW x AS SELECT y.o FROM fg() WITH ORDINALITY y(a, o);
	ALTER TABLE grows ADD COLUMN gone integer; ALTER TABLE grows DROP COLUMN gone});
like($node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('kept')}), qr/^view\/d\t.*^view\/e\t.*^view\/f\t.*^view\/g\t.*^view\/h\t.*^view\/m\t.*^view\/n\t.*^view\/o\t.*^view\/p\t.*^view\/q\t.*^view\/r\t.*^view\/s\t.*^view\/u\t.*^view\/v\t.*^view\/x\t.*^view\/y\t.*^view\/z\t/ms, 'columns of one name no whole row reads are held, a join\'s, ROW(), a whole row of a table with columns dropped, of a type, beside a join or a FULL JOIN of other names, a USING kept only in another query or under an alias, and a function\'s columns, of a domain too, while its type has not changed but for what is dropped');
$node->safe_psql('postgres', q{DROP SCHEMA kept CASCADE});

$node->stop;
done_testing();
