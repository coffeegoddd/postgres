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
CREATE TYPE sn.pair_range AS RANGE (subtype = sn.normal_pair);

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
-- A range over a composite reaches the member's receive through record_recv.
SELECT prolly_canonical_value('["(ABC,1)","(ABC,2)")'::pair_range) =
    prolly_canonical_value('["(abc,1)","(abc,2)")'::pair_range) AS same_composite_range,
    prolly_canonical_value('{["(ABC,1)","(ABC,2)")}'::pair_multirange) =
    prolly_canonical_value('{["(abc,1)","(abc,2)")}'::pair_multirange) AS same_composite_multirange;
CREATE TABLE sn.t (id int PRIMARY KEY, v test_normalizing, n int);
CREATE TABLE sn.keyed (k test_normalizing PRIMARY KEY, n int);
CREATE TABLE sn.keyless (v test_normalizing);
CREATE TABLE sn.composite_key (k test_normalizing[], n int, PRIMARY KEY (k, n));
INSERT INTO sn.t VALUES (1, 'base', 0);
INSERT INTO sn.keyed VALUES ('base', 0);
INSERT INTO sn.keyless VALUES ('base'), ('base');
INSERT INTO sn.composite_key VALUES (ARRAY['base']::test_normalizing[], 1);
SELECT length(dolt_commit('-Am', 'base')::text) AS committed;
SELECT dolt_branch('upper');
UPDATE sn.t SET v = 'abc';
UPDATE sn.keyed SET k = 'abc';
UPDATE sn.keyless SET v = 'abc';
UPDATE sn.composite_key SET k = ARRAY['abc']::test_normalizing[];
SELECT length(dolt_commit('-Am', 'lower spelling')::text) AS committed;
SELECT dolt_checkout('upper');
UPDATE sn.t SET v = 'ABC';
UPDATE sn.keyed SET k = 'ABC';
UPDATE sn.keyless SET v = 'ABC';
UPDATE sn.composite_key SET k = ARRAY['ABC']::test_normalizing[];
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
-- Normalization must preserve equality under the actual primary-key opclass.
CREATE TABLE sn.unsafe (k test_raw PRIMARY KEY);
INSERT INTO sn.unsafe VALUES ('ABC');
INSERT INTO sn.unsafe VALUES ('abc');
UPDATE sn.unsafe SET k = 'ABC';
SELECT k FROM sn.unsafe;
CREATE TABLE sn.later_key (k test_raw);
INSERT INTO sn.later_key VALUES ('ABC');
ALTER TABLE sn.later_key ADD PRIMARY KEY (k);
-- The check reaches unsafe values nested in arrays, composites and ranges.
CREATE TYPE sn.raw_pair AS (v test_raw, n int);
CREATE TYPE sn.raw_pair_range AS RANGE (subtype = sn.raw_pair);
CREATE TABLE sn.unsafe_array (k test_raw[] PRIMARY KEY);
INSERT INTO sn.unsafe_array VALUES (ARRAY['ABC']::test_raw[]);
INSERT INTO sn.unsafe_array VALUES (ARRAY['abc']::test_raw[]);
CREATE TABLE sn.unsafe_composite (k sn.raw_pair PRIMARY KEY);
INSERT INTO sn.unsafe_composite VALUES (ROW('ABC', 1));
INSERT INTO sn.unsafe_composite VALUES (ROW('abc', 1));
CREATE TABLE sn.unsafe_range (k sn.raw_pair_range PRIMARY KEY);
INSERT INTO sn.unsafe_range VALUES ('["(ABC,1)","(ABC,2)")');
INSERT INTO sn.unsafe_range VALUES ('["(abc,1)","(abc,2)")');
-- Ranges and multiranges over a safe ordering may be keys.
CREATE TABLE sn.range_key (k normal_range PRIMARY KEY);
CREATE TABLE sn.multirange_key (k normal_multirange PRIMARY KEY);
INSERT INTO sn.range_key VALUES ('[A,C)');
INSERT INTO sn.multirange_key VALUES ('{[A,C)}');
SELECT k FROM sn.range_key;
SELECT k FROM sn.multirange_key;
DROP TABLE sn.unsafe, sn.later_key, sn.unsafe_array, sn.unsafe_composite,
    sn.unsafe_range, sn.range_key, sn.multirange_key;
-- A unique or exclusion index other than the key re-inserts normalized
-- values when a commit is reconstructed, so values it tells apart from
-- their normalized form are refused, as written and as such an index is built.
CREATE TABLE sn.uniq (id int PRIMARY KEY, v test_raw UNIQUE,
    w test_normalizing UNIQUE);
INSERT INTO sn.uniq VALUES (1, 'ABC', 'x');
INSERT INTO sn.uniq VALUES (2, 'abc', 'XYZ');
UPDATE sn.uniq SET v = 'DEF';
-- Expressions and predicates see what receive returns, whatever the opclass.
CREATE UNIQUE INDEX uniq_w_text ON sn.uniq ((w::text));
CREATE UNIQUE INDEX uniq_w_partial ON sn.uniq (id) WHERE w::text <> '';
CREATE TABLE sn.excl (id int PRIMARY KEY, v test_raw,
    EXCLUDE USING btree (v WITH =));
INSERT INTO sn.excl VALUES (1, 'ABC');
INSERT INTO sn.excl VALUES (2, 'abc');
SELECT length(dolt_commit('-Am', 'unique values')::text) AS committed;
DROP TABLE sn.uniq, sn.excl;
SELECT dolt_reset('--hard');
SELECT * FROM sn.uniq;
SELECT * FROM sn.excl;
DROP TABLE sn.uniq, sn.excl;
-- Format 16's pre-normalized data must never be reused as a new baseline.
SELECT test_commit_of(test_root_with_format(prolly_root_hash('HEAD'), '16'),
    'old binary bytes') AS old_format \gset
SELECT dolt_reset('--hard', :'old_format');
\c contrib_regression
DROP DATABASE regress_send_canonical;
