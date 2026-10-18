-- A move (a branch's first use, reset, merge, cherry-pick) to a commit
-- that needs what the database lacks is refused, naming all of it, before
-- anything moves.  A move never makes or changes an extension: one the
-- commit needs is the user's to install or update, for every branch.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;

CREATE SCHEMA mv_util;
CREATE FUNCTION mv_util.one() RETURNS int LANGUAGE sql AS 'SELECT 1';
CREATE SCHEMA mv VERSIONED;
CREATE TABLE mv.t (id int PRIMARY KEY, n int);
SELECT length(dolt_commit('-Am', 'base')::text) AS committed;
ALTER TABLE mv.t ALTER COLUMN n SET DEFAULT mv_util.one();
SELECT (dolt_commit('-Am', 'default'))[1] AS needs_one \gset
SELECT dolt_checkout('-b', 'side');
INSERT INTO mv.t (id) VALUES (1);
SELECT (dolt_commit('-Am', 'side row'))[1] AS side \gset
SELECT dolt_checkout('main');
SELECT dolt_branch('-D', 'side');		-- (its copies would need the function)
SELECT dolt_branch('old', 'HEAD');		-- only a head, until first used
ALTER TABLE mv.t ALTER COLUMN n DROP DEFAULT;
SELECT length(dolt_commit('-Am', 'no default')::text) AS committed;
DROP FUNCTION mv_util.one();

-- each move to a commit needing the function: refused, nothing moved
SELECT dolt_checkout('old');
SELECT dolt_reset('--hard', :'needs_one');
SELECT dolt_cherry_pick(:'needs_one');
SELECT dolt_merge(:'side');
SELECT * FROM dolt_status;
SELECT column_default FROM information_schema.columns
  WHERE table_schema = 'mv' AND column_name = 'n';
-- with the function made again, each goes ahead
CREATE FUNCTION mv_util.one() RETURNS int LANGUAGE sql AS 'SELECT 1';
SELECT dolt_checkout('old');
SELECT column_default FROM information_schema.columns
  WHERE table_schema = 'mv' AND column_name = 'n';
SELECT dolt_checkout('main');
SELECT dolt_merge(:'side') IS NOT NULL AS merged;
SELECT id, n FROM mv.t;

-- a commit (made elsewhere) needing an extension the database lacks: the
-- move says to install it; nothing is installed
SELECT test_commit_needing(test_fold_root(), ARRAY['extension ltree'],
						   ARRAY['{1.3,public}']) AS lt \gset
SELECT dolt_branch('needs_ltree', :'lt');
SELECT dolt_checkout('needs_ltree');
SELECT dolt_reset('--hard', :'lt');
SELECT count(*) AS ltree FROM pg_extension WHERE extname = 'ltree';

SELECT dolt_branch('-D', 'old');
SELECT dolt_branch('-D', 'needs_ltree');
DROP SCHEMA mv CASCADE;
SELECT length(dolt_commit('-Am', 'mv gone')::text) AS committed;
DROP SCHEMA mv_util CASCADE;
