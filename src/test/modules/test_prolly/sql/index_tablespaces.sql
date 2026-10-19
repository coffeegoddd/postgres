-- Tablespaces are local to a cluster: neither a table's nor its indexes'
-- reach the canonical form, so the same tables hash the same wherever their
-- storage is, and a clone into a cluster without the tablespace (or a move
-- that makes an index again) needs none.

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

-- and back, the table with them: still nothing
ALTER INDEX tsp.t_b SET TABLESPACE regress_prolly_ts;
ALTER TABLE tsp.t SET TABLESPACE regress_prolly_ts;
SELECT * FROM dolt_status;

-- a move makes the indexes from definitions without a tablespace
SELECT dolt_branch('tsp_side');
SELECT dolt_checkout('tsp_side');
DROP INDEX tsp.t_b;
SELECT length(dolt_commit('-Am', 'no t_b')::text) AS committed;
SELECT dolt_checkout('main');
SELECT dolt_reset('--hard', 'tsp_side');
SELECT dolt_reset('--hard', 'HEAD~1');
SELECT indexname, tablespace FROM pg_indexes WHERE schemaname = 'tsp'
  ORDER BY indexname COLLATE "C";
SELECT * FROM dolt_status;

SELECT dolt_branch('-D', 'tsp_side');
DROP SCHEMA tsp CASCADE;
SELECT length(dolt_commit('-Am', 'tsp gone')::text) AS committed;
DROP TABLESPACE regress_prolly_ts;
