-- Binary receive can normalize two different send spellings to one value.
-- Canonical keys and values must store that normalized representation.
CREATE DATABASE regress_send_canonical;
\c regress_send_canonical
CREATE EXTENSION test_prolly;
CREATE SCHEMA sn VERSIONED;
CREATE EXTENSION test_prolly_normalizing SCHEMA sn;
SET search_path = sn, public;
CREATE DOMAIN sn.normal_domain AS sn.test_normalizing;
CREATE TYPE sn.normal_pair AS (v sn.test_normalizing, n int);
CREATE TYPE sn.normal_range AS RANGE (subtype = sn.test_normalizing);

SELECT test_normalizing_send('ABC') <> test_normalizing_send('abc') AS distinct_send,
    prolly_canonical_value('ABC'::test_normalizing) =
    prolly_canonical_value('abc'::test_normalizing) AS same_canonical,
    prolly_canonical_round_trip('ABC'::test_normalizing)::text AS normalized;
SELECT prolly_canonical_value(ARRAY['ABC', NULL]::test_normalizing[]) =
    prolly_canonical_value(ARRAY['abc', NULL]::test_normalizing[]) AS same_array,
    prolly_canonical_value('ABC'::normal_domain) =
    prolly_canonical_value('abc'::normal_domain) AS same_domain,
    prolly_canonical_value(ROW('ABC', 1)::normal_pair) =
    prolly_canonical_value(ROW('abc', 1)::normal_pair) AS same_composite,
    prolly_canonical_value('[A,C)'::normal_range) =
    prolly_canonical_value('[a,c)'::normal_range) AS same_range,
    prolly_canonical_value('{[A,C)}'::normal_multirange) =
    prolly_canonical_value('{[a,c)}'::normal_multirange) AS same_multirange;
CREATE TABLE sn.t (id int PRIMARY KEY, v test_normalizing, n int);
CREATE TABLE sn.keyed (k test_normalizing PRIMARY KEY, n int);
CREATE TABLE sn.keyless (v test_normalizing);
INSERT INTO sn.t VALUES (1, 'base', 0);
INSERT INTO sn.keyed VALUES ('base', 0);
INSERT INTO sn.keyless VALUES ('base'), ('base');
SELECT length(dolt_commit('-Am', 'base')::text) AS committed;
SELECT dolt_branch('upper');
UPDATE sn.t SET v = 'abc';
UPDATE sn.keyed SET k = 'abc';
UPDATE sn.keyless SET v = 'abc';
SELECT length(dolt_commit('-Am', 'lower spelling')::text) AS committed;
SELECT dolt_checkout('upper');
UPDATE sn.t SET v = 'ABC';
UPDATE sn.keyed SET k = 'ABC';
UPDATE sn.keyless SET v = 'ABC';
SELECT length(dolt_commit('-Am', 'upper spelling')::text) AS committed;
SELECT prolly_root_hash('main') = prolly_root_hash('upper') AS same_root;
SELECT count(*) AS differences FROM dolt_diff('main', 'upper', NULL::sn.t);
SELECT test_fold_root(true) = prolly_root_hash('HEAD') AS full_matches;
-- Replacing the working spelling alone is not a change or a merge conflict.
UPDATE sn.t SET v = 'abc';
SELECT count(*) AS working_differences FROM dolt_diff('HEAD', 'WORKING', NULL::sn.t);
SELECT count(*) AS dirty_tables FROM dolt_status();
SELECT (dolt_merge('main'))[2:4];

-- A cell merge folds the working side before comparing it with tree values.
SELECT dolt_branch('cells');
UPDATE sn.t SET v = 'XYZ';
SELECT length(dolt_commit('-Am', 'change value')::text) AS committed;
SELECT dolt_checkout('cells');
UPDATE sn.t SET n = 1;
SELECT length(dolt_commit('-Am', 'change other cell')::text) AS committed;
SELECT (dolt_merge('upper'))[2:4];
SELECT id, v, n FROM sn.t;
SELECT count(*) AS conflicts FROM dolt_conflicts(NULL::sn.t);

-- Out-of-line values go through the same normalization, and a rebuild is
-- idempotent: it decodes the already normalized wire form.
INSERT INTO sn.t VALUES (2, repeat('AB', 5000)::test_normalizing, 2);
SELECT length(dolt_commit('-Am', 'large')::text) AS committed;
SELECT test_fold_root() AS before_rebuild \gset
DROP TABLE sn.t;
SELECT dolt_reset('--hard');
SELECT id, left(v::text, 3), length(v::text), n FROM sn.t ORDER BY id;
SELECT test_fold_root(true) = :'before_rebuild' AS rebuild_same_root;
-- Format 16's pre-normalized data must never be reused as a new baseline.
SELECT test_commit_of(test_root_with_format(prolly_root_hash('HEAD'), '16'),
    'old binary bytes') AS old_format \gset
SELECT dolt_reset('--hard', :'old_format');
\c contrib_regression
DROP DATABASE regress_send_canonical;
