-- A merge converts the values it reads across a column's change of type
-- (by the column's cast, or as the value's text form) under fixed
-- settings: TimeZone UTC, ISO dates, the postgres IntervalStyle, the C
-- locale's money, numbers and times, and the rest canonical text forms are
-- written under.  So the same merge makes the same rows and the same
-- commit whatever the merging session's settings.  (ALTER TABLE ... TYPE
-- converts under the session's own settings, so a side's own ALTER can
-- write a value the merge wouldn't.)

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;

SET TimeZone = 'UTC';
SET DateStyle = 'ISO, MDY';
SET IntervalStyle = postgres;
SET lc_monetary = 'C';
CREATE SCHEMA mc VERSIONED;
CREATE TABLE mc.t (id int PRIMARY KEY, ts timestamptz, m money, d date,
				   iv interval, n numeric);
INSERT INTO mc.t
SELECT g, '2024-03-01 12:00:00+00'::timestamptz + g * interval '1 hour',
	   g * 1.25, date '2024-03-01' + g, g * interval '90 minutes', g * 1.5
FROM generate_series(1, 4) g;
SELECT length(dolt_commit('-Am', 'base')::text) AS committed;
SELECT dolt_branch('mc_f');

-- Both sides make every column text (under the fixed settings), and each
-- changes a row of its own; both change row 4's ts.
ALTER TABLE mc.t ALTER COLUMN ts TYPE text, ALTER COLUMN m TYPE text,
	ALTER COLUMN d TYPE text, ALTER COLUMN iv TYPE text, ALTER COLUMN n TYPE text;
UPDATE mc.t SET ts = 'ours', m = 'ours', d = 'ours', iv = 'ours', n = 'ours'
WHERE id = 1;
SELECT length(dolt_commit('-am', 'main: text')::text) AS committed;
SELECT dolt_checkout('mc_f') IS NOT NULL AS on_f;
ALTER TABLE mc.t ALTER COLUMN ts TYPE text, ALTER COLUMN m TYPE text,
	ALTER COLUMN d TYPE text, ALTER COLUMN iv TYPE text, ALTER COLUMN n TYPE text;
UPDATE mc.t SET ts = 'theirs', m = 'theirs', d = 'theirs', iv = 'theirs',
	n = 'theirs' WHERE id = 2;
SELECT length(dolt_commit('-am', 'mc_f: text')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SELECT dolt_branch('mc_pre');

-- Merged under the fixed settings: base's values, read as text, are each
-- side's where it didn't change them.
SELECT (dolt_merge('mc_f', '--no-commit'))[3:4];
SELECT length(dolt_commit('-am', 'merged', '--date', '2024-01-01 00:00:00+00')::text) AS committed;
SELECT prolly_root_hash('HEAD') AS fixed_root \gset
SELECT dolt_hashof('HEAD') AS fixed_commit \gset
SELECT * FROM mc.t ORDER BY id;

-- The same merge under other settings: the same rows, root and commit.
SELECT dolt_reset('--hard', 'mc_pre');
SET TimeZone = 'Asia/Tokyo';
SET DateStyle = 'SQL, DMY';
SET IntervalStyle = sql_standard;
SET extra_float_digits = 0;
-- (a locale whose money is shown otherwise, where the system has one: not
-- every system does, so with none the money column is only read as the C
-- locale shows it.  lc_numeric changes no number's text form, and isn't
-- set.)
DO $$
DECLARE
	l text;
BEGIN
	FOREACH l IN ARRAY ARRAY['es_ES.utf8', 'es_ES.UTF-8', 'de_DE.utf8',
							 'de_DE.UTF-8', 'fr_FR.utf8', 'fr_FR.UTF-8']
	LOOP
		BEGIN
			PERFORM set_config('lc_monetary', l, false);
			EXIT;
		EXCEPTION WHEN invalid_parameter_value THEN
			NULL;
		END;
	END LOOP;
END $$;
SELECT current_setting('lc_monetary') = 'C' OR
	   1.25::money::text <> '$1.25' AS money_shown_otherwise;
SELECT (dolt_merge('mc_f', '--no-commit'))[3:4];
SELECT length(dolt_commit('-am', 'merged', '--date', '2024-01-01 00:00:00+00')::text) AS committed;
SELECT prolly_root_hash('HEAD') = :'fixed_root' AS same_root,
	   dolt_hashof('HEAD') = :'fixed_commit' AS same_commit;
SELECT * FROM mc.t ORDER BY id;

-- A conflict (both changed row 4's ts) is listed as the merge found it,
-- in a session of other settings too: only row 4.
SELECT dolt_reset('--hard', 'mc_pre');
UPDATE mc.t SET ts = 'ours again' WHERE id = 4;
SELECT length(dolt_commit('-am', 'main: row 4')::text) AS committed;
SELECT dolt_checkout('mc_f') IS NOT NULL AS on_f;
UPDATE mc.t SET ts = 'theirs again' WHERE id = 4;
SELECT length(dolt_commit('-am', 'mc_f: row 4')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
RESET TimeZone;
RESET DateStyle;
RESET IntervalStyle;
RESET lc_monetary;
RESET extra_float_digits;
SET TimeZone = 'UTC';
BEGIN;
SELECT (dolt_merge('mc_f'))[3:4];
SET LOCAL TimeZone = 'Asia/Tokyo';
SET LOCAL DateStyle = 'SQL, DMY';
SELECT (base_row).id, (our_row).ts AS ours, (their_row).ts AS theirs
FROM dolt_conflicts(NULL::mc.t) ORDER BY 1;
ROLLBACK;

RESET ALL;
SELECT dolt_branch('-D', 'mc_f');
SELECT dolt_branch('-D', 'mc_pre');
DROP SCHEMA mc CASCADE;
SELECT length(dolt_commit('-Am', 'mc gone')::text) AS committed;

-- A user's cast that raises an error while the merge converts with the
-- settings fixed: the error is raised as it is, and the session's settings
-- are as they were, at the top level and in a PL/pgSQL block that catches
-- it.  So is a cast the statement's timeout cancels.
SET TimeZone = 'Asia/Tokyo';
SET mc.cast_mode = 'raise';
CREATE FUNCTION public.mc_int_text(i int4) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	IF current_setting('TimeZone') = 'UTC' AND
		current_setting('mc.cast_mode') <> 'pass' THEN
		IF current_setting('mc.cast_mode') = 'sleep' THEN
			PERFORM pg_sleep(30);
		END IF;
		RAISE EXCEPTION 'cast saw TimeZone %', current_setting('TimeZone');
	END IF;
	RETURN i::numeric::text;
END $$;
CREATE CAST (int4 AS text) WITH FUNCTION public.mc_int_text(int4) AS ASSIGNMENT;
CREATE SCHEMA ge VERSIONED;
CREATE TABLE ge.t (id int PRIMARY KEY, a int);
INSERT INTO ge.t SELECT g, g FROM generate_series(1, 4) g;
SELECT length(dolt_commit('-Am', 'ge base')::text) AS committed;
SELECT dolt_branch('ge_f');
ALTER TABLE ge.t ALTER COLUMN a TYPE text;
UPDATE ge.t SET a = 'ours' WHERE id = 1;
SELECT length(dolt_commit('-am', 'ge ours')::text) AS committed;
SELECT dolt_checkout('ge_f') IS NOT NULL AS on_f;
ALTER TABLE ge.t ALTER COLUMN a TYPE text;
UPDATE ge.t SET a = 'theirs' WHERE id = 2;
SELECT length(dolt_commit('-am', 'ge theirs')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
\set VERBOSITY terse
SELECT dolt_merge('ge_f', '--no-commit');
\set VERBOSITY default
SHOW TimeZone;
SELECT now() IS NOT NULL AS session_fine;
DO $$
BEGIN
	PERFORM dolt_merge('ge_f', '--no-commit');
EXCEPTION WHEN raise_exception THEN
	RAISE NOTICE 'caught: %; TimeZone %', SQLERRM, current_setting('TimeZone');
END $$;
SHOW TimeZone;
SELECT now() IS NOT NULL AS session_fine;
BEGIN;
SELECT 1 AS in_block;
SAVEPOINT s;
\set VERBOSITY terse
SELECT dolt_merge('ge_f', '--no-commit');
\set VERBOSITY default
ROLLBACK TO SAVEPOINT s;
SHOW TimeZone;
COMMIT;
SET mc.cast_mode = 'sleep';
SET statement_timeout = '500ms';
\set VERBOSITY terse
SELECT dolt_merge('ge_f', '--no-commit');
\set VERBOSITY default
RESET statement_timeout;
SHOW TimeZone;
SELECT now() IS NOT NULL AS session_fine;
-- (and listing a merge's conflicts, which reads them as the merge did)
SELECT dolt_reset('--hard');
UPDATE ge.t SET a = 'ours 3' WHERE id = 3;
SELECT length(dolt_commit('-am', 'ge ours 3')::text) AS committed;
SELECT dolt_checkout('ge_f') IS NOT NULL AS on_f;
UPDATE ge.t SET a = 'theirs 3' WHERE id = 3;
SELECT length(dolt_commit('-am', 'ge theirs 3')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
BEGIN;
SET LOCAL mc.cast_mode = 'pass';
SELECT (dolt_merge('ge_f'))[3:4];
SET LOCAL mc.cast_mode = 'raise';
SAVEPOINT s;
\set VERBOSITY terse
SELECT count(*) FROM dolt_conflicts(NULL::ge.t);
\set VERBOSITY default
ROLLBACK TO SAVEPOINT s;
SHOW TimeZone;
SET LOCAL mc.cast_mode = 'pass';
SELECT (base_row).id, (our_row).a AS ours, (their_row).a AS theirs
FROM dolt_conflicts(NULL::ge.t) ORDER BY 1;
ROLLBACK;
SHOW TimeZone;
SELECT dolt_reset('--hard');
DROP CAST (int4 AS text);
DROP FUNCTION public.mc_int_text(int4);
RESET mc.cast_mode;
SET TimeZone = 'UTC';
SELECT dolt_branch('-D', 'ge_f');
DROP SCHEMA ge CASCADE;
SELECT length(dolt_commit('-Am', 'ge gone')::text) AS committed;

-- The merge's own replay of a side's ALTER TABLE ... TYPE ... USING, and
-- the CHECKs it adds, convert with the settings fixed too: a merge that
-- succeeds in a session of one DateStyle succeeds in one of another, and
-- makes the same rows and commit.
SET DateStyle = 'ISO, MDY';
CREATE SCHEMA ma VERSIONED;
CREATE TABLE ma.t (id int PRIMARY KEY, d text, x int);
CREATE TABLE ma.u (id int PRIMARY KEY, y int);
INSERT INTO ma.t SELECT g, '01/1' || g || '/2024', g FROM generate_series(3, 6) g;
INSERT INTO ma.u VALUES (1, 1);
SELECT length(dolt_commit('-Am', 'ma base')::text) AS committed;
SELECT dolt_branch('ma_f');
UPDATE ma.u SET y = 2;
SELECT length(dolt_commit('-am', 'ma ours')::text) AS committed;
SELECT dolt_checkout('ma_f') IS NOT NULL AS on_f;
ALTER TABLE ma.t ALTER COLUMN d TYPE date USING d::date;
ALTER TABLE ma.t ADD CONSTRAINT d_iso CHECK (d::text LIKE '2024-%');
ALTER TABLE ma.t ADD COLUMN s text DEFAULT make_date(2024, 1, 13)::text
	CHECK (s LIKE '2024-%');
UPDATE ma.t SET x = 30 WHERE id = 3;
SELECT length(dolt_commit('-am', 'ma theirs')::text) AS committed;
SELECT dolt_checkout('main') IS NOT NULL AS on_main;
SELECT dolt_branch('ma_pre');
SELECT (dolt_merge('ma_f', '--no-commit'))[3:4];
SELECT length(dolt_commit('-am', 'merged', '--date', '2024-01-01 00:00:00+00')::text) AS committed;
SELECT prolly_root_hash('HEAD') AS ma_root \gset
SELECT dolt_hashof('HEAD') AS ma_commit \gset
SELECT * FROM ma.t ORDER BY id;
SELECT dolt_reset('--hard', 'ma_pre');
SET DateStyle = 'ISO, DMY';
SELECT (dolt_merge('ma_f', '--no-commit'))[3:4];
SELECT length(dolt_commit('-am', 'merged', '--date', '2024-01-01 00:00:00+00')::text) AS committed;
SELECT prolly_root_hash('HEAD') = :'ma_root' AS same_root,
	   dolt_hashof('HEAD') = :'ma_commit' AS same_commit;
SHOW DateStyle;
SELECT dolt_reset('--hard', 'ma_pre');
-- (dates shown as SQL writes them break the CHECKs, read so)
SET DateStyle = 'SQL, MDY';
SELECT (dolt_merge('ma_f', '--no-commit'))[3:4];
SELECT length(dolt_commit('-am', 'merged', '--date', '2024-01-01 00:00:00+00')::text) AS committed;
SELECT prolly_root_hash('HEAD') = :'ma_root' AS same_root,
	   dolt_hashof('HEAD') = :'ma_commit' AS same_commit;
SHOW DateStyle;
RESET DateStyle;
SELECT dolt_reset('--hard', 'ma_pre');
SELECT dolt_branch('-D', 'ma_f');
SELECT dolt_branch('-D', 'ma_pre');
DROP SCHEMA ma CASCADE;
SELECT length(dolt_commit('-Am', 'ma gone')::text) AS committed;
