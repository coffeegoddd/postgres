-- Merging the rows of a table whose column changed type: the other side's
-- rows, and base's, come to the new type through the column's assignment
-- cast, with its typmod, as ALTER TABLE ... TYPE converted the side's own
-- rows; not through their text forms.  A change one side made merges with
-- the other side's changes only as a string type made no shorter (others
-- are schema conflicts, schema_merges); made alike on both sides, any
-- change merges, base's rows read through the cast.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;

CREATE SCHEMA rt VERSIONED;
CREATE TABLE rt.c (id int PRIMARY KEY, w char(3), s char(5));
INSERT INTO rt.c VALUES (1, 'a', 'a');
SELECT length(dolt_commit('-Am', 'base')::text) AS committed;
SELECT dolt_branch('feature');

-- Ours widens char(3) to char(10) (the same type, another typmod) and makes
-- char(5) varchar(10); theirs adds a row.  Its values are padded, and
-- trimmed, as ALTER did ours'.
ALTER TABLE rt.c ALTER COLUMN w TYPE char(10), ALTER COLUMN s TYPE varchar(10);
SELECT length(dolt_commit('-am', 'main: wider')::text) AS committed;
SELECT dolt_checkout('feature');
INSERT INTO rt.c VALUES (2, 'b', 'b');
SELECT length(dolt_commit('-am', 'feature: a row')::text) AS committed;
SELECT dolt_checkout('main');
SELECT (dolt_merge('feature', '-m', 'merged wider'))[3:4];
SELECT id, w, octet_length(w) AS w_len, s, length(s) AS s_len FROM rt.c ORDER BY id;

-- Theirs widens; ours adds a row.  The same, the other way.
SELECT dolt_checkout('feature');
SELECT dolt_reset('--hard', 'main');
ALTER TABLE rt.c ALTER COLUMN w TYPE char(12);
SELECT length(dolt_commit('-am', 'feature: wider again')::text) AS committed;
SELECT dolt_checkout('main');
INSERT INTO rt.c VALUES (3, 'c', 'c');
SELECT length(dolt_commit('-am', 'main: a row')::text) AS committed;
SELECT (dolt_merge('feature', '-m', 'merged wider again'))[3:4];
SELECT id, w, octet_length(w) AS w_len FROM rt.c ORDER BY id;

-- dolt_diff reads the older rows through the cast too: none differs.
SELECT count(*) AS changed FROM dolt_diff('HEAD~1', 'HEAD', NULL::rt.c)
 WHERE diff_type = 'modified';
SELECT dolt_branch('-D', 'feature');

-- A typmod narrowed on one side, the other adding a row: a schema
-- conflict.  Narrowed alike on both, it merges; base's rows too long for
-- the column, which each side deleted or shortened to narrow it, are read
-- as base had them, neither refused nor cut to fit: deleted on both, the
-- row is gone; shortened on both, differently, it's a conflict (cut to
-- 'abc', ours' would look unchanged, and theirs' taken).
CREATE TABLE rt.n (id int PRIMARY KEY, v varchar(10));
INSERT INTO rt.n VALUES (1, 'a'), (8, 'abcdefghij'), (9, 'abcdefghij');
SELECT length(dolt_commit('-Am', 'n base')::text) AS committed;
SELECT dolt_branch('feature');
DELETE FROM rt.n WHERE id IN (8, 9);
ALTER TABLE rt.n ALTER COLUMN v TYPE varchar(3);
SELECT length(dolt_commit('-am', 'main: narrower')::text) AS committed;
SELECT dolt_checkout('feature');
INSERT INTO rt.n VALUES (2, 'b');
SELECT length(dolt_commit('-am', 'feature: a row')::text) AS committed;
SELECT dolt_checkout('main');
SELECT (dolt_merge('feature', '-m', 'merged narrower'))[3:4];
INSERT INTO rt.n VALUES (8, 'abc');
SELECT length(dolt_commit('-am', 'main: shortened')::text) AS committed;
SELECT dolt_checkout('feature');
DELETE FROM rt.n WHERE id = 9;
UPDATE rt.n SET v = 'xyz' WHERE id = 8;
ALTER TABLE rt.n ALTER COLUMN v TYPE varchar(3);
SELECT length(dolt_commit('-am', 'feature: narrower too')::text) AS committed;
SELECT dolt_checkout('main');
BEGIN;
SELECT (dolt_merge('feature', '-m', 'merged narrower'))[2:4];
SELECT * FROM rt.n ORDER BY id;
SELECT base_row, our_row, their_row FROM dolt_conflicts(NULL::rt.n);
ROLLBACK;
SELECT dolt_branch('-D', 'feature');

-- Another type, made alike on both sides: boolean to text, numeric to
-- integer.  Base's row comes as the cast makes it ('true', not 't', so
-- only theirs changed it; 1.5 rounded, not refused).
CREATE TABLE rt.o (id int PRIMARY KEY, b boolean, n numeric);
INSERT INTO rt.o VALUES (1, true, 1.5);
SELECT length(dolt_commit('-Am', 'o base')::text) AS committed;
SELECT dolt_branch('feature');
ALTER TABLE rt.o ALTER COLUMN b TYPE text, ALTER COLUMN n TYPE int;
INSERT INTO rt.o VALUES (2, 'false', 3);
SELECT length(dolt_commit('-am', 'main: retyped')::text) AS committed;
SELECT dolt_checkout('feature');
ALTER TABLE rt.o ALTER COLUMN b TYPE text, ALTER COLUMN n TYPE int;
UPDATE rt.o SET b = 'yes' WHERE id = 1;
SELECT length(dolt_commit('-am', 'feature: retyped')::text) AS committed;
SELECT dolt_checkout('main');
SELECT (dolt_merge('feature', '-m', 'merged retyped'))[3:4];
SELECT * FROM rt.o ORDER BY id;
SELECT dolt_branch('-D', 'feature');

-- A change only an explicit cast makes (a USING clause), alike on both
-- sides: base's rows come through the explicit cast, as a move's ALTER
-- converts them.
CREATE TABLE rt.u (id int PRIMARY KEY, v text);
INSERT INTO rt.u VALUES (1, ' 1');
SELECT length(dolt_commit('-Am', 'u base')::text) AS committed;
SELECT dolt_branch('feature');
ALTER TABLE rt.u ALTER COLUMN v TYPE int USING v::int;
INSERT INTO rt.u VALUES (2, 2);
SELECT length(dolt_commit('-am', 'main: to int')::text) AS committed;
SELECT dolt_checkout('feature');
ALTER TABLE rt.u ALTER COLUMN v TYPE int USING v::int;
UPDATE rt.u SET v = 5 WHERE id = 1;
SELECT length(dolt_commit('-am', 'feature: to int')::text) AS committed;
SELECT dolt_checkout('main');
SELECT (dolt_merge('feature', '-m', 'merged to int'))[3:4];
SELECT * FROM rt.u ORDER BY id;
SELECT dolt_branch('-D', 'feature');

DROP SCHEMA rt CASCADE;
