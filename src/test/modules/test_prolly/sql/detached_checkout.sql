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

-- A branch checked out ends it, and the hidden branch, which no session
-- is on any more, is deleted.
SELECT count(*) AS hidden FROM pg_branch WHERE brname LIKE 'detached/%';
SELECT (dolt_checkout('main'))[2];
SELECT count(*) AS hidden FROM pg_branch WHERE brname LIKE 'detached/%';
INSERT INTO dc.t VALUES (4, 'four');
SELECT * FROM dc.t ORDER BY id;
SELECT dolt_branch('detached/mine');

-- Read-only goes with the branch: a checkout of a commit rolled back
-- leaves the session where it was, writable; a switch that fails leaves it
-- detached, read-only; leaving inside a read-only transaction keeps the
-- transaction read-only.
SELECT dolt_reset('--hard');
BEGIN;
SELECT dolt_checkout(:'c1') IS NOT NULL AS detached;
ROLLBACK;
SELECT active_branch();
INSERT INTO dc.t VALUES (5, 'five');
SELECT dolt_checkout(:'c1') IS NOT NULL AS detached;
SELECT active_branch() LIKE 'detached/%' AS on_a_hidden_branch;
SELECT dolt_checkout('-b', 'main');
INSERT INTO dc.t VALUES (6, 'six');
BEGIN READ ONLY;
SELECT (dolt_checkout('main'))[2];
INSERT INTO dc.t VALUES (6, 'six');
ROLLBACK;
INSERT INTO dc.t VALUES (6, 'six');
-- (a transaction on a commit asked to be read-only makes no branch there,
-- nor moves one)
SELECT dolt_hashof('from_first') AS ff \gset
SELECT dolt_checkout(:'c1') IS NOT NULL AS detached;
BEGIN READ ONLY;
SELECT dolt_branch('ro_made');
ROLLBACK;
BEGIN READ ONLY;
SELECT dolt_branch('-f', 'from_first', 'main');
ROLLBACK;
BEGIN;
SET TRANSACTION READ ONLY;
SELECT dolt_branch('-f', 'from_first', 'main');
ROLLBACK;
BEGIN;
SET LOCAL transaction_read_only = on;
SELECT dolt_branch('-f', 'from_first', 'main');
ROLLBACK;
-- (nor one chained to it, which is read-only as asked too)
BEGIN READ ONLY;
COMMIT AND CHAIN;
SHOW transaction_read_only;
SELECT dolt_branch('-f', 'from_first', 'main');
ROLLBACK;
BEGIN READ ONLY;
ROLLBACK AND CHAIN;
SELECT dolt_branch('ro_made');
ROLLBACK;
-- (one chained to a transaction read-only only for the commit makes one)
BEGIN;
COMMIT AND CHAIN;
SELECT dolt_branch('ro_chained');
COMMIT;
SELECT dolt_hashof('from_first') = :'ff' AS not_moved;
SELECT dolt_branch('ro_made');
SELECT dolt_hashof('ro_made') = :'c1' AS made_there;
SELECT (dolt_checkout('main'))[2];
SELECT dolt_branch('-D', 'ro_made', 'ro_chained');
-- (a transaction chained to one that checked out a commit is read-only
-- for it, as one begun there is)
BEGIN;
SELECT dolt_checkout(:'c1') IS NOT NULL AS detached;
COMMIT AND CHAIN;
SHOW transaction_read_only;
INSERT INTO dc.t VALUES (7, 'seven');
ROLLBACK;
SELECT (dolt_checkout('main'))[2];
-- (a hidden branch isn't checked out by name, nor HEAD as a commit)
SELECT dolt_checkout('detached/' || substr(:'c1', 1, 40));
SELECT dolt_checkout('HEAD');
-- (and one nobody is on any more goes, with the next checkout that keeps)
SELECT (dolt_checkout('main'))[2];
SELECT count(*) AS hidden_left FROM pg_branch WHERE brname LIKE 'detached/%';

SELECT dolt_reset('--hard');
SELECT dolt_branch('-D', 'from_first', 'also_first');
DROP SCHEMA dc CASCADE;
