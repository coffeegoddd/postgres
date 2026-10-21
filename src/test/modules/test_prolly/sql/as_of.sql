-- dolt_at: a table's rows as any ref has them, read from its tree, never
-- written to pages (Dolt's AS OF); dolt_history_table: its rows at each
-- commit, with the commit (Dolt's dolt_history_<table>).
CREATE DATABASE regress_as_of;
\c regress_as_of
CREATE EXTENSION test_prolly;
CREATE SCHEMA ao VERSIONED;
CREATE TABLE ao.t (id int PRIMARY KEY, v text);
CREATE TABLE ao.bag (v int);
CREATE TABLE ao.big (id int PRIMARY KEY, body text STORAGE EXTERNAL);
INSERT INTO ao.t VALUES (1, 'one'), (2, 'two');
INSERT INTO ao.bag VALUES (7), (7), (8);
INSERT INTO ao.big VALUES (1, repeat('a', 100000));
SELECT length(dolt_commit('-Am', 'first')::text) AS committed;
SELECT dolt_hashof('HEAD') AS c1 \gset
UPDATE ao.t SET v = 'uno' WHERE id = 1;
INSERT INTO ao.t VALUES (3, 'three');
DELETE FROM ao.bag WHERE v = 8;
UPDATE ao.big SET body = repeat('b', 100000);
SELECT length(dolt_commit('-Am', 'second')::text) AS committed;
-- a column added: old rows read as the table is defined now
ALTER TABLE ao.t ADD COLUMN w int DEFAULT 0;
UPDATE ao.t SET w = id * 10;
SELECT length(dolt_commit('-Am', 'third')::text) AS committed;
DELETE FROM ao.t WHERE id = 2;

SELECT * FROM dolt_at(:'c1', NULL::ao.t) ORDER BY id;
SELECT * FROM dolt_at('HEAD~1', NULL::ao.t) ORDER BY id;
SELECT * FROM dolt_at('HEAD', NULL::ao.t) ORDER BY id;
SELECT * FROM dolt_at('WORKING', NULL::ao.t) ORDER BY id;
SELECT * FROM dolt_at('main', NULL::ao.t) ORDER BY id;
-- a keyless table's copies; a value stored out of line
SELECT v, count(*) FROM dolt_at(:'c1', NULL::ao.bag) GROUP BY v ORDER BY v;
SELECT v, count(*) FROM dolt_at('HEAD', NULL::ao.bag) GROUP BY v ORDER BY v;
SELECT id, left(body, 3), length(body) FROM dolt_at(:'c1', NULL::ao.big);

-- nothing written: no branch made, the table's pages as they were
SELECT count(*) AS branches FROM dolt_branches \gset
SELECT pg_relation_size('ao.t') AS pages \gset
SELECT count(*) FROM dolt_at(:'c1', NULL::ao.t);
SELECT count(*) = :branches AS no_branch FROM dolt_branches;
SELECT pg_relation_size('ao.t') = :pages AS no_page;

-- each commit's rows
SELECT (row).id, (row).v, (row).w, c.message
FROM dolt_history_table(NULL::ao.t) h JOIN dolt_log c ON c.commit_hash = h.commit_hash
ORDER BY c.commit_order DESC, (row).id;
SELECT count(*) = 3 + 2 + 2 AS each_commits_copies FROM dolt_history_table(NULL::ao.bag)
WHERE committer IS NOT NULL AND commit_date IS NOT NULL;

SELECT * FROM dolt_at('no_such_ref', NULL::ao.t);
SELECT * FROM dolt_at(NULL, NULL::ao.t);

-- the planner's estimate of a commit's rows: the count its tree records,
-- not prorows' 1000; the default where it can't tell (WORKING, a ref
-- that doesn't resolve, a parameter in a generic plan, a commit without
-- the table).  A keyless table's rows count once each: bag's one row has
-- two copies of 7.
CREATE FUNCTION pg_temp.estimate(query text) RETURNS float8 LANGUAGE plpgsql AS $$
DECLARE
	plan json;
BEGIN
	EXECUTE 'EXPLAIN (FORMAT JSON) ' || query INTO plan;
	RETURN (plan->0->'Plan'->>'Plan Rows')::float8;
END
$$;
CREATE TABLE ao.many (id int PRIMARY KEY, v text);
SELECT dolt_hashof('HEAD') AS before_many \gset
INSERT INTO ao.many SELECT g, 'v' || g FROM generate_series(1, 2500) g;
SELECT length(dolt_commit('-Am', 'many')::text) AS committed;
DELETE FROM ao.many WHERE id > 40;
SELECT length(dolt_commit('-Am', 'fewer')::text) AS committed;
SELECT pg_temp.estimate($q$SELECT * FROM dolt_at('HEAD~1', NULL::ao.many)$q$) AS at_many,
	   pg_temp.estimate($q$SELECT * FROM dolt_at('HEAD', NULL::ao.many)$q$) AS at_fewer,
	   pg_temp.estimate($q$SELECT * FROM dolt_at('HEAD', NULL::ao.bag)$q$) AS keyless,
	   pg_temp.estimate($q$SELECT * FROM dolt_at('WORKING', NULL::ao.many)$q$) AS working,
	   pg_temp.estimate($q$SELECT * FROM dolt_at('no_such_ref', NULL::ao.many)$q$) AS no_ref,
	   pg_temp.estimate(format($q$SELECT * FROM dolt_at(%L, NULL::ao.many)$q$, :'before_many')) AS no_table;
-- a parameter: the default in a generic plan, the commit's in a custom one
PREPARE at_ref(text) AS SELECT * FROM dolt_at($1, NULL::ao.many);
SET plan_cache_mode = force_generic_plan;
SELECT pg_temp.estimate('EXECUTE at_ref(''HEAD~1'')') AS generic;
SET plan_cache_mode = force_custom_plan;
SELECT pg_temp.estimate('EXECUTE at_ref(''HEAD~1'')') AS custom;
RESET plan_cache_mode;
DEALLOCATE at_ref;

\c contrib_regression
DROP DATABASE regress_as_of;
