-- The history window's retires.  In archive mode no retire takes a
-- generation newer than the store's as the newest fragment archived, and
-- found whole there, was severed: while a sever's archive is unconfirmed
-- (it failed, so nothing was recorded), what came since stays, however
-- garbage it is thought; once a sever archives, it may go.  A fragment
-- made asks the background for a retire, and a batch boundary passed
-- since the newest generation ended asks for a seal (both the
-- background's, in t/607_gen_window_auto.pl).
\getenv abs_builddir PG_ABS_BUILDDIR
\set dir :abs_builddir '/tmp_check/generations_window'
SELECT 'file://' || :'dir' || '/' || txid_current() AS url \gset
SELECT 'liar://' || :'dir' || '/' || txid_current() || '_liar' AS liar_url \gset
\set VERBOSITY terse
CREATE DATABASE generations_window_db;
\c generations_window_db
CREATE EXTENSION test_prolly;
CREATE FUNCTION gw_err(q text) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE q;
	RETURN 'no error';
EXCEPTION WHEN OTHERS THEN
	RETURN regexp_replace(regexp_replace(SQLERRM, '[0-9a-f]{40}', '<hash>', 'g'),
						  '(file|liar)://[^ ]*', '<url>', 'g');
END $$;
CREATE SCHEMA gw VERSIONED;
CREATE TABLE gw.t (id int PRIMARY KEY, v text);
CREATE FUNCTION gw_commit(msg text, ago interval) RETURNS boolean LANGUAGE sql AS $$
	SELECT length(dolt_commit('-Am', msg, '--date', (now() - ago)::text)::text) > 0
$$;
INSERT INTO gw.t SELECT g, 'v' || g FROM generate_series(1, 2000) g;
SELECT gw_commit('w1', '10 days') AS committed;
SELECT kept >= 0 AS collected FROM test_chunk_gc(false);
UPDATE gw.t SET v = 'w2' WHERE id % 2 = 0;
SELECT gw_commit('w2', '8 days') AS committed;
SELECT kept >= 0 AS collected FROM test_chunk_gc(false);
UPDATE gw.t SET v = 'w3' WHERE id % 2 = 1;
SELECT gw_commit('w3', '1 day') AS committed;
SELECT kept >= 0 AS collected FROM test_chunk_gc(false);

-- No window yet: no limit, nothing due.
SELECT * FROM test_window_retire_due();

-- The first sever archives: a retire may take what was settled by then.
ALTER DATABASE generations_window_db SET prolly.history_archive = :'url';
ALTER DATABASE generations_window_db SET prolly.history_window = '3 days';
\c generations_window_db
SELECT test_chunk_generation() AS g1 \gset
SELECT action, commits FROM dolt_history_sever();
SELECT max_hi = :g1 AS limit_at_first, max_hi AS limit
	FROM test_window_retire_due() \gset
SELECT :'limit_at_first' AS limit_at_first;
SELECT lo AS lo, hi AS hi FROM test_chunk_retire_choose(NULL, :limit) \gset
SELECT freed > 0 AS freed FROM test_chunk_retire(:lo, :hi);

-- What came since, settled in newer generations and thought all garbage:
-- with the next sever's archive failing, no retire takes it.
UPDATE gw.t SET v = 'w4' WHERE id <= 10;
SELECT gw_commit('w4', '0 days') AS committed;
SELECT kept >= 0 AS collected FROM test_chunk_gc(false);
SELECT test_chunk_generation() AS g_new \gset
SELECT test_chunk_note_garbage(array(SELECT test_chunk_file_hashes(name)))
	> 0 AS noted
	FROM test_chunk_generations() WHERE epoch_lo = :g_new;
SELECT lo = :g_new AS new_due FROM test_chunk_retire_choose();
ALTER DATABASE generations_window_db SET prolly.history_window = '12 hours';
ALTER DATABASE generations_window_db SET prolly.history_archive = :'liar_url';
\c generations_window_db
LOAD 'test_prolly';
SELECT gw_err('SELECT * FROM dolt_history_sever()') AS unconfirmed;
SELECT max_hi = :g1 AS limit_kept FROM test_window_retire_due();
SELECT count(*) AS chosen
	FROM test_window_retire_due() d, test_chunk_retire_choose(NULL, d.max_hi) c
	WHERE c.lo IS NOT NULL;

-- Archived, found whole: the limit moves past it, and it may be retired.
ALTER DATABASE generations_window_db SET prolly.history_archive = :'url';
\c generations_window_db
SELECT action, commits FROM dolt_history_sever();
SELECT max_hi >= :g_new AS limit_past_it FROM test_window_retire_due();
SELECT c.lo <= :g_new AND c.hi >= :g_new AS chosen_now
	FROM test_window_retire_due() d, test_chunk_retire_choose(NULL, d.max_hi) c;
SELECT test_chunk_verify_generations() > 0 AS generations;

\c postgres
DROP DATABASE generations_window_db;
