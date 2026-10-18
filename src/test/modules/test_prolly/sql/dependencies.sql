-- What versioned objects need from outside the versioned schemas, recorded
-- at each commit as "d:" items of the folded root: an extension (with its
-- version) for its members, an object of an ordinary schema.  Not what
-- initdb made (built-in types and functions), and not roles: owners and
-- grants aren't versioned, so a role isn't a dependency (a grant changes
-- nothing below).

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;

CREATE FUNCTION pg_temp.deps() RETURNS TABLE (key text, value text)
  LANGUAGE sql AS $$
	SELECT key, value FROM test_fold_items() WHERE key LIKE 'd:%' ORDER BY key
  $$;

CREATE ROLE regress_dep_reader;
CREATE EXTENSION citext VERSION '1.6';
CREATE SCHEMA dep_util;
CREATE TYPE dep_util.mood AS ENUM ('ok', 'meh');
CREATE FUNCTION dep_util.twice(int) RETURNS int LANGUAGE sql AS 'SELECT $1 * 2';

CREATE SCHEMA dep VERSIONED;
CREATE TABLE dep.plain (id int PRIMARY KEY, n numeric, t text);
SELECT * FROM pg_temp.deps();			-- nothing outside (built-ins only)

CREATE TABLE dep.t (id int PRIMARY KEY, m dep_util.mood,
					c citext DEFAULT dep_util.twice(1)::text);
GRANT SELECT ON dep.t TO regress_dep_reader;
SELECT * FROM pg_temp.deps();
SELECT length(dolt_commit('-Am', 'deps')::text) AS committed;

-- described afresh at each fold: an update, a rename outside
ALTER EXTENSION citext UPDATE TO '1.8';
ALTER FUNCTION dep_util.twice(int) RENAME TO double_it;
SELECT * FROM pg_temp.deps();
SELECT * FROM dolt_status ORDER BY table_name;	-- shown as changes
SELECT length(dolt_commit('-Am', 'deps changed')::text) AS committed;

-- a merge of branches whose dependencies differ: no schema conflict, and
-- the merged commit's are described afresh
SELECT dolt_branch('dep_side');
REVOKE SELECT ON dep.t FROM regress_dep_reader;	-- (not a dependency)
ALTER TABLE dep.t ALTER COLUMN c DROP DEFAULT;	-- the function no longer needed
SELECT length(dolt_commit('-Am', 'no default')::text) AS committed;
SELECT dolt_checkout('dep_side');
CREATE TABLE dep.u (id int PRIMARY KEY, m dep_util.mood);
SELECT length(dolt_commit('-Am', 'u')::text) AS committed;
SELECT dolt_checkout('main');
SELECT dolt_merge('dep_side') IS NOT NULL AS merged;
SELECT * FROM pg_temp.deps();
SELECT dolt_checkout('dep_side');
SELECT * FROM pg_temp.deps();
SELECT dolt_checkout('main');

DROP SCHEMA dep CASCADE;
SELECT length(dolt_commit('-Am', 'dep gone')::text) AS committed;
SELECT dolt_branch('-D', 'dep_side');
DROP SCHEMA dep_util CASCADE;
DROP EXTENSION citext;
DROP ROLE regress_dep_reader;
