-- Tablespaces are local to a cluster: neither a table's nor its indexes'
-- reach the canonical form, so the same tables hash the same wherever their
-- storage is.  A move that makes an index again keeps it where the branch
-- had it; one the branch didn't have is made in the default tablespace.
-- (A clone into a cluster without the tablespace: t/101_index_tablespaces.pl.)

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;

SET allow_in_place_tablespaces = on;
CREATE TABLESPACE regress_prolly_ts LOCATION '';

CREATE SCHEMA tsp VERSIONED;
CREATE TABLE tsp.t (id int PRIMARY KEY USING INDEX TABLESPACE regress_prolly_ts,
	a int, b int, UNIQUE (a) USING INDEX TABLESPACE regress_prolly_ts);
CREATE INDEX t_b ON tsp.t (b) TABLESPACE regress_prolly_ts;
INSERT INTO tsp.t SELECT g, g, g FROM generate_series(1, 10) g;
SELECT length(dolt_commit('-Am', 'in a tablespace')::text) AS committed;

-- the indexes moved to the default tablespace: nothing changed
ALTER INDEX tsp.t_pkey SET TABLESPACE pg_default;
ALTER INDEX tsp.t_a_key SET TABLESPACE pg_default;
ALTER INDEX tsp.t_b SET TABLESPACE pg_default;
SELECT * FROM dolt_status;

-- and back, and the table too (which moves none of its indexes): still
-- nothing
ALTER INDEX tsp.t_pkey SET TABLESPACE regress_prolly_ts;
ALTER INDEX tsp.t_a_key SET TABLESPACE regress_prolly_ts;
ALTER INDEX tsp.t_b SET TABLESPACE regress_prolly_ts;
ALTER TABLE tsp.t SET TABLESPACE regress_prolly_ts;
SELECT * FROM dolt_status;

-- a move that makes the indexes again (their definitions changed, a
-- constraint's among them) keeps them where they were
SELECT dolt_branch('tsp_side');
SELECT dolt_checkout('tsp_side');
ALTER INDEX tsp.t_pkey SET TABLESPACE pg_default;
ALTER INDEX tsp.t_a_key SET TABLESPACE pg_default;
ALTER INDEX tsp.t_b SET TABLESPACE pg_default;
ALTER TABLE tsp.t DROP CONSTRAINT t_a_key, ADD CONSTRAINT t_a_key UNIQUE (a, b);
DROP INDEX tsp.t_b;
CREATE INDEX t_b ON tsp.t (b, a);
SELECT length(dolt_commit('-Am', 'redefined')::text) AS committed;
SELECT dolt_checkout('main');
SELECT dolt_reset('--hard', 'tsp_side');
SELECT indexname, tablespace, indexdef FROM pg_indexes WHERE schemaname = 'tsp'
  ORDER BY indexname COLLATE "C";
SELECT * FROM dolt_status;
SELECT dolt_reset('--hard', 'HEAD~1');
SELECT indexname, tablespace FROM pg_indexes WHERE schemaname = 'tsp'
  ORDER BY indexname COLLATE "C";

-- an index the branch didn't have is made in the default tablespace
SELECT dolt_checkout('tsp_side');
DROP INDEX tsp.t_b;
SELECT length(dolt_commit('-Am', 'no t_b')::text) AS committed;
SELECT dolt_checkout('main');
SELECT dolt_reset('--hard', 'tsp_side');
SELECT dolt_reset('--hard', 'HEAD~2');
SELECT indexname, tablespace FROM pg_indexes WHERE schemaname = 'tsp'
  ORDER BY indexname COLLATE "C";
SELECT * FROM dolt_status;

-- a commit of format 6, whose index lines had the TABLESPACE clause, isn't
-- moved to (a fold of this format no longer matches it; nor is it merged)
SELECT test_commit_of(test_root_with_format(prolly_root_hash('HEAD'), '6'),
	'format 6') AS tsp_old \gset
SELECT dolt_reset('--hard', :'tsp_old');

-- a user without CREATE on the tablespace: the move goes on, and the index
-- it makes again stays in the default tablespace, with a warning
CREATE ROLE regress_tsp_owner IN ROLE pg_prolly_write;
CREATE SCHEMA tso VERSIONED;
GRANT ALL ON SCHEMA tso TO regress_tsp_owner;
SET ROLE regress_tsp_owner;
CREATE TABLE tso.t (id int PRIMARY KEY, b int);
CREATE INDEX t_b ON tso.t (b);
SELECT length(dolt_commit('-Am', 'tso')::text) AS committed;
RESET ROLE;
SELECT dolt_branch('tso_side');
SET ROLE regress_tsp_owner;
DROP INDEX tso.t_b;
CREATE INDEX t_b ON tso.t (b, id);
SELECT length(dolt_commit('-Am', 'tso redefined')::text) AS committed;
RESET ROLE;
ALTER INDEX tso.t_b SET TABLESPACE regress_prolly_ts;
SET ROLE regress_tsp_owner;
SELECT dolt_reset('--hard', 'HEAD~1');
SELECT indexname, tablespace, indexdef FROM pg_indexes WHERE schemaname = 'tso'
  ORDER BY indexname COLLATE "C";
SELECT * FROM dolt_status;
-- (a merge, likewise)
SELECT dolt_checkout('tso_side');
DROP INDEX tso.t_b;
CREATE INDEX t_b ON tso.t (id, b);
SELECT length(dolt_commit('-Am', 'tso side')::text) AS committed;
SELECT dolt_checkout('main');
RESET ROLE;
ALTER INDEX tso.t_b SET TABLESPACE regress_prolly_ts;
SET ROLE regress_tsp_owner;
SELECT (dolt_merge('tso_side'))[2:3] IS NOT NULL AS merged;
SELECT indexname, tablespace, indexdef FROM pg_indexes WHERE schemaname = 'tso'
  ORDER BY indexname COLLATE "C";
RESET ROLE;
SELECT dolt_branch('-D', 'tso_side');
DROP SCHEMA tso CASCADE;
DROP ROLE regress_tsp_owner;

SELECT dolt_branch('-D', 'tsp_side');
DROP SCHEMA tsp CASCADE;
SELECT length(dolt_commit('-Am', 'tsp gone')::text) AS committed;
DROP TABLESPACE regress_prolly_ts;
