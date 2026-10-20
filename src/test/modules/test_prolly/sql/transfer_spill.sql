-- What a transfer has seen is held to maintenance_work_mem: past it, its
-- maps go to a temporary file as sorted runs, each with a filter in
-- memory, read back as needed (and changed in place).
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;
SELECT current_database() AS db \gset

-- The map alone: in memory, then across runs.
SELECT test_seen_check(1000, 1024 * 1024) AS runs_in_memory;
SELECT test_seen_check(20000, 64 * 1024) > 1 AS spilled;

\getenv abs_builddir PG_ABS_BUILDDIR
SELECT 'file://' || :'abs_builddir' || '/tmp_check/remotes/spill-' || txid_current() AS url \gset

-- A push and a fetch of some thousands of chunks, in 64 kB: all of them
-- moved.
\c postgres
CREATE DATABASE spill_from;
\c spill_from
CREATE EXTENSION test_prolly;
CREATE SCHEMA sp VERSIONED;
CREATE TABLE sp.t (id int PRIMARY KEY, b bytea STORAGE EXTERNAL);
INSERT INTO sp.t SELECT g, test_noise('row ' || g, 3000) FROM generate_series(1, 3000) g;
SELECT length(dolt_commit('-Am', 'many chunks')::text) AS committed;
-- (the same push to two remotes: with room for its maps in memory, and
-- with 64 kB, its maps written out as runs too)
SELECT dolt_remote('add', 'roomy', :'url' || '-roomy');
SELECT dolt_remote('add', 'origin', :'url');
SELECT pg_stat_force_next_flush();
SELECT temp_bytes AS before FROM pg_stat_database WHERE datname = 'spill_from' \gset
SET maintenance_work_mem = '1GB';
SELECT dolt_push('roomy', 'main') AS pushed;
SELECT pg_stat_force_next_flush();
\c spill_from
SELECT temp_bytes - :before AS roomy_bytes FROM pg_stat_database WHERE datname = 'spill_from' \gset
SELECT temp_bytes AS before FROM pg_stat_database WHERE datname = 'spill_from' \gset
SET maintenance_work_mem = '64kB';
SELECT dolt_push('origin', 'main') AS pushed;
SELECT pg_stat_force_next_flush();
\c spill_from
SELECT temp_bytes - :before > :roomy_bytes AS push_spilled FROM pg_stat_database
WHERE datname = 'spill_from';

\c postgres
CREATE DATABASE spill_into;
\c spill_into
CREATE EXTENSION test_prolly;
CREATE SCHEMA sp VERSIONED;
SELECT length(dolt_commit('-Am', 'empty')::text) AS committed;
SELECT dolt_remote('add', 'origin', :'url');
SET maintenance_work_mem = '64kB';
SELECT dolt_fetch('origin') AS fetched;
SELECT dolt_checkout('-b', 'theirs', 'origin/main') IS NOT NULL AS checked_out;
SELECT count(*), count(*) FILTER (WHERE b = test_noise('row ' || id, 3000)) AS whole
FROM sp.t;

-- Served as a libpq remote serves it, in 64 kB: what was sent written out
-- as runs, and its file closed as the call ends (no resource left open).
\c spill_from
SET maintenance_work_mem = '64kB';
SELECT count(*) > 3000 AS sent_since
FROM dolt_serve_since(ARRAY[decode(dolt_hashof('main'), 'hex')], '{}');
RESET maintenance_work_mem;

\c postgres
DROP DATABASE spill_from;
DROP DATABASE spill_into;
\c :db
