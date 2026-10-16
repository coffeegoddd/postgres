-- Checking out a commit, not a branch: the session is detached at it, on
-- a hidden branch there that sessions detached at it share, and every
-- transaction of the session is read-only.  Changing anything takes a
-- branch made from it; checking out a branch ends it.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;

CREATE SCHEMA dc VERSIONED;
CREATE TABLE dc.t (id int PRIMARY KEY, v text);
INSERT INTO dc.t VALUES (1, 'one');
SELECT (dolt_commit('-Am', 'first'))[1] AS c1 \gset
UPDATE dc.t SET v = 'uno' WHERE id = 1;
INSERT INTO dc.t VALUES (2, 'two');
SELECT length(dolt_commit('-am', 'second')::text) AS committed;

SELECT (dolt_checkout(:'c1'))[2] = 'HEAD is now at ' || :'c1' || ' (detached, read-only)' AS detached;
SELECT * FROM dc.t ORDER BY id;
SELECT dolt_hashof('HEAD') = :'c1' AS at_c1;
SELECT name FROM dolt_branches ORDER BY name;

-- Read-only, however asked.
INSERT INTO dc.t VALUES (3, 'three');
BEGIN READ WRITE;
SET default_transaction_read_only = off;
UPDATE dc.t SET v = 'x';
RESET default_transaction_read_only;
SELECT dolt_commit('--allow-empty', '-m', 'no');
CREATE TABLE dc.u (a int);

-- A branch made there is writable.
SELECT (dolt_checkout('-b', 'from_first'))[2];
INSERT INTO dc.t VALUES (3, 'three');
SELECT length(dolt_commit('-am', 'on from_first')::text) AS committed;
SELECT * FROM dc.t ORDER BY id;
-- (one made while detached too, without leaving)
SELECT dolt_checkout(:'c1') IS NOT NULL AS detached_again;
SELECT dolt_branch('also_first');
SELECT dolt_hashof('also_first') = :'c1' AS made_there;
INSERT INTO dc.t VALUES (4, 'four');

-- A branch checked out ends it.
SELECT (dolt_checkout('main'))[2];
INSERT INTO dc.t VALUES (4, 'four');
SELECT * FROM dc.t ORDER BY id;
SELECT dolt_branch('detached/mine');

SELECT dolt_reset('--hard');
SELECT dolt_branch('-D', 'from_first', 'also_first');
DROP SCHEMA dc CASCADE;
