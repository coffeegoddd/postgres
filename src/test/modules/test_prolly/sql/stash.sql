-- dolt_stash sets the working set aside (a commit of it whose parent is
-- the head) and brings it back, three-way, as a cherry-pick without a
-- commit; dolt_stashes lists them.  Every change is staged (there is no
-- staging area), so new tables are stashed too.
CREATE DATABASE regress_stash;
\c regress_stash
CREATE SCHEMA st VERSIONED;
CREATE TABLE st.t (pk int PRIMARY KEY, v text);
CREATE TABLE st.o (id int PRIMARY KEY);
INSERT INTO st.t VALUES (1, 'a'), (2, 'b');
SELECT length(dolt_commit('-Am', 'two rows')::text) AS committed;

-- nothing to stash, nothing to pop
SELECT dolt_stash('push', 's');
SELECT dolt_stash('pop', 's');
SELECT dolt_stash('drop', 's');
SELECT dolt_stash('clear', 's');
SELECT dolt_stash('push');
SELECT dolt_stash('shove', 's');

-- push and pop: the working set exactly as it was
INSERT INTO st.t VALUES (3, 'c');
UPDATE st.t SET v = 'z' WHERE pk = 1;
SELECT table_name || ' ' || status AS before FROM dolt_status \gset
SELECT dolt_stash('push', 's');
SELECT count(*) AS changed FROM dolt_status;
SELECT * FROM st.t ORDER BY pk;
SELECT name, stash_id, branch, hash = dolt_hashof('HEAD') AS on_head, commit_message
FROM dolt_stashes;
SELECT dolt_stash('pop', 's');
SELECT table_name || ' ' || status = :'before' AS same_status FROM dolt_status;
SELECT * FROM st.t ORDER BY pk;
SELECT count(*) AS stashes FROM dolt_stashes;

-- several, in named lists, newest first; a specific one popped; apply
-- keeps it; drop and clear
SELECT dolt_stash('push', 's');
INSERT INTO st.t VALUES (4, 'd');
SELECT dolt_stash('push', 's');
INSERT INTO st.t VALUES (5, 'e');
SELECT dolt_stash('push', 'other');
SELECT name, stash_id FROM dolt_stashes;
SELECT dolt_stash('apply', 's', 'stash@{1}');
SELECT * FROM st.t ORDER BY pk;
SELECT count(*) AS stashes FROM dolt_stashes;
SELECT dolt_reset('--hard');
SELECT dolt_stash('drop', 's', 'stash@{1}');
SELECT dolt_stash('drop', 's', 'stash@{5}');
SELECT dolt_stash('drop', 's', 'stash@x');
SELECT name, stash_id FROM dolt_stashes;
SELECT dolt_stash('clear', 'other');
SELECT name, stash_id FROM dolt_stashes;

-- popped onto a head that moved: its changes, three-way
UPDATE st.t SET v = 'b2' WHERE pk = 2;
SELECT length(dolt_commit('-am', 'b2')::text) AS committed;
SELECT dolt_stash('pop', 's');
SELECT * FROM st.t ORDER BY pk;
SELECT dolt_reset('--hard');

-- popped over uncommitted changes: merged into them, as Dolt's, the same
-- table's other rows too (#585); refused, kept, where a row both changed
-- conflicts
INSERT INTO st.t VALUES (6, 'f');
SELECT dolt_stash('push', 's');
INSERT INTO st.t VALUES (7, 'g');
INSERT INTO st.o VALUES (1);
SELECT dolt_stash('pop', 's');
SELECT * FROM st.t ORDER BY pk;
SELECT * FROM st.o;
SELECT count(*) AS stashes FROM dolt_stashes;
SELECT dolt_reset('--hard');
INSERT INTO st.t VALUES (6, 'f');
UPDATE st.t SET v = 'a2' WHERE pk = 1;
SELECT dolt_stash('push', 's');
INSERT INTO st.t VALUES (6, 'other');
SELECT dolt_stash('pop', 's');
SELECT count(*) AS stashes FROM dolt_stashes;
SELECT * FROM st.t ORDER BY pk;
UPDATE st.t SET v = 'f' WHERE pk = 6;
SELECT dolt_stash('apply', 's');
SELECT * FROM st.t ORDER BY pk;
SELECT count(*) AS stashes FROM dolt_stashes;
SELECT dolt_stash('drop', 's');
SELECT dolt_reset('--hard');

-- popped over a table the working set made: it stays, with its rows
INSERT INTO st.t VALUES (6, 'f');
SELECT dolt_stash('push', 's');
CREATE TABLE st.w (id int PRIMARY KEY);
INSERT INTO st.w VALUES (1);
INSERT INTO st.t VALUES (7, 'g');
SELECT dolt_stash('pop', 's');
SELECT * FROM st.t ORDER BY pk;
SELECT * FROM st.w;
DROP TABLE st.w;
SELECT dolt_reset('--hard');

-- specific stashes popped one after another, each into what the one
-- before left (Dolt's "Popping specific stashes")
INSERT INTO st.t VALUES (11, 'a');
SELECT dolt_stash('push', 's');
INSERT INTO st.t VALUES (12, 'b');
SELECT dolt_stash('push', 's');
INSERT INTO st.t VALUES (13, 'c');
SELECT dolt_stash('push', 's');
INSERT INTO st.t VALUES (14, 'd');
SELECT dolt_stash('push', 's');
SELECT dolt_stash('pop', 's', 'stash@{3}');
SELECT dolt_stash('pop', 's', 'stash@{1}');
SELECT * FROM st.t WHERE pk > 10 ORDER BY pk;
SELECT stash_id FROM dolt_stashes;
SELECT dolt_stash('clear', 's');
SELECT dolt_reset('--hard');

-- popped over uncommitted changes to more than rows, each merged rather
-- than refused as a local change it would stomp
CREATE SEQUENCE st.q;
CREATE TABLE st.d (id int PRIMARY KEY);
CREATE TABLE st.a (id int PRIMARY KEY);
CREATE TABLE st.m (id int PRIMARY KEY);
CREATE MATERIALIZED VIEW st.mv AS SELECT coalesce(sum(id), 0) AS c FROM st.m;
SELECT length(dolt_commit('-Am', 'more kinds')::text) AS committed;
-- a sequence both advanced: the larger value
SELECT nextval('st.q'), nextval('st.q'), nextval('st.q');
SELECT dolt_stash('push', 's');
SELECT nextval('st.q');
SELECT dolt_stash('pop', 's');
SELECT last_value FROM st.q;
SELECT count(*) AS stashes FROM dolt_stashes;
SELECT dolt_reset('--hard');
-- a materialized view both refreshed: refreshed from the merged rows
INSERT INTO st.m VALUES (1);
REFRESH MATERIALIZED VIEW st.mv;
SELECT dolt_stash('push', 's');
INSERT INTO st.m VALUES (10);
REFRESH MATERIALIZED VIEW st.mv;
SELECT dolt_stash('pop', 's');
SELECT c FROM st.mv;
SELECT count(*) AS stashes FROM dolt_stashes;
SELECT dolt_reset('--hard');
-- a table the working set altered whose rows the stash changes
INSERT INTO st.a VALUES (1);
SELECT dolt_stash('push', 's');
ALTER TABLE st.a ADD COLUMN x int;
INSERT INTO st.a VALUES (2, 2);
SELECT dolt_stash('pop', 's');
SELECT * FROM st.a ORDER BY id;
SELECT count(*) AS stashes FROM dolt_stashes;
SELECT dolt_reset('--hard');
-- a table the working set dropped whose rows the stash changes: refused,
-- the stash kept
INSERT INTO st.d VALUES (1);
SELECT dolt_stash('push', 's');
DROP TABLE st.d;
SELECT dolt_stash('pop', 's');
SELECT count(*) AS stashes FROM dolt_stashes;
SELECT dolt_reset('--hard');
SELECT dolt_stash('pop', 's');
SELECT * FROM st.d;
DROP MATERIALIZED VIEW st.mv;
DROP TABLE st.m, st.a, st.d;
DROP SEQUENCE st.q;
SELECT length(dolt_commit('-Am', 'fewer kinds')::text) AS committed;

-- popped onto a head whose change conflicts: refused, kept
INSERT INTO st.t VALUES (8, 'i');
SELECT dolt_stash('push', 's');
INSERT INTO st.t VALUES (8, 'j');
SELECT length(dolt_commit('-am', 'eight')::text) AS committed;
SELECT dolt_stash('pop', 's');
SELECT count(*) AS stashes FROM dolt_stashes;
SELECT dolt_stash('clear', 's');

-- a new table and a column added: stashed (with -u, as without), and back
CREATE TABLE st.n (id int PRIMARY KEY);
INSERT INTO st.n VALUES (1);
ALTER TABLE st.t ADD COLUMN w int;
UPDATE st.t SET w = pk;
SELECT dolt_stash('push', 's', '-u');
SELECT to_regclass('st.n') IS NULL AS new_table_gone;
SELECT count(*) AS columns FROM information_schema.columns
WHERE table_schema = 'st' AND table_name = 't';
SELECT dolt_gc();
SELECT dolt_stash('pop', 's');
SELECT * FROM st.n;
SELECT pk, w FROM st.t ORDER BY pk;

-- in a transaction block, as anything else: a rollback undoes it
SELECT length(dolt_commit('-Am', 'all in')::text) AS committed;
INSERT INTO st.t VALUES (9, 'k', 9);
BEGIN;
SELECT dolt_stash('push', 's');
ROLLBACK;
SELECT count(*) AS stashes FROM dolt_stashes;
SELECT pk FROM st.t WHERE pk = 9;

-- on a protected branch, an apply or pop, which commits nothing, is
-- refused as a write is (the stash kept)
SELECT dolt_stash('push', 's');
SELECT dolt_branch('--protect', 'main');
UPDATE st.t SET v = 'protected' WHERE pk = 1;
SELECT dolt_stash('apply', 's');
SELECT dolt_stash('pop', 's');
SELECT count(*) AS changed FROM dolt_status;
SELECT count(*) AS stashes FROM dolt_stashes;
SELECT dolt_branch('--unprotect', 'main');
SELECT dolt_stash('pop', 's');
SELECT pk FROM st.t WHERE pk = 9;

\c contrib_regression
DROP DATABASE regress_stash;
