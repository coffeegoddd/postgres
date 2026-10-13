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
-- Table access methods: creating storage dispatches to the engine interface,
-- which the stub engines don't implement.
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
FROM pg_opclass WHERE opcmethod = (SELECT oid FROM pg_am WHERE amname = 'prolly')
ORDER BY opcname;
SELECT amvalidate(oid) FROM pg_opclass
WHERE opcmethod = (SELECT oid FROM pg_am WHERE amname = 'prolly');
CREATE INDEX ON prolly_heap USING prolly (a);
CREATE INDEX ON prolly_heap USING prolly (b);
CREATE INDEX ON prolly_heap USING prolly (c);
CREATE INDEX ON prolly_heap USING prolly (a, c);
DROP TABLE prolly_heap;

--
-- Version-control functions dispatch to the refs, diff/merge and transfer
-- interfaces.
--
SELECT prolly_branch_create('feature', NULL);
SELECT prolly_branch_create(NULL, NULL);
SELECT prolly_checkout('feature');
SELECT prolly_commit('first commit');
SELECT * FROM prolly_diff('main', 'feature');
SELECT prolly_merge('feature');
SELECT prolly_clone('file:///nowhere');
SELECT prolly_fetch('file:///nowhere', 'main');
SELECT prolly_push('file:///nowhere', 'main');
