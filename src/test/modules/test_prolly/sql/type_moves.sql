-- Moving a branch to a commit that defines types or materialized views
-- otherwise (a reset, a fast-forward): each changed type is made again
-- under its name and the columns using it converted, before the rows are
-- written; materialized views are made again or refreshed after.  The
-- branch then folds to the commit's root.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;

CREATE SCHEMA tm VERSIONED;
CREATE TYPE tm.mood AS ENUM ('sad', 'ok');
CREATE DOMAIN tm.pos AS int CHECK (VALUE > 0);
CREATE TYPE tm.pair AS (x int, y int);
CREATE TABLE tm.t (id int PRIMARY KEY, m tm.mood, ms tm.mood[], p tm.pos, pr tm.pair);
INSERT INTO tm.t VALUES (1, 'sad', '{sad,ok}', 1, ROW(1, 2)), (2, 'ok', '{}', 2, ROW(3, 4));
CREATE MATERIALIZED VIEW tm.counts AS SELECT m, count(*) AS n FROM tm.t GROUP BY m;
CREATE MATERIALIZED VIEW tm.ids AS SELECT id FROM tm.t;
SELECT length(dolt_commit('-Am', 'base')::text) AS committed;
SELECT dolt_branch('feature');
SELECT dolt_checkout('feature');

-- An enum gains a label and a row uses it; a domain's check changes; a
-- composite gains an attribute; a materialized view's query changes and
-- another's rows.
ALTER TYPE tm.mood ADD VALUE 'happy';
INSERT INTO tm.t VALUES (3, 'happy', '{happy}', 3, ROW(5, 6));
ALTER DOMAIN tm.pos DROP CONSTRAINT pos_check;
ALTER DOMAIN tm.pos ADD CONSTRAINT pos_check CHECK (VALUE > 0 AND VALUE < 100);
ALTER TYPE tm.pair ADD ATTRIBUTE z int;
DROP MATERIALIZED VIEW tm.counts;
CREATE MATERIALIZED VIEW tm.counts AS SELECT m, count(*) AS n, max(id) AS last FROM tm.t GROUP BY m;
REFRESH MATERIALIZED VIEW tm.ids;
SELECT length(dolt_commit('-Am', 'feature types')::text) AS committed;
SELECT dolt_checkout('main');

-- A fast-forward moves main there.
SELECT (dolt_merge('feature'))[2:4];
SELECT test_fold_root() = prolly_root_hash('feature') AS same_root;
SELECT * FROM tm.t ORDER BY id;
SELECT enum_range(NULL::tm.mood);
SELECT * FROM tm.counts ORDER BY m;
SELECT count(*) FROM tm.ids;
SELECT count(*) AS no_leftovers FROM pg_type WHERE typname LIKE 'prolly_moving_%';

-- And back: a label goes (a row using it goes first), the rest as before.
SELECT dolt_reset('--hard', 'HEAD~1');
SELECT test_fold_root() = prolly_root_hash('HEAD') AS same_root;
SELECT * FROM tm.t ORDER BY id;
SELECT enum_range(NULL::tm.mood);
SELECT * FROM tm.counts ORDER BY m;

SELECT dolt_branch('-D', 'feature');
DROP SCHEMA tm CASCADE;
