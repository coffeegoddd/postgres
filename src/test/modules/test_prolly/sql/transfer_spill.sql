-- What a transfer has seen is held to maintenance_work_mem: past it, its
-- maps go to temporary files as sorted runs, each with a filter in
-- memory, read back as needed (and changed in place), the runs merged.
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;
SELECT current_database() AS db \gset

-- The map alone: in memory, then across runs.
SELECT test_seen_check(1000, 1024 * 1024) AS runs_in_memory;
SELECT test_seen_check(20000, 64 * 1024) > 1 AS spilled;

-- Checked against a map in memory as it grows (hashes never entered looked
-- up, spilled ones entered again and changed), in 64 kB: its memory holds
-- to the budget, its filters to their half, however many runs it writes;
-- its runs merge four of a level into one, so a lookup checks at most
-- three filters a level, a level for each base-4 digit of the runs
-- written.
SELECT n, runs_written > n / 400 AS many_runs,
       memory_peak <= 65536 AS within_budget,
       filter_bytes_peak <= filter_budget AS filters_within,
       runs_peak <= 3 * (floor(log(4, runs_written)) + 1) AS runs_logarithmic,
       max_filter_checks <= runs_peak AS checks_bounded,
       merges > 0 AS merged
FROM unnest(ARRAY[5000, 20000, 80000]) n, test_seen_measure(n, 64 * 1024);
-- (with room for its filters, they rule out nearly every run not holding
-- what is looked up)
SELECT false_positives::float8 /
       (filter_checks - (run_searches - false_positives)) < 0.02 AS filters_work,
       memory_peak <= 1024 * 1024 AS within_budget
FROM test_seen_measure(80000, 1024 * 1024);

-- A cancel lands in the map's work (writing or merging runs), and its
-- files go as the statement fails.
SET statement_timeout = '300ms';
SELECT test_seen_measure(100000000, 64 * 1024);
RESET statement_timeout;
SELECT count(*) AS files_left FROM pg_ls_tmpdir()
WHERE name LIKE 'pgsql_tmp' || pg_backend_pid() || '.%';

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
SELECT dolt_hashof('main') AS from_main \gset
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
SELECT dolt_hashof('origin/main') = :'from_main' AS same_root;
SELECT dolt_checkout('-b', 'theirs', 'origin/main') IS NOT NULL AS checked_out;
SELECT count(*), count(*) FILTER (WHERE b = test_noise('row ' || id, 3000)) AS whole
FROM sp.t;

-- A clone of it in 64 kB, its maps written out as runs: the same commit.
\c postgres
CREATE DATABASE spill_clone;
\c spill_clone
SET maintenance_work_mem = '64kB';
SELECT dolt_clone(:'url') IS NOT NULL AS cloned;
SELECT dolt_hashof('main') = :'from_main' AS same_root;
SELECT count(*), count(*) FILTER (WHERE length(b) = 3000) AS whole FROM sp.t;
SELECT pg_stat_force_next_flush();
\c spill_clone
SELECT temp_bytes > 0 AS clone_spilled FROM pg_stat_database
WHERE datname = 'spill_clone';

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
DROP DATABASE spill_clone;
\c :db
