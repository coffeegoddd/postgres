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
-- the cast's value: code and tags conflict.  Name conflicts too: the
-- history cannot distinguish rpad from a plain cast and an edit on ours.
-- (A plain char(3)
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
-- new type, compare both outcomes.  The text reading's conflict stays:
-- char(5) key 'cd   ' as text is 'cd   ', a key neither side
-- has (they trimmed it), so base's rows read as deleted on both sides and
-- theirs' edited row against ours' is a conflict; keyless, base's rows
-- are copies neither side has, and both sides' inserts merge.  The cast
-- reading produces the same keyless rows, so those merges stay supported.
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


-- Both sides' plain casts give C, but theirs edits back to the old type's
-- text reading T: boolean true -> 'true' -> 't', and char padding trimmed
-- then restored.  Neither reading can identify the unchanged side, so all
-- three rows conflict.  Agreeing and NULL values still merge, as does an
-- independent edit in another column of a row with an ambiguous base.
CREATE TABLE rt.amb (id int PRIMARY KEY, f boolean, c char(5), v char(5), note int);
INSERT INTO rt.amb VALUES (1, true, NULL, NULL, 0), (2, NULL, 'ab', NULL, 0),
  (3, NULL, NULL, 'ab', 0), (4, false, 'cd', 'ef', 0), (5, NULL, NULL, NULL, 0);
SELECT dolt_commit('-Am', 'ambiguous base') IS NOT NULL AS committed;
SELECT dolt_branch('feature');
ALTER TABLE rt.amb ALTER COLUMN f TYPE text, ALTER COLUMN c TYPE text, ALTER COLUMN v TYPE varchar;
SELECT dolt_commit('-Am', 'ours cast') IS NOT NULL AS committed;
SELECT dolt_checkout('feature');
ALTER TABLE rt.amb ALTER COLUMN f TYPE text, ALTER COLUMN c TYPE text, ALTER COLUMN v TYPE varchar;
UPDATE rt.amb SET f = 't' WHERE id = 1;
UPDATE rt.amb SET c = 'ab   ' WHERE id = 2;
UPDATE rt.amb SET v = 'ab   ' WHERE id = 3;
UPDATE rt.amb SET note = 7 WHERE id IN (4, 5);
SELECT dolt_commit('-Am', 'theirs text forms') IS NOT NULL AS committed;
SELECT dolt_checkout('main');
BEGIN;
SELECT (dolt_merge('feature'))[2:4];
SELECT base_row, our_row, their_row FROM dolt_conflicts(NULL::rt.amb) ORDER BY 1;
SELECT * FROM rt.amb ORDER BY id;
SELECT dolt_conflicts_resolve('--theirs', 'rt.amb');
SELECT id, f, octet_length(c), octet_length(v), note FROM rt.amb ORDER BY id;
ROLLBACK;
-- Swapping sides must not silently discard either interpretation either.
SELECT dolt_checkout('feature');
BEGIN;
SELECT (dolt_merge('main'))[2:4];
SELECT base_row, our_row, their_row FROM dolt_conflicts(NULL::rt.amb) ORDER BY 1;
ROLLBACK;
SELECT dolt_checkout('main');
SELECT dolt_branch('-D', 'feature');

-- amb_key: text and cast readings disagree about a deletion (or copy count).
-- Refuse atomically in both directions; the successful insert-only and
-- pre-existing conflict cases above remain supported.
CREATE TABLE rt.amb_key (k char(5) PRIMARY KEY, v int);
INSERT INTO rt.amb_key VALUES ('ab', 1), ('cd', 2);
SELECT dolt_commit('-Am', 'amb_key base') IS NOT NULL AS committed;
SELECT dolt_branch('feature');
ALTER TABLE rt.amb_key ALTER COLUMN k TYPE text;
SELECT dolt_commit('-Am', 'ours cast') IS NOT NULL AS committed;
SELECT dolt_checkout('feature');
ALTER TABLE rt.amb_key ALTER COLUMN k TYPE text;
DELETE FROM rt.amb_key WHERE k = 'cd';
SELECT dolt_commit('-Am', 'theirs deleted') IS NOT NULL AS committed;
SELECT dolt_checkout('main');
SELECT dolt_hashof('HEAD') AS before_merge \gset
SELECT dolt_merge('feature');
SELECT dolt_hashof('HEAD') = :'before_merge' AND
  test_fold_root() = prolly_root_hash('HEAD') AS unchanged;
SELECT * FROM dolt_status();
SELECT quote_literal(k), v FROM rt.amb_key ORDER BY 1;
SELECT dolt_checkout('feature');
SELECT dolt_hashof('HEAD') AS before_merge \gset
SELECT dolt_merge('main');
SELECT dolt_hashof('HEAD') = :'before_merge' AND
  test_fold_root() = prolly_root_hash('HEAD') AS unchanged;
SELECT * FROM dolt_status();
SELECT quote_literal(k), v FROM rt.amb_key ORDER BY 1;
SELECT dolt_checkout('main');
SELECT dolt_branch('-D', 'feature');

-- amb_char: text and cast readings disagree about a deletion (or copy count).
-- Refuse atomically in both directions; the successful insert-only and
-- pre-existing conflict cases above remain supported.
CREATE TABLE rt.amb_char (c char(5), v int);
INSERT INTO rt.amb_char VALUES ('ab', 1), ('cd', 2);
SELECT dolt_commit('-Am', 'amb_char base') IS NOT NULL AS committed;
SELECT dolt_branch('feature');
ALTER TABLE rt.amb_char ALTER COLUMN c TYPE varchar(10);
SELECT dolt_commit('-Am', 'ours cast') IS NOT NULL AS committed;
SELECT dolt_checkout('feature');
ALTER TABLE rt.amb_char ALTER COLUMN c TYPE varchar(10);
DELETE FROM rt.amb_char WHERE c = 'cd';
SELECT dolt_commit('-Am', 'theirs deleted') IS NOT NULL AS committed;
SELECT dolt_checkout('main');
SELECT dolt_hashof('HEAD') AS before_merge \gset
SELECT dolt_merge('feature');
SELECT dolt_hashof('HEAD') = :'before_merge' AND
  test_fold_root() = prolly_root_hash('HEAD') AS unchanged;
SELECT * FROM dolt_status();
SELECT quote_literal(c), v FROM rt.amb_char ORDER BY 1;
SELECT dolt_checkout('feature');
SELECT dolt_hashof('HEAD') AS before_merge \gset
SELECT dolt_merge('main');
SELECT dolt_hashof('HEAD') = :'before_merge' AND
  test_fold_root() = prolly_root_hash('HEAD') AS unchanged;
SELECT * FROM dolt_status();
SELECT quote_literal(c), v FROM rt.amb_char ORDER BY 1;
SELECT dolt_checkout('main');
SELECT dolt_branch('-D', 'feature');

-- amb_bool: text and cast readings disagree about a deletion (or copy count).
-- Refuse atomically in both directions; the successful insert-only and
-- pre-existing conflict cases above remain supported.
CREATE TABLE rt.amb_bool (f boolean, v int);
INSERT INTO rt.amb_bool VALUES (true, 1), (false, 2);
SELECT dolt_commit('-Am', 'amb_bool base') IS NOT NULL AS committed;
SELECT dolt_branch('feature');
ALTER TABLE rt.amb_bool ALTER COLUMN f TYPE text;
SELECT dolt_commit('-Am', 'ours cast') IS NOT NULL AS committed;
SELECT dolt_checkout('feature');
ALTER TABLE rt.amb_bool ALTER COLUMN f TYPE text;
DELETE FROM rt.amb_bool WHERE f = 'false';
SELECT dolt_commit('-Am', 'theirs deleted') IS NOT NULL AS committed;
SELECT dolt_checkout('main');
SELECT dolt_hashof('HEAD') AS before_merge \gset
SELECT dolt_merge('feature');
SELECT dolt_hashof('HEAD') = :'before_merge' AND
  test_fold_root() = prolly_root_hash('HEAD') AS unchanged;
SELECT * FROM dolt_status();
SELECT * FROM rt.amb_bool ORDER BY v;
SELECT dolt_checkout('feature');
SELECT dolt_hashof('HEAD') AS before_merge \gset
SELECT dolt_merge('main');
SELECT dolt_hashof('HEAD') = :'before_merge' AND
  test_fold_root() = prolly_root_hash('HEAD') AS unchanged;
SELECT * FROM dolt_status();
SELECT * FROM rt.amb_bool ORDER BY v;
SELECT dolt_checkout('main');
SELECT dolt_branch('-D', 'feature');

-- amb_copies: text and cast readings disagree about a deletion (or copy count).
-- Refuse atomically in both directions; the successful insert-only and
-- pre-existing conflict cases above remain supported.
CREATE TABLE rt.amb_copies (c char(5), v int);
INSERT INTO rt.amb_copies VALUES ('ab', 1), ('cd', 2), ('cd', 2);
SELECT dolt_commit('-Am', 'amb_copies base') IS NOT NULL AS committed;
SELECT dolt_branch('feature');
ALTER TABLE rt.amb_copies ALTER COLUMN c TYPE varchar(10);
SELECT dolt_commit('-Am', 'ours cast') IS NOT NULL AS committed;
SELECT dolt_checkout('feature');
ALTER TABLE rt.amb_copies ALTER COLUMN c TYPE varchar(10);
DELETE FROM rt.amb_copies WHERE ctid = (SELECT ctid FROM rt.amb_copies WHERE c = 'cd' LIMIT 1);
SELECT dolt_commit('-Am', 'theirs deleted') IS NOT NULL AS committed;
SELECT dolt_checkout('main');
SELECT dolt_hashof('HEAD') AS before_merge \gset
SELECT dolt_merge('feature');
SELECT dolt_hashof('HEAD') = :'before_merge' AND
  test_fold_root() = prolly_root_hash('HEAD') AS unchanged;
SELECT * FROM dolt_status();
SELECT quote_literal(c), v, count(*) FROM rt.amb_copies GROUP BY c, v ORDER BY 1;
SELECT dolt_checkout('feature');
SELECT dolt_hashof('HEAD') AS before_merge \gset
SELECT dolt_merge('main');
SELECT dolt_hashof('HEAD') = :'before_merge' AND
  test_fold_root() = prolly_root_hash('HEAD') AS unchanged;
SELECT * FROM dolt_status();
SELECT quote_literal(c), v, count(*) FROM rt.amb_copies GROUP BY c, v ORDER BY 1;
SELECT dolt_checkout('main');
-- Both sides removing one copy agrees under both readings and is safe.
DELETE FROM rt.amb_copies WHERE ctid = (SELECT ctid FROM rt.amb_copies WHERE c = 'cd' LIMIT 1);
SELECT dolt_commit('-Am', 'ours deleted too') IS NOT NULL AS committed;
SELECT (dolt_merge('feature'))[2:4];
SELECT quote_literal(c), v, count(*) FROM rt.amb_copies GROUP BY c, v ORDER BY 1;
SELECT * FROM dolt_status();
SELECT dolt_branch('-D', 'feature');

-- amb_mix: an ambiguous key beside an unsure non-key column.  The cast
-- reading keeps f unsure, so theirs' 't' and ours' 'true' differ either
-- way, but the readings still disagree about theirs' deletion of 'cd'.
CREATE TABLE rt.amb_mix (k char(5) PRIMARY KEY, f boolean, v int);
INSERT INTO rt.amb_mix VALUES ('ab', true, 1), ('cd', false, 2);
SELECT dolt_commit('-Am', 'amb_mix base') IS NOT NULL AS committed;
SELECT dolt_branch('feature');
ALTER TABLE rt.amb_mix ALTER COLUMN k TYPE text, ALTER COLUMN f TYPE text;
SELECT dolt_commit('-Am', 'ours cast') IS NOT NULL AS committed;
SELECT dolt_checkout('feature');
ALTER TABLE rt.amb_mix ALTER COLUMN k TYPE text, ALTER COLUMN f TYPE text;
UPDATE rt.amb_mix SET f = 't' WHERE k = 'ab';
DELETE FROM rt.amb_mix WHERE k = 'cd';
SELECT dolt_commit('-Am', 'theirs edited and deleted') IS NOT NULL AS committed;
SELECT dolt_checkout('main');
SELECT dolt_hashof('HEAD') AS before_merge \gset
SELECT dolt_merge('feature');
SELECT dolt_hashof('HEAD') = :'before_merge' AND
  test_fold_root() = prolly_root_hash('HEAD') AS unchanged;
SELECT * FROM dolt_status();
SELECT quote_literal(k), f, v FROM rt.amb_mix ORDER BY 1;
SELECT dolt_branch('-D', 'feature');

-- amb_upd: keyless, ours updates a row and theirs inserts one.  Read as
-- text, base's rows are copies neither side has, so theirs' unchanged
-- ('ab', 1) would come back beside ours' ('ab', 10); read through the
-- cast, ours' update stands.  The readings differ: refused.
CREATE TABLE rt.amb_upd (c char(5), v int);
INSERT INTO rt.amb_upd VALUES ('ab', 1), ('cd', 2);
SELECT dolt_commit('-Am', 'amb_upd base') IS NOT NULL AS committed;
SELECT dolt_branch('feature');
ALTER TABLE rt.amb_upd ALTER COLUMN c TYPE varchar(10);
UPDATE rt.amb_upd SET v = 10 WHERE c = 'ab';
SELECT dolt_commit('-Am', 'ours cast and updated') IS NOT NULL AS committed;
SELECT dolt_checkout('feature');
ALTER TABLE rt.amb_upd ALTER COLUMN c TYPE varchar(10);
INSERT INTO rt.amb_upd VALUES ('ef', 3);
SELECT dolt_commit('-Am', 'theirs inserted') IS NOT NULL AS committed;
SELECT dolt_checkout('main');
SELECT dolt_hashof('HEAD') AS before_merge \gset
SELECT dolt_merge('feature');
SELECT dolt_hashof('HEAD') = :'before_merge' AND
  test_fold_root() = prolly_root_hash('HEAD') AS unchanged;
SELECT * FROM dolt_status();
SELECT quote_literal(c), v FROM rt.amb_upd ORDER BY 1, 2;
SELECT dolt_branch('-D', 'feature');

-- amb_comp: a composite key with one of its columns retyped.
CREATE TABLE rt.amb_comp (k char(5), n int, v int, PRIMARY KEY (k, n));
INSERT INTO rt.amb_comp VALUES ('ab', 1, 1), ('cd', 2, 2);
SELECT dolt_commit('-Am', 'amb_comp base') IS NOT NULL AS committed;
SELECT dolt_branch('feature');
ALTER TABLE rt.amb_comp ALTER COLUMN k TYPE text;
SELECT dolt_commit('-Am', 'ours cast') IS NOT NULL AS committed;
SELECT dolt_checkout('feature');
ALTER TABLE rt.amb_comp ALTER COLUMN k TYPE text;
DELETE FROM rt.amb_comp WHERE k = 'cd' AND n = 2;
SELECT dolt_commit('-Am', 'theirs deleted') IS NOT NULL AS committed;
SELECT dolt_checkout('main');
SELECT dolt_hashof('HEAD') AS before_merge \gset
SELECT dolt_merge('feature');
SELECT dolt_hashof('HEAD') = :'before_merge' AND
  test_fold_root() = prolly_root_hash('HEAD') AS unchanged;
SELECT * FROM dolt_status();
SELECT quote_literal(k), n, v FROM rt.amb_comp ORDER BY 1, 2;
SELECT dolt_branch('-D', 'feature');

-- amb_pick: a cherry-pick refuses the same way, its commit's parent the
-- base.
CREATE TABLE rt.amb_pick (k char(5) PRIMARY KEY, v int);
INSERT INTO rt.amb_pick VALUES ('ab', 1), ('cd', 2);
SELECT dolt_commit('-Am', 'amb_pick base') IS NOT NULL AS committed;
SELECT dolt_branch('feature');
ALTER TABLE rt.amb_pick ALTER COLUMN k TYPE text;
SELECT dolt_commit('-Am', 'ours cast') IS NOT NULL AS committed;
SELECT dolt_checkout('feature');
ALTER TABLE rt.amb_pick ALTER COLUMN k TYPE text;
DELETE FROM rt.amb_pick WHERE k = 'cd';
SELECT dolt_commit('-Am', 'theirs cast and deleted') IS NOT NULL AS committed;
SELECT dolt_hashof('HEAD') AS pick \gset
SELECT dolt_checkout('main');
SELECT dolt_hashof('HEAD') AS before_pick \gset
SELECT dolt_cherry_pick(:'pick');
SELECT dolt_hashof('HEAD') = :'before_pick' AND
  test_fold_root() = prolly_root_hash('HEAD') AS unchanged;
SELECT * FROM dolt_status();
SELECT quote_literal(k), v FROM rt.amb_pick ORDER BY 1;
SELECT dolt_branch('-D', 'feature');

DROP SCHEMA rt CASCADE;
