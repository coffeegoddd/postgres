-- A fold describes definitions (defaults, checks, generated columns,
-- index expressions, views, policies) with the settings values' text
-- forms are written under fixed: TimeZone UTC, ISO dates, the postgres
-- IntervalStyle, the shortest floats, hex bytea.  A constant in one prints
-- alike in any session, so the same definitions committed in sessions of
-- other settings make the same root, and a fold in one session agrees
-- with one in another (a reset, a status).

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;

-- The definitions, made again in each session.
CREATE FUNCTION ds_make() RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	CREATE SCHEMA ds VERSIONED;
	CREATE TABLE ds.t (
		id int PRIMARY KEY,
		d date DEFAULT date '2024-01-13' CHECK (d > date '2000-02-01'),
		ts timestamptz DEFAULT timestamptz '2024-01-13 09:30:00+00',
		iv interval DEFAULT interval '1 day 02:03:04',
		f float8 DEFAULT 0.30000000000000004::float8,
		b bytea DEFAULT '\x0102'::bytea,
		later date GENERATED ALWAYS AS (d + interval '1 year 2 days') STORED);
	CREATE INDEX t_after ON ds.t ((ts > timestamptz '2024-02-01 00:00:00+00'));
	CREATE VIEW ds.v AS
		SELECT id, d = date '2024-01-13' AS on_day,
			   iv + interval '3 hours' AS longer
		FROM ds.t WHERE ts < timestamptz '2025-06-01 12:00:00+00';
	ALTER TABLE ds.t ENABLE ROW LEVEL SECURITY;
	CREATE POLICY recent ON ds.t USING (d >= date '2024-01-01');
	INSERT INTO ds.t (id) VALUES (1);
END $$;

SET TimeZone = 'UTC';
SET DateStyle = 'ISO, MDY';
SET IntervalStyle = postgres;
SET extra_float_digits = 1;
SET bytea_output = hex;
SELECT ds_make();
SELECT length(dolt_commit('-Am', 'ds fixed')::text) AS committed;
SELECT prolly_root_hash('HEAD') AS fixed_root \gset
DROP SCHEMA ds CASCADE;
SELECT length(dolt_commit('-Am', 'ds gone')::text) AS committed;

-- Other sessions: the same root.  Each reset back in the first session's
-- settings agrees with the fold its commit took.
SET TimeZone = 'Asia/Tokyo';
SET DateStyle = 'SQL, DMY';
SET IntervalStyle = sql_standard;
SET extra_float_digits = 0;
SET bytea_output = escape;
SELECT ds_make();
SELECT length(dolt_commit('-Am', 'ds other')::text) AS committed;
SELECT prolly_root_hash('HEAD') = :'fixed_root' AS same_root;
RESET ALL;
SELECT dolt_reset('--hard');
SELECT count(*) AS changes FROM dolt_status();
SET TimeZone = 'Asia/Tokyo';
SET DateStyle = 'SQL, DMY';
SET IntervalStyle = sql_standard;
SET extra_float_digits = 0;
SET bytea_output = escape;
DROP SCHEMA ds CASCADE;
SELECT length(dolt_commit('-Am', 'ds gone')::text) AS committed;

SET TimeZone = 'America/New_York';
SET DateStyle = 'German, DMY';
SET IntervalStyle = iso_8601;
SET extra_float_digits = -3;
SELECT ds_make();
SELECT length(dolt_commit('-Am', 'ds another')::text) AS committed;
SELECT prolly_root_hash('HEAD') = :'fixed_root' AS same_root;
SET TimeZone = 'UTC';
SET DateStyle = 'Postgres, MDY';
SET IntervalStyle = postgres_verbose;
SET extra_float_digits = 1;
SELECT dolt_reset('--hard');
SELECT count(*) AS changes FROM dolt_status();
-- (the session's own settings show the definitions as it would)
SELECT pg_get_expr(adbin, adrelid) FROM pg_attrdef
WHERE adrelid = 'ds.t'::regclass AND adnum = 2;
-- (and the rows are as made)
SELECT id, d, iv, f FROM ds.t;

RESET ALL;
DROP SCHEMA ds CASCADE;
DROP FUNCTION ds_make();
SELECT length(dolt_commit('-Am', 'ds gone')::text) AS committed;
