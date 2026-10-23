-- Merging the rows of a table one side redefined while the other changed
-- them: the three versions' rows are read as the merged definition has
-- them, column by column by name, and merged three-way.  dolt_diff and
-- dolt_conflicts read rows of other definitions the same way.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;

CREATE SCHEMA rm VERSIONED;
CREATE TABLE rm.t (id int PRIMARY KEY, v text, n int);
INSERT INTO rm.t SELECT g, 'v' || g, g FROM generate_series(1, 5) g;
SELECT length(dolt_commit('-Am', 'base')::text) AS committed;
SELECT dolt_branch('feature');

-- Theirs adds a column and changes a row; ours changes others.
SELECT dolt_checkout('feature');
ALTER TABLE rm.t ADD COLUMN w int DEFAULT 7;
UPDATE rm.t SET v = 'feature' WHERE id = 1;
SELECT length(dolt_commit('-am', 'feature: a column, a row')::text) AS committed;
SELECT dolt_checkout('main');
UPDATE rm.t SET v = 'main' WHERE id = 2;
INSERT INTO rm.t VALUES (6, 'six', 6);
SELECT length(dolt_commit('-am', 'main: rows')::text) AS committed;
SELECT (dolt_merge('feature', '-m', 'merged'))[3:4];
SELECT * FROM rm.t ORDER BY id;
SELECT count(*) AS clean FROM dolt_status;

-- dolt_diff of versions defined otherwise reads their rows as now.
SELECT diff_type, from_row, to_row FROM dolt_diff('HEAD~1', 'HEAD', NULL::rm.t)
ORDER BY (coalesce(to_row, from_row)).id;

-- Ours drops a column; theirs changes rows.
SELECT dolt_checkout('feature');
SELECT dolt_reset('--hard', 'main');
UPDATE rm.t SET v = 'feature again' WHERE id = 3;
SELECT length(dolt_commit('-am', 'feature: a row')::text) AS committed;
SELECT dolt_checkout('main');
ALTER TABLE rm.t DROP COLUMN w;
UPDATE rm.t SET n = 400 WHERE id = 4;
SELECT length(dolt_commit('-am', 'main: no w')::text) AS committed;
SELECT (dolt_merge('feature', '-m', 'merged again'))[3:4];
SELECT * FROM rm.t ORDER BY id;

-- Ours retypes a column theirs's rows changed under: a schema conflict, as
-- in Dolt (only a string type made no shorter merges across the other
-- side's changes), and the merge doesn't happen.
SELECT dolt_checkout('feature');
SELECT dolt_reset('--hard', 'main');
UPDATE rm.t SET n = 30 WHERE id = 3;
SELECT length(dolt_commit('-am', 'feature: n of a row')::text) AS committed;
SELECT dolt_checkout('main');
ALTER TABLE rm.t ALTER COLUMN n TYPE bigint;
SELECT length(dolt_commit('-am', 'main: n bigint')::text) AS committed;
SELECT (dolt_merge('feature', '-m', 'not merged'))[3:4];
SELECT * FROM rm.t ORDER BY id;

-- A row both changed is a conflict, listed as now defined.
SELECT dolt_checkout('feature');
SELECT dolt_reset('--hard', 'main');
ALTER TABLE rm.t ADD COLUMN z text;
UPDATE rm.t SET v = 'theirs' WHERE id = 5;
SELECT length(dolt_commit('-am', 'feature: z, row 5')::text) AS committed;
SELECT dolt_checkout('main');
UPDATE rm.t SET v = 'ours' WHERE id = 5;
SELECT length(dolt_commit('-am', 'main: row 5')::text) AS committed;
BEGIN;
SELECT (dolt_merge('feature'))[3:4];
SELECT base_row, our_row, their_row FROM dolt_conflicts(NULL::rm.t);
SELECT dolt_conflicts_resolve('--theirs', 'rm.t');
SELECT id, v, z FROM rm.t WHERE id = 5;
SELECT length(dolt_commit('-am', 'resolved')::text) AS committed;
COMMIT;

-- A conflict in a table neither base nor ours had (both made it): its
-- versions are listed, the base's as none.
SELECT dolt_checkout('feature');
SELECT dolt_reset('--hard', 'main');
CREATE TABLE rm.both (id int PRIMARY KEY, v text);
INSERT INTO rm.both VALUES (1, 'theirs');
SELECT length(dolt_commit('-Am', 'feature: both')::text) AS committed;
SELECT dolt_checkout('main');
CREATE TABLE rm.both (id int PRIMARY KEY, v text);
INSERT INTO rm.both VALUES (1, 'ours');
SELECT length(dolt_commit('-Am', 'main: both')::text) AS committed;
BEGIN;
SELECT (dolt_merge('feature'))[3:4];
SELECT base_row, our_row, their_row FROM dolt_conflicts(NULL::rm.both);
ROLLBACK;
DROP TABLE rm.both;
SELECT length(dolt_commit('-am', 'main: not both')::text) AS committed;

-- Theirs keys a keyless table and ours leaves its rows: theirs' rows,
-- whole, as before (ours' can't be read keyed).
ALTER TABLE rm.t ADD COLUMN u int;
CREATE TABLE rm.k (id int, v text);
INSERT INTO rm.k VALUES (1, 'one'), (2, 'two');
SELECT length(dolt_commit('-Am', 'main: a column, a keyless table')::text) AS committed;
SELECT dolt_checkout('feature');
SELECT dolt_reset('--hard', 'main');
ALTER TABLE rm.k ADD PRIMARY KEY (id);
UPDATE rm.k SET v = 'uno' WHERE id = 1;
SELECT length(dolt_commit('-am', 'feature: rm.k keyed')::text) AS committed;
SELECT dolt_checkout('main');
SELECT length(dolt_commit('--allow-empty', '-m', 'main: nothing')::text) AS committed;
SELECT (dolt_merge('feature', '-m', 'merged a key'))[3:4];
SELECT id, v FROM rm.k ORDER BY id;

-- A column renamed on one side while the other changed it is matched by
-- its identity, which the rename keeps: both changes merge.  Dropped
-- instead, the change would be lost: refused.
SELECT dolt_checkout('feature');
SELECT dolt_reset('--hard', 'main');
ALTER TABLE rm.t RENAME COLUMN n TO n2;
SELECT length(dolt_commit('-am', 'feature: n renamed')::text) AS committed;
SELECT dolt_checkout('main');
UPDATE rm.t SET n = 100 WHERE id = 1;
SELECT length(dolt_commit('-am', 'main: n of 1')::text) AS committed;
SELECT (dolt_merge('feature', '-m', 'merged a rename'))[3:4];
SELECT id, n2 FROM rm.t ORDER BY id;
SELECT dolt_reset('--hard', 'HEAD~1');
SELECT dolt_checkout('feature');
SELECT dolt_reset('--hard', 'main~1');
ALTER TABLE rm.t DROP COLUMN n;
SELECT length(dolt_commit('-am', 'feature: n dropped')::text) AS committed;
SELECT dolt_checkout('main');
SELECT (dolt_merge('feature', '-m', 'merged a drop'))[3:4];
SELECT dolt_reset('--hard');

-- A serial column one side added: its rows' values can't be told from the
-- other side's rows, which don't have it: refused, not a false conflict.
SELECT dolt_checkout('feature');
SELECT dolt_reset('--hard', 'main');
UPDATE rm.t SET u = 3 WHERE id = 3;
SELECT length(dolt_commit('-am', 'feature: u of 3')::text) AS committed;
SELECT dolt_checkout('main');
ALTER TABLE rm.t ADD COLUMN s serial;
SELECT length(dolt_commit('-am', 'main: a serial')::text) AS committed;
SELECT (dolt_merge('feature', '-m', 'merged a serial'))[3:4];
ALTER TABLE rm.t DROP COLUMN s;
SELECT length(dolt_commit('-am', 'main: no serial')::text) AS committed;

-- A default with a newline in it doesn't hide the columns after it.
SELECT dolt_checkout('feature');
SELECT dolt_reset('--hard', 'main');
ALTER TABLE rm.t ADD COLUMN note text DEFAULT E'a\nb', ADD COLUMN after_note int DEFAULT 9;
UPDATE rm.t SET after_note = 10 WHERE id = 2;
SELECT length(dolt_commit('-am', 'feature: a default with a newline')::text) AS committed;
SELECT dolt_checkout('main');
UPDATE rm.t SET u = 4 WHERE id = 4;
SELECT length(dolt_commit('-am', 'main: u of 4')::text) AS committed;
SELECT (dolt_merge('feature', '-m', 'merged past a newline'))[3:4];
SELECT id, u, note = E'a\nb' AS note_kept, after_note FROM rm.t ORDER BY id;

SELECT dolt_branch('-D', 'feature');
DROP SCHEMA rm CASCADE;
