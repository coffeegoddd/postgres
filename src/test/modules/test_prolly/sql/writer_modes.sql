-- Writer modes: in the shared mode (the default) every session writes a
-- branch; in the owned mode a dirty branch is one writer's (dolt.writer),
-- claimed by its first write and given up when it is clean again.
CREATE SCHEMA wm VERSIONED;
CREATE TABLE wm.t (id int PRIMARY KEY, v text);
INSERT INTO wm.t VALUES (1, 'base');
CREATE ROLE wm_other;
CREATE ROLE wm_carol;
GRANT USAGE ON SCHEMA wm TO wm_other, wm_carol;
GRANT ALL ON wm.t TO wm_other, wm_carol;
SELECT length(dolt_commit('-m', 'wm base')::text) AS committed;
SELECT dolt_writer_mode();

-- Shared: writers mix, and nobody owns the branch.
SET dolt.writer = 'alice';
INSERT INTO wm.t VALUES (2, 'alice');
SET dolt.writer = 'bob';
INSERT INTO wm.t VALUES (3, 'bob');
SELECT name, dirty, owner, owner_role FROM dolt_branches() WHERE name = 'main';
SELECT dolt_reset('--hard');

-- Owned: the first write claims the branch, as a catalog row of the
-- transaction (gone if it aborts).
SELECT dolt_writer_mode('owned');
SELECT dolt_writer_mode();
SET dolt.writer = 'alice';
BEGIN;
INSERT INTO wm.t VALUES (2, 'alice');
ROLLBACK;
SELECT name, dirty, owner FROM dolt_branches() WHERE name = 'main';
INSERT INTO wm.t VALUES (2, 'alice');
SELECT name, dirty, owner, owner_role = current_user AS owner_role
FROM dolt_branches() WHERE name = 'main';

-- The owner writes on; anyone else is refused at once, rows and
-- definitions and sequences alike.
UPDATE wm.t SET v = 'alice again' WHERE id = 2;
SET dolt.writer = 'bob';
INSERT INTO wm.t VALUES (3, 'bob');
CREATE TABLE wm.u (id int);
CREATE SEQUENCE wm.s;
SELECT dolt_commit('-m', 'bob commits alice''s work');
SELECT * FROM wm.t ORDER BY id;

-- Reads and unversioned writes are anyone's.
CREATE TABLE wm_plain (id int);
INSERT INTO wm_plain VALUES (1);
DROP TABLE wm_plain;

-- A transaction is one writer's: it can't change writer once it has
-- written.
SET dolt.writer = 'alice';
BEGIN;
INSERT INTO wm.t VALUES (4, 'alice');
SET LOCAL dolt.writer = 'bob';
ROLLBACK;
SET dolt.writer = 'not a writer!';
SET dolt.writer = 'a_writer_name_far_longer_than_thirty_two_bytes';

-- The owner's commit makes the branch clean, and anyone's to claim.
SELECT length(dolt_commit('-m', 'alice''s')::text) AS committed;
SELECT name, dirty, owner FROM dolt_branches() WHERE name = 'main';
SET dolt.writer = 'bob';
INSERT INTO wm.t VALUES (3, 'bob');
SELECT name, owner FROM dolt_branches() WHERE name = 'main';

-- A reset by the owner discards the changes and the claim.
SELECT dolt_reset('--hard');
SELECT name, dirty, owner FROM dolt_branches() WHERE name = 'main';

-- A commit in the middle of a transaction gives the branch up, and the
-- transaction's next write claims it again.
BEGIN;
INSERT INTO wm.t VALUES (30, 'bob');
SELECT length(dolt_commit('-m', 'bob, mid-transaction')::text) AS committed;
SELECT name, owner FROM dolt_branches() WHERE name = 'main';
INSERT INTO wm.t VALUES (31, 'bob');
COMMIT;
SELECT name, dirty, owner FROM dolt_branches() WHERE name = 'main';
SELECT dolt_reset('--hard');
-- (taking a clean branch over takes nothing: the next write claims it)
SELECT dolt_branch('--take', 'main');
SELECT name, owner FROM dolt_branches() WHERE name = 'main';

-- An aborted merge is a reset: the owner's, and it gives the branch up.
SELECT dolt_branch('wm_c');
SELECT dolt_checkout('wm_c');
UPDATE wm.t SET v = 'on c' WHERE id = 1;
SELECT length(dolt_commit('-Am', 'wm c')::text) AS committed;
SELECT dolt_checkout('main');
UPDATE wm.t SET v = 'on main' WHERE id = 1;
SELECT length(dolt_commit('-Am', 'wm main')::text) AS committed;
SET dolt.allow_commit_conflicts = on;
SELECT (dolt_merge('wm_c'))[3] AS conflicts;
RESET dolt.allow_commit_conflicts;
SELECT name, owner FROM dolt_branches() WHERE name = 'main';
SET dolt.writer = 'alice';
SELECT dolt_merge('--abort');
SET dolt.writer = 'bob';
SELECT dolt_merge('--abort') IS NOT NULL AS aborted;
SELECT name, dirty, owner FROM dolt_branches() WHERE name = 'main';
SELECT dolt_branch('-D', 'wm_c');

-- A write that changes nothing still claims (the branch counts as dirty
-- until a commit or reset says otherwise).
INSERT INTO wm.t VALUES (5, 'gone');
DELETE FROM wm.t WHERE id = 5;
SELECT name, dirty, owner FROM dolt_branches() WHERE name = 'main';
SET dolt.writer = 'alice';
INSERT INTO wm.t VALUES (6, 'alice');
SELECT dolt_reset('--hard');	-- (the database's owner discards bob's)
SELECT name, dirty, owner FROM dolt_branches() WHERE name = 'main';

-- Another role can't write as a writer that is a role's, nor take a
-- branch over without owning the database or being the role.
SET ROLE wm_carol;
SET dolt.writer = 'carol';
INSERT INTO wm.t VALUES (7, 'carol');
SET ROLE wm_other;
INSERT INTO wm.t VALUES (8, 'carol, spoofed');
SELECT dolt_branch('--take', 'main');
SET dolt.writer = 'dave';
INSERT INTO wm.t VALUES (8, 'dave');
RESET ROLE;

-- The database's owner takes it over: carol's changes become dave's.
SET dolt.writer = 'dave';
SELECT dolt_branch('--take', 'main');
SELECT name, owner FROM dolt_branches() WHERE name = 'main';
INSERT INTO wm.t VALUES (8, 'dave');
SELECT length(dolt_commit('-m', 'carol''s and dave''s')::text) AS committed;
SELECT name, dirty, owner FROM dolt_branches() WHERE name = 'main';

-- Branches are owned each on their own.
SELECT dolt_checkout('-b', 'wm_f');
INSERT INTO wm.t VALUES (9, 'dave on f');
SET dolt.writer = 'erin';
SELECT dolt_checkout('main');
INSERT INTO wm.t VALUES (9, 'erin on main');
SELECT name, owner FROM dolt_branches() WHERE name IN ('main', 'wm_f') ORDER BY name;
SELECT dolt_checkout('wm_f');
INSERT INTO wm.t VALUES (10, 'erin on f');
SET dolt.writer = 'dave';
SELECT dolt_reset('--hard');
SELECT dolt_checkout('main');
SET dolt.writer = 'erin';
SELECT dolt_reset('--hard');
SELECT dolt_branch('-D', 'wm_f');

-- Only the database's owner sets the mode; and back to shared.
SET ROLE wm_other;
SELECT dolt_writer_mode('shared');
RESET ROLE;
SELECT dolt_writer_mode('nobody');
SELECT dolt_writer_mode('shared');
RESET dolt.writer;
REVOKE ALL ON wm.t FROM wm_other, wm_carol;
REVOKE USAGE ON SCHEMA wm FROM wm_other, wm_carol;
DROP ROLE wm_other;
DROP ROLE wm_carol;
