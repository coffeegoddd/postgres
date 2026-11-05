# Copyright (c) 2026, PostgreSQL Global Development Group
#
# What a commit holds must print the same once it has been restored and printed
# again.  Postgres's printing does not promise that of itself, so this holds a broad
# set of expressions in each place one can be, and compares every object's bytes.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

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
		d date, ts timestamp, tz timestamptz, j json, jb jsonb, a integer[], p pt, u uuid, m m, dm dm, dv dv, ad ad);
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

# printing is what is stable, so what it does not ask for is as Postgres has always been
is( $node->safe_psql('postgres', q{SELECT pg_get_viewdef('sp.w1'::regclass) LIKE '%ANY ((ARRAY[%])::text[])%'}), 't', 'ordinary printing is as it was');
is( $node->safe_psql('postgres', q{SET stable_deparse = on; SELECT pg_get_viewdef('sp.w1'::regclass) LIKE '%ANY (ARRAY[(%)::text%'}), 't', 'and the setting prints what reading it back gives');

my $held = $node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('sp')});
my %was = map { my ($key, $v) = split /\t/; $key => $v } split /\n/, $held;
cmp_ok(scalar(keys %was), '>', 3 * scalar(@exprs), 'every expression is held in each place');
is($node->safe_psql('postgres', q{DO $$ BEGIN PERFORM test_ws_obj_bytes('sp'); END $$; SHOW stable_deparse}), 'off', 'and capture leaves the setting as it was');

$node->safe_psql('postgres', q{SELECT test_ws_obj_copy('sp', 'sp2', 'before')});
$node->safe_psql('postgres', "SET ROLE g_stable; SET search_path = sp2;\n" . $tables);
$node->safe_psql('postgres', q{SELECT test_ws_obj_copy('sp', 'sp2', 'after')});

# (all at once, so this says which, not only that one did)
my %is = map { my ($key, $v) = split /\t/; $key => $v } split /\n/, $node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('sp2')});
my @differ = grep { ($is{$_} // '') ne $was{$_} } sort keys %was;
for my $key (@differ[0 .. ($#differ < 9 ? $#differ : 9)])
{
	diag("$key:\n  was " . pack('H*', $was{$key}) . "\n  is  " . pack('H*', $is{$key} // ''));
}
is(scalar(@differ), 0, 'every object is the same bytes restored');
is(scalar(keys %is), scalar(keys %was), 'and there are no others');

# a second round, as a form that only settled later would pass the first by chance
$node->safe_psql('postgres', q{CREATE SCHEMA sp3 AUTHORIZATION g_stable; SELECT test_ws_obj_copy('sp2', 'sp3', 'before')});
$node->safe_psql('postgres', "SET ROLE g_stable; SET search_path = sp3;\n" . $tables);
$node->safe_psql('postgres', q{SELECT test_ws_obj_copy('sp2', 'sp3', 'after')});
is($node->safe_psql('postgres', q{SELECT test_ws_obj_bytes('sp3')}), $held, 'and again');

$node->stop;
done_testing();
