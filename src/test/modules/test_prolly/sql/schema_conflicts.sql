-- Schema conflicts kept, as Dolt keeps them: a table both branches changed
-- in ways that don't merge (its definitions changed differently, or one
-- dropped it and the other changed it) is left as the branch has it, its
-- rows unmerged, and the conflict is recorded in the merge in progress.
-- The other tables merge.  dolt_schema_conflicts lists each with its
-- statements in the base, ours and theirs; dolt_conflicts_resolve()
-- refuses them, as Dolt's does; a commit is refused while they remain;
-- dolt_merge('--abort') ends the merge.  Dolt's cases from its
-- SchemaChangeTestsSchemaConflicts, in both directions.
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;

CREATE SCHEMA sc VERSIONED;
CREATE TABLE sc.t (pk int PRIMARY KEY, col1 int, col2 varchar(100));
CREATE TABLE sc.other (id int PRIMARY KEY, v text);
INSERT INTO sc.t VALUES (1, 10, 'a'), (2, 20, 'b');
INSERT INTO sc.other VALUES (1, 'one');
SELECT length(dolt_commit('-Am', 'base')::text) AS committed;
SELECT dolt_branch('right');

-- Dolt's "changing the type of a column": theirs retypes col1 and adds
-- rows, ours adds rows; and both change sc.other's rows, which merge
SELECT dolt_checkout('right') IS NOT NULL AS on_right;
ALTER TABLE sc.t ALTER COLUMN col1 TYPE varchar(100);
INSERT INTO sc.t VALUES (3, 'thirty', 'c');
INSERT INTO sc.other VALUES (3, 'three');
SELECT length(dolt_commit('-am', 'right')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
INSERT INTO sc.t VALUES (5, 50, 'e');
INSERT INTO sc.other VALUES (5, 'five');
SELECT length(dolt_commit('-am', 'left')::text) AS committed;

-- outside a transaction block: refused, as Dolt's autocommit merge is
SELECT dolt_merge('right');
SELECT count(*) AS no_merge_state FROM dolt_schema_conflicts;

BEGIN;
SELECT (dolt_merge('right'))[2:4];
SELECT table_name, description FROM dolt_schema_conflicts;
SELECT base_schema = our_schema AS ours_unchanged,
	   their_schema LIKE '%col1 character varying(100)%' AS theirs_retyped,
	   our_schema LIKE '%col1 integer%' AS ours_integer
FROM dolt_schema_conflicts;
-- the table is the branch's, its rows unmerged; the other table merged
SELECT pk, col1 FROM sc.t ORDER BY pk;
SELECT pg_typeof(col1) FROM sc.t LIMIT 1;
SELECT id, v FROM sc.other ORDER BY id;
SELECT table_name, staged, status FROM dolt_status ORDER BY table_name;
-- an ordinary table meanwhile
INSERT INTO sc.t VALUES (7, 70, 'g');
SAVEPOINT s;
UPDATE sc.t SET col1 = 71 WHERE pk = 7;
ROLLBACK TO SAVEPOINT s;
SELECT col1 FROM sc.t WHERE pk = 7;
-- not resolved, as in Dolt
SAVEPOINT r;
SELECT dolt_conflicts_resolve('--ours', 'sc.t');
ROLLBACK TO SAVEPOINT r;
SAVEPOINT c;
SELECT dolt_commit('-am', 'merged');
ROLLBACK TO SAVEPOINT c;
SELECT (dolt_merge('--abort'))[4];
SELECT count(*) AS after_abort FROM dolt_schema_conflicts;
SELECT pk FROM sc.t ORDER BY pk;
SELECT id FROM sc.other ORDER BY id;
COMMIT;

-- the same, the other way round: theirs is main
SELECT dolt_checkout('right') IS NOT NULL AS on_right;
BEGIN;
SELECT (dolt_merge('main'))[2:4];
SELECT table_name, description FROM dolt_schema_conflicts;
SELECT our_schema LIKE '%col1 character varying(100)%' AS ours_retyped,
	   their_schema = base_schema AS theirs_unchanged
FROM dolt_schema_conflicts;
SELECT pk, col1 FROM sc.t ORDER BY pk;
SELECT id FROM sc.other ORDER BY id;
SELECT (dolt_merge('--abort'))[4];
COMMIT;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;

-- with dolt.allow_commit_conflicts, a merge outside a block keeps them;
-- the branch can't commit until the merge is aborted
SET dolt.allow_commit_conflicts = on;
SELECT (dolt_merge('right'))[2:4];
SELECT table_name FROM dolt_schema_conflicts;
SELECT dolt_commit('-am', 'merged');
SELECT (dolt_merge('--abort'))[4];
RESET dolt.allow_commit_conflicts;

-- Dolt's "type changes to a column on both sides of a merge"
SELECT dolt_branch('both_right');
SELECT dolt_branch('both_left');
SELECT dolt_checkout('both_right') IS NOT NULL AS on_right;
ALTER TABLE sc.t ALTER COLUMN col2 TYPE text;
SELECT length(dolt_commit('-am', 'text')::text) AS committed;
SELECT dolt_checkout('both_left') IS NOT NULL AS on_left;
ALTER TABLE sc.t ALTER COLUMN col2 TYPE varchar(10);
SELECT length(dolt_commit('-am', 'varchar')::text) AS committed;
BEGIN;
SELECT (dolt_merge('both_right'))[2:4];
SELECT table_name FROM dolt_schema_conflicts;
SELECT (dolt_merge('--abort'))[4];
COMMIT;

-- Dolt's "both sides add an index with the same name, same columns, but
-- different type" (here: one partial)
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SELECT dolt_branch('idx_right');
SELECT dolt_branch('idx_left');
SELECT dolt_checkout('idx_right') IS NOT NULL AS on_right;
CREATE INDEX idx1 ON sc.t (col2) WHERE pk > 1;
INSERT INTO sc.t VALUES (10, 100, '100');
SELECT length(dolt_commit('-am', 'idx partial')::text) AS committed;
SELECT dolt_checkout('idx_left') IS NOT NULL AS on_left;
CREATE INDEX idx1 ON sc.t (col2);
INSERT INTO sc.t VALUES (20, 200, '200');
SELECT length(dolt_commit('-am', 'idx')::text) AS committed;
BEGIN;
SELECT (dolt_merge('idx_right'))[2:4];
SELECT table_name, our_schema LIKE '%idx1%' AS ours_has_idx1,
	   their_schema LIKE '%WHERE%' AS theirs_partial
FROM dolt_schema_conflicts;
SELECT pk FROM sc.t ORDER BY pk;
SELECT (dolt_merge('--abort'))[4];
COMMIT;

-- Dolt's "modifying a column on one side of a merge, and deleting it on
-- the other"; and a table dropped on one side and changed on the other
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SELECT dolt_branch('drop_right');
SELECT dolt_branch('drop_left');
SELECT dolt_checkout('drop_right') IS NOT NULL AS on_right;
ALTER TABLE sc.t DROP COLUMN col1;
DROP TABLE sc.other;
SELECT length(dolt_commit('-Am', 'drops')::text) AS committed;
SELECT dolt_checkout('drop_left') IS NOT NULL AS on_left;
ALTER TABLE sc.t ALTER COLUMN col1 TYPE bigint;
ALTER TABLE sc.other ADD COLUMN w int;
SELECT length(dolt_commit('-am', 'changes')::text) AS committed;
BEGIN;
SELECT (dolt_merge('drop_right'))[2:4];
SELECT table_name, their_schema, description FROM dolt_schema_conflicts
WHERE table_name LIKE '%other';
SELECT table_name FROM dolt_schema_conflicts ORDER BY table_name;
SELECT count(*) AS other_kept FROM sc.other;
SELECT (dolt_merge('--abort'))[4];
COMMIT;
-- and the other way round: ours dropped it
SELECT dolt_checkout('drop_right') IS NOT NULL AS on_right;
BEGIN;
SELECT (dolt_merge('drop_left'))[2:4];
SELECT table_name, our_schema, description FROM dolt_schema_conflicts
WHERE table_name LIKE '%other';
SELECT to_regclass('sc.other') IS NULL AS still_dropped;
SELECT (dolt_merge('--abort'))[4];
COMMIT;

-- a pick or a revert still refuses a schema conflict
SELECT dolt_checkout('drop_left') IS NOT NULL AS on_left;
SELECT dolt_cherry_pick('drop_right');

SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SELECT dolt_branch('-D', b) FROM unnest(ARRAY['right', 'both_right',
	'both_left', 'idx_right', 'idx_left', 'drop_right', 'drop_left']) b;
DROP SCHEMA sc CASCADE;
SELECT length(dolt_commit('-Am', 'sc gone')::text) AS committed;
