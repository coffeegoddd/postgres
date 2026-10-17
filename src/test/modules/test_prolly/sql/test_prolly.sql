--
-- Prolly storage: registered implementations, conformance suites, and
-- canonical values.
--
CREATE EXTENSION test_prolly;
\set VERBOSITY terse

-- Every implementation of every interface.
SELECT interface, name, impl_id, format_version
FROM prolly_implementations();

-- Conformance suites against every built-in implementation.  Interfaces
-- that only have stubs report "not implemented" for everything but
-- "identity".
SELECT * FROM prolly_conformance() WHERE implementation NOT LIKE 'test%';

-- Filters, and the suite passing and failing on the test-only hashes
-- registered by this module.
SELECT * FROM prolly_conformance('tree', 'cow');
SELECT * FROM prolly_conformance('content_hash');
SELECT * FROM prolly_conformance('no_such_interface');

--
-- Canonical value bytes carry no local OIDs and no DDL history: the same
-- values of user-defined types encode identically after the types are
-- dropped and recreated (with new OIDs), and a composite type that dropped
-- an attribute encodes like one that never had it.
--
CREATE TYPE prolly_mood AS ENUM ('sad', 'ok', 'happy');
CREATE TYPE prolly_pair AS (m prolly_mood, n int4, tags text[]);
SELECT prolly_canonical_value(ARRAY['happy', NULL, 'sad']::prolly_mood[]) AS enum_array,
       prolly_canonical_value(ROW('ok', 3, '{a,b}')::prolly_pair) AS composite
\gset
SELECT prolly_canonical_round_trip(ARRAY['happy', NULL, 'sad']::prolly_mood[]),
       prolly_canonical_round_trip(ROW('ok', 3, '{a,b}')::prolly_pair),
       prolly_canonical_round_trip('[2:3]={1.5,NULL}'::numeric[]),
       prolly_canonical_round_trip('{"a": [1, 2]}'::jsonb);
SELECT 'prolly_mood'::regtype::oid AS old_oid \gset
DROP TYPE prolly_pair;
DROP TYPE prolly_mood;
CREATE TYPE prolly_mood AS ENUM ('sad', 'ok', 'happy');
CREATE TYPE prolly_pair AS (m prolly_mood, n int4, tags text[]);
SELECT 'prolly_mood'::regtype::oid <> :old_oid AS new_oid,
       prolly_canonical_value(ARRAY['happy', NULL, 'sad']::prolly_mood[]) = :'enum_array'
         AS same_enum_array,
       prolly_canonical_value(ROW('ok', 3, '{a,b}')::prolly_pair) = :'composite'
         AS same_composite;
CREATE TYPE prolly_dropped AS (a int4, b int8, c text);
ALTER TYPE prolly_dropped DROP ATTRIBUTE b;
CREATE TYPE prolly_never AS (a int4, c text);
SELECT prolly_canonical_value(ROW(1, 'x')::prolly_dropped) =
       prolly_canonical_value(ROW(1, 'x')::prolly_never) AS same_after_drop;
DROP TYPE prolly_pair, prolly_mood, prolly_dropped, prolly_never;
