--
-- Prolly storage: registered implementations, conformance suites, and the
-- stub table AMs, index AM and version-control functions reached from SQL.
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
SELECT * FROM prolly_conformance('engine', 'd1');
SELECT * FROM prolly_conformance('content_hash');
SELECT * FROM prolly_conformance('no_such_interface');

--
-- Table access methods: creating storage dispatches to the engine interface.
-- Engine d1 creates its trees (its tables are tested in d1_tables); the
-- others are stubs.
--
CREATE TABLE prolly_t1 (a int) USING prolly_d1;
CREATE TABLE prolly_t2 (a int) USING prolly_d2;
CREATE TABLE prolly_t3a (a int) USING prolly_d3a;
CREATE TABLE prolly_t3b (a int) USING prolly_d3b;
CREATE TABLE prolly_t4 (a int) USING prolly_d4;

SET default_table_access_method = prolly_d1;
CREATE TABLE prolly_default (a int);
RESET default_table_access_method;

CREATE TABLE prolly_heap (a int, b bigint, c text);
INSERT INTO prolly_heap VALUES (1, 2, 'three');
ALTER TABLE prolly_heap SET ACCESS METHOD prolly_d2;
SELECT amname FROM pg_class c JOIN pg_am a ON a.oid = c.relam
WHERE c.relname = 'prolly_heap';

--
-- Index access method: operator classes follow the btree contract, and
-- building an index reaches the stub.
--
SELECT opcname, opcintype::regtype
FROM pg_opclass WHERE opcmethod = (SELECT oid FROM pg_am WHERE amname = 'prolly_d1_index')
ORDER BY opcname;
SELECT amvalidate(oid) FROM pg_opclass
WHERE opcmethod = (SELECT oid FROM pg_am WHERE amname = 'prolly_d1_index');
CREATE INDEX ON prolly_heap USING prolly_d1_index (a);
CREATE INDEX ON prolly_heap USING prolly_d1_index (b);
CREATE INDEX ON prolly_heap USING prolly_d1_index (c);
CREATE INDEX ON prolly_heap USING prolly_d1_index (a, c);
DROP TABLE prolly_heap;

--
-- Version-control functions dispatch to the refs, diff/merge and transfer
-- interfaces.
--
SELECT prolly_branch_create('feature', NULL) IS NULL AS created;
SELECT prolly_branch_create(NULL, NULL);
SELECT prolly_checkout('feature') IS NULL AS checked_out;
SELECT prolly_commit('first commit') IS NOT NULL AS committed;
-- (a head moved alone, as a fast-forward moves it, is a change of the refs)
SELECT test_refs_epoch() AS refs_epoch \gset
SELECT test_refs_set_head('HEAD');
SELECT test_refs_epoch() > :refs_epoch AS epoch_moved;
SELECT * FROM prolly_diff('main', 'feature');
SELECT prolly_merge('feature') IS NOT NULL AS merged;
SELECT prolly_checkout('main') IS NULL AS back_on_main;

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
