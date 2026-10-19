-- Merging the rows of a table whose column changed type: the other side's
-- rows come to the new type through the column's assignment cast, with its
-- typmod, as ALTER TABLE ... TYPE converted the side's own rows; not
-- through their text forms.  A change one side made merges with the other
-- side's changes only as a string type made no shorter (others are schema
-- conflicts, schema_merges); made alike on both sides, any change merges.
-- Base's rows, only compared, are cast only where nothing is lost (the
-- value comes back from casting it back); a value that doesn't is kept as
-- base had it where its type stores alike, else taken as changed by both
-- sides (a conflict, unless they agree).  A diff shows older values so too.

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
-- integer.  Base is read as master read it, its text form: true is 't'
-- (which a USING may have meant, not the cast's 'true'), so theirs' 'yes'
-- against ours' 'true' is a conflict, as on master.  1.5 isn't an
-- integer's text: unsure (shown null), not refused, and as both sides
-- have 2, they agree.
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
BEGIN;
SELECT (dolt_merge('feature', '-m', 'merged retyped'))[2:4];
SELECT * FROM rt.o ORDER BY id;
SELECT base_row, our_row, their_row FROM dolt_conflicts(NULL::rt.o);
ROLLBACK;
SELECT dolt_branch('-D', 'feature');

-- A change only an explicit cast makes (a USING clause), alike on both
-- sides.  Base's values are compared through the explicit cast where it
-- gives what master compared, their text form read as integers: '1' and
-- ' 3' read as 1 and 3, so theirs' 5 and 7 are the only changes.
CREATE TABLE rt.u (id int PRIMARY KEY, v text);
INSERT INTO rt.u VALUES (1, '1'), (3, ' 3');
SELECT length(dolt_commit('-Am', 'u base')::text) AS committed;
SELECT dolt_branch('feature');
ALTER TABLE rt.u ALTER COLUMN v TYPE int USING v::int;
INSERT INTO rt.u VALUES (2, 2);
SELECT length(dolt_commit('-am', 'main: to int')::text) AS committed;
SELECT dolt_checkout('feature');
ALTER TABLE rt.u ALTER COLUMN v TYPE int USING v::int;
UPDATE rt.u SET v = 5 WHERE id = 1;
UPDATE rt.u SET v = 7 WHERE id = 3;
SELECT length(dolt_commit('-am', 'feature: to int')::text) AS committed;
SELECT dolt_checkout('main');
BEGIN;
SELECT (dolt_merge('feature', '-m', 'merged to int'))[2:4];
SELECT * FROM rt.u ORDER BY id;
SELECT base_row, our_row, their_row FROM dolt_conflicts(NULL::rt.u);
ROLLBACK;
SELECT dolt_branch('-D', 'feature');

-- A USING that isn't the cast, alike on both sides: base's 0.02 casts to
-- 0, theirs' new value, but casting 0 back doesn't give 0.02, so theirs'
-- change isn't taken for no change (ours' 2 winning): it's a conflict.
CREATE TABLE rt.x (id int PRIMARY KEY, n numeric);
INSERT INTO rt.x VALUES (1, 0.02), (2, 0.05);
SELECT length(dolt_commit('-Am', 'x base')::text) AS committed;
SELECT dolt_branch('feature');
ALTER TABLE rt.x ALTER COLUMN n TYPE int USING (n * 100)::int;
INSERT INTO rt.x VALUES (3, 9);
SELECT length(dolt_commit('-am', 'main: cents')::text) AS committed;
SELECT dolt_checkout('feature');
ALTER TABLE rt.x ALTER COLUMN n TYPE int USING (n * 100)::int;
UPDATE rt.x SET n = 0 WHERE id = 1;
SELECT length(dolt_commit('-am', 'feature: cents')::text) AS committed;
SELECT dolt_checkout('main');
BEGIN;
SELECT (dolt_merge('feature', '-m', 'merged cents'))[2:4];
SELECT * FROM rt.x ORDER BY id;
SELECT our_row, their_row FROM dolt_conflicts(NULL::rt.x);
ROLLBACK;
SELECT dolt_branch('-D', 'feature');

-- A key's typmod narrowed alike on both sides, the cast rounding base's
-- 1.25 and 1.26 both to 1.3 (each side deleted 1.26 to narrow it): base's
-- keys that don't come back from the cast are kept as base had them, keys
-- neither side has, not merged into one.  2.50 does come back: theirs'
-- change to its row is taken.
CREATE TABLE rt.k (id numeric(5,2) PRIMARY KEY, v text);
INSERT INTO rt.k VALUES (1.25, 'a'), (1.26, 'b'), (2.50, 'c');
SELECT length(dolt_commit('-Am', 'k base')::text) AS committed;
SELECT dolt_branch('feature');
DELETE FROM rt.k WHERE id = 1.26;
ALTER TABLE rt.k ALTER COLUMN id TYPE numeric(5,1);
SELECT length(dolt_commit('-am', 'main: coarser')::text) AS committed;
SELECT dolt_checkout('feature');
DELETE FROM rt.k WHERE id = 1.26;
ALTER TABLE rt.k ALTER COLUMN id TYPE numeric(5,1);
INSERT INTO rt.k VALUES (7, 'x');
UPDATE rt.k SET v = 'C' WHERE id = 2.5;
SELECT length(dolt_commit('-am', 'feature: coarser')::text) AS committed;
SELECT dolt_checkout('main');
SELECT (dolt_merge('feature', '-m', 'merged coarser'))[3:4];
SELECT * FROM rt.k ORDER BY id;
SELECT dolt_branch('-D', 'feature');

-- A narrowing alike on both sides from text, another type: base's row too
-- long for varchar(3) doesn't read as one (master failed there), so it's
-- unsure, not refused; both sides deleted it, so it's gone.
CREATE TABLE rt.t (id int PRIMARY KEY, v text);
INSERT INTO rt.t VALUES (1, 'a'), (9, 'abcdefghij');
SELECT length(dolt_commit('-Am', 't base')::text) AS committed;
SELECT dolt_branch('feature');
DELETE FROM rt.t WHERE id = 9;
ALTER TABLE rt.t ALTER COLUMN v TYPE varchar(3);
SELECT length(dolt_commit('-am', 'main: varchar')::text) AS committed;
SELECT dolt_checkout('feature');
DELETE FROM rt.t WHERE id = 9;
ALTER TABLE rt.t ALTER COLUMN v TYPE varchar(3);
INSERT INTO rt.t VALUES (2, 'b');
SELECT length(dolt_commit('-am', 'feature: varchar')::text) AS committed;
SELECT dolt_checkout('main');
SELECT (dolt_merge('feature', '-m', 'merged varchar'))[3:4];
SELECT * FROM rt.t ORDER BY id;
SELECT dolt_branch('-D', 'feature');

-- dolt_diff across a narrowing shows older values as they were stored:
-- 'abcdefghij' updated to 'abc' is a change, not cut to 'abc' and hidden;
-- keys 'abcd1' and 'abcd2' replaced by 'abcd' are removed as themselves.
CREATE TABLE rt.d (id int PRIMARY KEY, v varchar(10));
CREATE TABLE rt.dk (k varchar(10) PRIMARY KEY, n int);
INSERT INTO rt.d VALUES (1, 'abcdefghij'), (2, 'x');
INSERT INTO rt.dk VALUES ('abcd1', 1), ('abcd2', 2);
SELECT length(dolt_commit('-Am', 'd base')::text) AS committed;
UPDATE rt.d SET v = 'abc' WHERE id = 1;
ALTER TABLE rt.d ALTER COLUMN v TYPE varchar(3);
DELETE FROM rt.dk;
INSERT INTO rt.dk VALUES ('abcd', 3);
ALTER TABLE rt.dk ALTER COLUMN k TYPE varchar(4);
SELECT length(dolt_commit('-am', 'd narrower')::text) AS committed;
SELECT diff_type, from_row, to_row FROM dolt_diff('HEAD~1', 'HEAD', NULL::rt.d) ORDER BY 1;
SELECT diff_type, from_row, to_row FROM dolt_diff('HEAD~1', 'HEAD', NULL::rt.dk) ORDER BY 1, 2;

-- Conflicts listed and resolved after the table changed definition since
-- the merge: read as a diff reads them, not refused as the merge would.
CREATE TABLE rt.r (id int PRIMARY KEY, v varchar(10));
INSERT INTO rt.r VALUES (1, 'base'), (2, 'abcdefghij');
SELECT length(dolt_commit('-Am', 'r base')::text) AS committed;
SELECT dolt_branch('feature');
UPDATE rt.r SET v = 'ours' WHERE id = 1;
SELECT length(dolt_commit('-am', 'main: ours')::text) AS committed;
SELECT dolt_checkout('feature');
UPDATE rt.r SET v = 'theirs' WHERE id = 1;
SELECT length(dolt_commit('-am', 'feature: theirs')::text) AS committed;
SELECT dolt_checkout('main');
-- (a column with a value each row was given, added mid-resolution: null)
BEGIN;
SELECT (dolt_merge('feature', '-m', 'merged r'))[2:4];
ALTER TABLE rt.r ADD COLUMN z serial;
SELECT base_row, our_row, their_row FROM dolt_conflicts(NULL::rt.r);
ROLLBACK;
-- (narrowed mid-resolution, ours' long row shortened first)
BEGIN;
SELECT (dolt_merge('feature', '-m', 'merged r'))[2:4];
UPDATE rt.r SET v = 'abc' WHERE id = 2;
ALTER TABLE rt.r ALTER COLUMN v TYPE varchar(6);
SELECT base_row, our_row, their_row FROM dolt_conflicts(NULL::rt.r);
SELECT dolt_conflicts_resolve('--theirs', 'rt.r');
SELECT * FROM rt.r ORDER BY id;
ROLLBACK;
-- (narrowed so that theirs' row, taken, doesn't fit: refused, not written)
BEGIN;
SELECT (dolt_merge('feature', '-m', 'merged r'))[2:4];
UPDATE rt.r SET v = 'abc' WHERE id = 2;
UPDATE rt.r SET v = 'o' WHERE id = 1;
ALTER TABLE rt.r ALTER COLUMN v TYPE varchar(4);
SELECT dolt_conflicts_resolve('--theirs', 'rt.r');
ROLLBACK;
SELECT dolt_branch('-D', 'feature');

-- (finding 3 without a key: a keyless row can't be taken as changed on
-- both sides, so a base value not trusted refuses the merge, as master's
-- text conversion of 0.02 to integer did)
CREATE TABLE rt.xk (id int, n numeric);
INSERT INTO rt.xk VALUES (1, 0.02), (2, 0.05);
SELECT length(dolt_commit('-Am', 'xk base')::text) AS committed;
SELECT dolt_branch('feature');
ALTER TABLE rt.xk ALTER COLUMN n TYPE int USING (n * 100)::int;
INSERT INTO rt.xk VALUES (3, 9);
SELECT length(dolt_commit('-am', 'main: cents')::text) AS committed;
SELECT dolt_checkout('feature');
ALTER TABLE rt.xk ALTER COLUMN n TYPE int USING (n * 100)::int;
UPDATE rt.xk SET n = 0 WHERE id = 1;
SELECT length(dolt_commit('-am', 'feature: cents')::text) AS committed;
SELECT dolt_checkout('main');
SELECT (dolt_merge('feature', '-m', 'merged cents'))[3:4];
SELECT * FROM rt.xk ORDER BY id;
SELECT dolt_branch('-D', 'feature');

-- A USING whose values the cast round-trips by chance: an inverted flag
-- made an integer.  Base's true casts to 1 and back, but its text form
-- 't' isn't an integer, so the cast isn't trusted: theirs' 1 against
-- ours' 0 is a conflict, not taken for no change.
CREATE TABLE rt.f (id int PRIMARY KEY, disabled boolean);
INSERT INTO rt.f VALUES (1, true), (2, false);
SELECT length(dolt_commit('-Am', 'f base')::text) AS committed;
SELECT dolt_branch('feature');
ALTER TABLE rt.f ALTER COLUMN disabled TYPE int USING (NOT disabled)::int;
INSERT INTO rt.f VALUES (3, 1);
SELECT length(dolt_commit('-am', 'main: enabled')::text) AS committed;
SELECT dolt_checkout('feature');
ALTER TABLE rt.f ALTER COLUMN disabled TYPE int USING (NOT disabled)::int;
UPDATE rt.f SET disabled = 1 WHERE id = 1;
SELECT length(dolt_commit('-am', 'feature: enabled')::text) AS committed;
SELECT dolt_checkout('main');
BEGIN;
SELECT (dolt_merge('feature', '-m', 'merged enabled'))[2:4];
SELECT * FROM rt.f ORDER BY id;
SELECT our_row, their_row FROM dolt_conflicts(NULL::rt.f);
ROLLBACK;
SELECT dolt_branch('-D', 'feature');

-- Into char(n), text's trailing blanks are char's padding: 'ab ' is
-- 'ab   ' as ALTER made it, not a change.
CREATE TABLE rt.s (id int PRIMARY KEY, v text);
INSERT INTO rt.s VALUES (1, 'ab ');
SELECT length(dolt_commit('-Am', 's base')::text) AS committed;
ALTER TABLE rt.s ALTER COLUMN v TYPE char(5);
SELECT length(dolt_commit('-am', 's char')::text) AS committed;
SELECT diff_type, from_row, to_row FROM dolt_diff('HEAD~1', 'HEAD', NULL::rt.s);

-- A column retyped mid-resolution isn't one the merge compared across:
-- base's 1.5 isn't unsure there, so no conflict the merge didn't find is
-- listed, and --theirs leaves ours' change (2.5, now 3) as the merge did.
CREATE TABLE rt.m (id int PRIMARY KEY, v text, n numeric);
INSERT INTO rt.m VALUES (1, 'base', 1), (5, 'x', 1.5);
SELECT length(dolt_commit('-Am', 'm base')::text) AS committed;
SELECT dolt_branch('feature');
UPDATE rt.m SET v = 'ours' WHERE id = 1;
UPDATE rt.m SET n = 2.5 WHERE id = 5;
SELECT length(dolt_commit('-am', 'main: ours')::text) AS committed;
SELECT dolt_checkout('feature');
UPDATE rt.m SET v = 'theirs' WHERE id = 1;
SELECT length(dolt_commit('-am', 'feature: theirs')::text) AS committed;
SELECT dolt_checkout('main');
BEGIN;
SELECT (dolt_merge('feature', '-m', 'merged m'))[2:4];
ALTER TABLE rt.m ALTER COLUMN n TYPE int;
SELECT base_row, our_row, their_row FROM dolt_conflicts(NULL::rt.m);
SELECT dolt_conflicts_resolve('--theirs', 'rt.m');
SELECT * FROM rt.m ORDER BY id;
ROLLBACK;
SELECT dolt_branch('-D', 'feature');

-- Retyped alike on both sides, base is compared as master compared it:
-- within one type, the cast trusted only where it gives base's stored
-- value; across types (strings too), base's text form.  A USING that pads
-- or folds otherwise than the cast can't then hide theirs' edit back to
-- the cast's value: code and tags conflict, and name, whose rpad ALTER
-- made base's text form 'gh   ', takes theirs' 'gh'.  (A plain char(3)
-- to char(10) on both sides is a conflict on a row a side edited too, as
-- on master: padding changes base's stored value.)
CREATE TABLE rt.up (id int PRIMARY KEY, code char(3), tags char(2)[], name char(5));
INSERT INTO rt.up VALUES (1, 'ab', '{ab}', 'ab'), (2, 'cd', '{ef}', 'cd'),
  (3, 'gh', '{gh}', 'gh');
SELECT length(dolt_commit('-Am', 'up base')::text) AS committed;
SELECT dolt_branch('feature');
ALTER TABLE rt.up ALTER COLUMN code TYPE char(10) USING upper(code),
  ALTER COLUMN tags TYPE char(5)[] USING upper(tags::text)::char(5)[],
  ALTER COLUMN name TYPE text USING rpad(name, 5);
INSERT INTO rt.up VALUES (4, 'x', '{x}', 'x');
SELECT length(dolt_commit('-am', 'main: upper')::text) AS committed;
SELECT dolt_checkout('feature');
ALTER TABLE rt.up ALTER COLUMN code TYPE char(10) USING upper(code),
  ALTER COLUMN tags TYPE char(5)[] USING upper(tags::text)::char(5)[],
  ALTER COLUMN name TYPE text USING rpad(name, 5);
UPDATE rt.up SET code = 'ab' WHERE id = 1;
UPDATE rt.up SET tags = '{ef}' WHERE id = 2;
UPDATE rt.up SET name = 'gh' WHERE id = 3;
SELECT length(dolt_commit('-am', 'feature: upper')::text) AS committed;
SELECT dolt_checkout('main');
BEGIN;
SELECT (dolt_merge('feature', '-m', 'merged upper'))[2:4];
SELECT our_row, their_row FROM dolt_conflicts(NULL::rt.up) ORDER BY 1;
SELECT id, '"' || name || '"' AS name FROM rt.up ORDER BY id;
ROLLBACK;
SELECT dolt_branch('-D', 'feature');

-- Where the cast and the text form differ but the text form reads as the
-- new type, base is read as its text form, as master read it, not
-- refused: char(5) key 'cd   ' as text is 'cd   ', a key neither side
-- has (they trimmed it), so base's rows read as deleted on both sides and
-- theirs' edited row against ours' is a conflict; keyless, base's rows
-- are copies neither side has, and both sides' inserts merge.
CREATE TABLE rt.ck (k char(5) PRIMARY KEY, v int);
CREATE TABLE rt.cl (c char(5), v int);
CREATE TABLE rt.bl (f boolean, v int);
INSERT INTO rt.ck VALUES ('ab', 1), ('cd', 2);
INSERT INTO rt.cl VALUES ('ab', 1), ('cd', 2);
INSERT INTO rt.bl VALUES (true, 1), (false, 2);
SELECT length(dolt_commit('-Am', 'ck base')::text) AS committed;
SELECT dolt_branch('feature');
ALTER TABLE rt.ck ALTER COLUMN k TYPE text;
ALTER TABLE rt.cl ALTER COLUMN c TYPE varchar(10);
ALTER TABLE rt.bl ALTER COLUMN f TYPE text;
INSERT INTO rt.ck VALUES ('zz', 9);
INSERT INTO rt.cl VALUES ('zz', 9);
INSERT INTO rt.bl VALUES ('zz', 9);
SELECT length(dolt_commit('-am', 'main: retyped')::text) AS committed;
SELECT dolt_checkout('feature');
ALTER TABLE rt.ck ALTER COLUMN k TYPE text;
ALTER TABLE rt.cl ALTER COLUMN c TYPE varchar(10);
ALTER TABLE rt.bl ALTER COLUMN f TYPE text;
UPDATE rt.ck SET v = 20 WHERE k = 'cd';
INSERT INTO rt.cl VALUES ('yy', 8);
INSERT INTO rt.bl VALUES ('yy', 8);
SELECT length(dolt_commit('-am', 'feature: retyped')::text) AS committed;
SELECT dolt_checkout('main');
BEGIN;
SELECT (dolt_merge('feature', '-m', 'merged retyped keys'))[2:4];
SELECT '"' || k || '"' AS k, v FROM rt.ck ORDER BY 1;
SELECT base_row, our_row, their_row FROM dolt_conflicts(NULL::rt.ck);
SELECT '"' || c || '"' AS c, v FROM rt.cl ORDER BY 1;
SELECT f, v FROM rt.bl ORDER BY 1;
ROLLBACK;
SELECT dolt_branch('-D', 'feature');

-- A domain's constraints don't apply to historical values only compared
-- or shown.  Both sides retype to a checked domain after fixing or deleting
-- the old values.  Keep those values for comparison and conflict listing.
CREATE DOMAIN rt.positive AS int CHECK (VALUE > 0);
CREATE TABLE rt.domain_rows (id int PRIMARY KEY, v int);
CREATE TABLE rt.domain_keys (id int PRIMARY KEY, v int);
CREATE TABLE rt.domain_keyless (v int);
INSERT INTO rt.domain_rows VALUES (1, -1), (2, -2), (3, -3);
INSERT INTO rt.domain_keys VALUES (-1, 1), (1, 1);
INSERT INTO rt.domain_keyless VALUES (-1), (1);
SELECT length(dolt_commit('-Am', 'domain base')::text) AS committed;
SELECT dolt_branch('feature');
DELETE FROM rt.domain_rows WHERE id = 3;
UPDATE rt.domain_rows SET v = id;
DELETE FROM rt.domain_keys WHERE id = -1;
DELETE FROM rt.domain_keyless WHERE v = -1;
ALTER TABLE rt.domain_rows ALTER COLUMN v TYPE rt.positive;
ALTER TABLE rt.domain_keys ALTER COLUMN id TYPE rt.positive;
ALTER TABLE rt.domain_keyless ALTER COLUMN v TYPE rt.positive;
INSERT INTO rt.domain_keys VALUES (2, 2);
INSERT INTO rt.domain_keyless VALUES (2);
SELECT length(dolt_commit('-am', 'main: positive')::text) AS committed;
SELECT diff_type, from_row, to_row
FROM dolt_diff('HEAD~1', 'HEAD', NULL::rt.domain_rows) ORDER BY from_row;
SELECT dolt_checkout('feature');
DELETE FROM rt.domain_rows WHERE id = 3;
UPDATE rt.domain_rows SET v = CASE id WHEN 1 THEN 3 ELSE id END;
DELETE FROM rt.domain_keys WHERE id = -1;
DELETE FROM rt.domain_keyless WHERE v = -1;
ALTER TABLE rt.domain_rows ALTER COLUMN v TYPE rt.positive;
ALTER TABLE rt.domain_keys ALTER COLUMN id TYPE rt.positive;
ALTER TABLE rt.domain_keyless ALTER COLUMN v TYPE rt.positive;
INSERT INTO rt.domain_keys VALUES (3, 3);
INSERT INTO rt.domain_keyless VALUES (3);
SELECT length(dolt_commit('-am', 'feature: positive')::text) AS committed;
SELECT dolt_checkout('main');
BEGIN;
SELECT (dolt_merge('feature'))[2:4];
SELECT base_row, our_row, their_row FROM dolt_conflicts(NULL::rt.domain_rows);
SELECT * FROM rt.domain_rows ORDER BY id;
SELECT * FROM rt.domain_keys ORDER BY id;
SELECT * FROM rt.domain_keyless ORDER BY v;
SELECT dolt_conflicts_resolve('--theirs', 'rt.domain_rows');
SELECT * FROM rt.domain_rows ORDER BY id;
ROLLBACK;
SELECT dolt_branch('-D', 'feature');

-- Values already stored when a NOT VALID constraint was added remain
-- comparable too.  The unchanged negative value mustn't hide the independent
-- edits to other columns, or create a spurious conflict.
CREATE DOMAIN rt.pending AS int;
CREATE TABLE rt.domain_pending (id int PRIMARY KEY, v int, a int, b int);
INSERT INTO rt.domain_pending VALUES (1, -1, 0, 0);
SELECT length(dolt_commit('-Am', 'pending base')::text) AS committed;
SELECT dolt_branch('feature');
ALTER TABLE rt.domain_pending ALTER COLUMN v TYPE rt.pending;
ALTER DOMAIN rt.pending ADD CONSTRAINT positive CHECK (VALUE > 0) NOT VALID;
UPDATE rt.domain_pending SET a = 1;
SELECT length(dolt_commit('-am', 'main: pending')::text) AS committed;
SELECT dolt_checkout('feature');
ALTER TABLE rt.domain_pending ALTER COLUMN v TYPE rt.pending;
ALTER DOMAIN rt.pending ADD CONSTRAINT positive CHECK (VALUE > 0) NOT VALID;
UPDATE rt.domain_pending SET b = 2;
SELECT length(dolt_commit('-am', 'feature: pending')::text) AS committed;
SELECT dolt_checkout('main');
BEGIN;
SELECT (dolt_merge('feature'))[2:4];
SELECT * FROM rt.domain_pending;
SELECT count(*) AS conflicts FROM dolt_conflicts(NULL::rt.domain_pending);
ROLLBACK;
SELECT dolt_branch('-D', 'feature');

-- Only one side retypes, after fixing a bad base value; the other changes
-- another column.  The schema merger refuses this before comparing rows:
-- only widening string changes merge with the other side's row edits.
CREATE TABLE rt.domain_one_side (id int PRIMARY KEY, v int, w int);
INSERT INTO rt.domain_one_side VALUES (1, -1, 0);
SELECT length(dolt_commit('-Am', 'one-sided domain base')::text) AS committed;
SELECT dolt_branch('feature');
UPDATE rt.domain_one_side SET v = 1;
ALTER TABLE rt.domain_one_side ALTER COLUMN v TYPE rt.positive;
SELECT length(dolt_commit('-am', 'main: one-sided domain')::text) AS committed;
SELECT dolt_checkout('feature');
UPDATE rt.domain_one_side SET w = 2;
SELECT length(dolt_commit('-am', 'feature: another column')::text) AS committed;
SELECT dolt_checkout('main');
BEGIN;
SELECT (dolt_merge('feature'))[2:4];
ROLLBACK;
SELECT * FROM rt.domain_one_side;
SELECT dolt_branch('-D', 'feature');

-- A retype during conflict resolution mustn't make the existing conflicts
-- unreadable.  A cast of JSON null to int returns SQL NULL: displaying it
-- as a NOT NULL domain must not enforce the domain either.
CREATE DOMAIN rt.required AS int NOT NULL;
CREATE TABLE rt.domain_mid (id int PRIMARY KEY, v int, n jsonb);
INSERT INTO rt.domain_mid VALUES (1, -1, 'null');
SELECT length(dolt_commit('-Am', 'mid base')::text) AS committed;
SELECT dolt_branch('feature');
UPDATE rt.domain_mid SET v = 1, n = '1';
SELECT length(dolt_commit('-am', 'main: mid')::text) AS committed;
SELECT dolt_checkout('feature');
UPDATE rt.domain_mid SET v = -2, n = '2';
SELECT length(dolt_commit('-am', 'feature: mid')::text) AS committed;
SELECT dolt_checkout('main');
BEGIN;
SELECT (dolt_merge('feature'))[2:4];
SAVEPOINT before_retype;
ALTER TABLE rt.domain_mid ALTER COLUMN v TYPE rt.positive;
SELECT base_row, our_row, their_row FROM dolt_conflicts(NULL::rt.domain_mid);
SAVEPOINT before_resolution;
SELECT dolt_conflicts_resolve('--theirs', 'rt.domain_mid');
ROLLBACK TO before_resolution;
SELECT * FROM rt.domain_mid;
SELECT count(*) AS conflicts_left FROM dolt_conflicts(NULL::rt.domain_mid);
ROLLBACK TO before_retype;
ALTER TABLE rt.domain_mid ALTER COLUMN n TYPE rt.required USING n::int;
SELECT base_row, our_row, their_row FROM dolt_conflicts(NULL::rt.domain_mid);
SELECT dolt_conflicts_resolve('--theirs', 'rt.domain_mid');
SELECT * FROM rt.domain_mid;
ROLLBACK;
SELECT dolt_branch('-D', 'feature');

-- Arrays still coerce their domain elements.  A bad historical element is
-- an unsure comparison, not a CHECK or NOT NULL error aborting the merge.
-- Different fixes conflict; matching fixes agree.  Resolution takes only
-- valid values, and a savepoint retry rebuilds the guarded reads.
CREATE TABLE rt.domain_arrays (id int PRIMARY KEY, v int[], n int[]);
INSERT INTO rt.domain_arrays VALUES (1, '{-1}', '{NULL}'), (2, '{-2}', '{NULL}');
SELECT length(dolt_commit('-Am', 'array domain base')::text) AS committed;
SELECT dolt_branch('feature');
UPDATE rt.domain_arrays SET v = ARRAY[id], n = ARRAY[id];
ALTER TABLE rt.domain_arrays ALTER COLUMN v TYPE rt.positive[],
                            ALTER COLUMN n TYPE rt.required[];
SELECT length(dolt_commit('-am', 'main: array domains')::text) AS committed;
SELECT dolt_checkout('feature');
UPDATE rt.domain_arrays SET v = '{2}', n = '{2}';
ALTER TABLE rt.domain_arrays ALTER COLUMN v TYPE rt.positive[],
                            ALTER COLUMN n TYPE rt.required[];
SELECT length(dolt_commit('-am', 'feature: array domains')::text) AS committed;
SELECT dolt_checkout('main');
BEGIN;
SAVEPOINT before_merge;
SELECT (dolt_merge('feature'))[2:4];
SELECT base_row, our_row, their_row FROM dolt_conflicts(NULL::rt.domain_arrays);
SELECT * FROM rt.domain_arrays ORDER BY id;
SELECT dolt_conflicts_resolve('--theirs', 'rt.domain_arrays');
SELECT * FROM rt.domain_arrays ORDER BY id;
SELECT count(*) AS conflicts_left FROM dolt_conflicts(NULL::rt.domain_arrays);
ROLLBACK TO before_merge;
SELECT (dolt_merge('feature'))[2:4];
SELECT count(*) AS conflicts FROM dolt_conflicts(NULL::rt.domain_arrays);
ROLLBACK;
SELECT dolt_branch('-D', 'feature');

-- Unwrapping nested domains retains the base type's typmod.  As with a
-- plain numeric(4,1) column, base's text reads as 1.3; theirs alone changed
-- it to 1.2, so the merge takes that edit without a conflict.
CREATE DOMAIN rt.tenths AS numeric(4,1);
CREATE DOMAIN rt.positive_tenths AS rt.tenths CHECK (VALUE > 0);
CREATE TABLE rt.domain_numeric (id int PRIMARY KEY, v text);
INSERT INTO rt.domain_numeric VALUES (1, '1.25');
SELECT length(dolt_commit('-Am', 'numeric domain base')::text) AS committed;
SELECT dolt_branch('feature');
ALTER TABLE rt.domain_numeric ALTER COLUMN v TYPE rt.positive_tenths USING v::numeric;
INSERT INTO rt.domain_numeric VALUES (2, 2.2);
SELECT length(dolt_commit('-am', 'main: tenths')::text) AS committed;
SELECT dolt_checkout('feature');
ALTER TABLE rt.domain_numeric ALTER COLUMN v TYPE rt.positive_tenths USING v::numeric;
UPDATE rt.domain_numeric SET v = 1.2;
INSERT INTO rt.domain_numeric VALUES (3, 3.3);
SELECT length(dolt_commit('-am', 'feature: tenths')::text) AS committed;
SELECT dolt_checkout('main');
BEGIN;
SELECT (dolt_merge('feature'))[2:4];
SELECT * FROM rt.domain_numeric ORDER BY id;
ROLLBACK;
SELECT dolt_branch('-D', 'feature');

-- The reverse cast used to check that a value is preserved mustn't
-- revalidate the source domain either.
CREATE DOMAIN rt.stored AS varchar(3);
CREATE TABLE rt.domain_source (id int PRIMARY KEY, v rt.stored);
INSERT INTO rt.domain_source VALUES (1, 'bad');
ALTER DOMAIN rt.stored ADD CONSTRAINT not_bad CHECK (VALUE <> 'bad') NOT VALID;
SELECT length(dolt_commit('-Am', 'source domain base')::text) AS committed;
ALTER TABLE rt.domain_source ALTER COLUMN v TYPE varchar(10);
SELECT length(dolt_commit('-am', 'source domain removed')::text) AS committed;
SELECT count(*) AS changed FROM dolt_diff('HEAD~1', 'HEAD', NULL::rt.domain_source);

DROP SCHEMA rt CASCADE;
