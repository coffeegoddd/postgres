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

-- popped over uncommitted changes: refused where they change a table it
-- changes (as Dolt's, by table), applied where they change another
INSERT INTO st.t VALUES (6, 'f');
SELECT dolt_stash('push', 's');
INSERT INTO st.t VALUES (7, 'g');
SELECT dolt_stash('pop', 's');
SELECT * FROM st.t ORDER BY pk;
DELETE FROM st.t WHERE pk = 7;
INSERT INTO st.o VALUES (1);
SELECT dolt_stash('pop', 's');
SELECT * FROM st.t ORDER BY pk;
SELECT * FROM st.o;
SELECT dolt_reset('--hard');

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

\c contrib_regression
DROP DATABASE regress_stash;
