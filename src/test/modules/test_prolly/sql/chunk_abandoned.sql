-- A backend's region of a chunk log, let go of by VACUUM once the backend
-- is gone: its last page, part written, made full, its rest dead; its
-- pages never written made all dead, free.  A region a live backend holds
-- stays as it is.
CREATE EXTENSION IF NOT EXISTS test_prolly;
SET chunk_free_in_background = off;
CREATE TABLE al () USING prolly_chunks;
CREATE TABLE ai () USING prolly_chunk_index;
CREATE FUNCTION ah(d bytea) RETURNS bytea LANGUAGE sql IMMUTABLE
	AS 'SELECT substr(sha256(d), 1, 20)';
CREATE FUNCTION ablob(tag text) RETURNS bytea LANGUAGE sql IMMUTABLE
	AS $$SELECT convert_to(rpad(tag, 2000, '.'), 'UTF8')$$;

-- ten chunks, five pages of a region of 32
SELECT count(*) FILTER (WHERE test_chunk_put('al', 'ai', ah(ablob('a' || g)), ablob('a' || g))) AS put
FROM generate_series(1, 10) g;
SELECT pages, full_pages, dead FROM test_chunk_log_space('al');

-- VACUUM while this backend lives: its region as it is
VACUUM ai;
SELECT pages, full_pages, dead FROM test_chunk_log_space('al');

-- a backend gone: its region's last page full, its rest dead
\c
SET chunk_free_in_background = off;
VACUUM ai;
SELECT pages, full_pages, dead > 0 AS rest_dead FROM test_chunk_log_space('al');
SELECT ok, chunks FROM test_chunk_check('al', 'ai');

-- this backend's region in a second log, pages past its last written made
-- (by a later region's chunk, a backend that then exits), let go of: the
-- region's pages never written made all dead, free
CREATE TABLE al2 () USING prolly_chunks;
CREATE TABLE ai2 () USING prolly_chunk_index;
SELECT test_chunk_put('al2', 'ai2', ah(ablob('x')), ablob('x')) AS put;
\c
SET chunk_free_in_background = off;
-- (a chunk longer than what is left of a region: a region of its own, the
-- first's never-written pages before it made)
SELECT test_chunk_put('al2', 'ai2', ah(repeat('y', 40 * 8192)::bytea), repeat('y', 40 * 8192)::bytea) AS put;
\c
SET chunk_free_in_background = off;
SELECT pages, free_pages FROM test_chunk_log_space('al2');
VACUUM ai2;
SELECT pages, free_pages AS never_written_free FROM test_chunk_log_space('al2');
SELECT ok, chunks FROM test_chunk_check('al2', 'ai2');

-- a region forgotten, its relcache entry invalidated (a GRANT), let go of
-- as the transaction ends; the next put in another region, the first
-- then VACUUM's to let go of
CREATE TABLE al3 () USING prolly_chunks;
CREATE TABLE ai3 () USING prolly_chunk_index;
SELECT test_chunk_put('al3', 'ai3', ah(ablob('f1')), ablob('f1')) AS put;
SELECT test_chunk_log_regions('al3') AS regions;
GRANT SELECT ON al3 TO PUBLIC;
SELECT test_chunk_log_regions('al3') AS regions_forgotten;
SELECT test_chunk_put('al3', 'ai3', ah(ablob('f2')), ablob('f2')) AS put;
SELECT pages, full_pages FROM test_chunk_log_space('al3');
VACUUM ai3;
SELECT pages, full_pages AS first_region_let_go FROM test_chunk_log_space('al3');
SELECT ok, chunks FROM test_chunk_check('al3', 'ai3');

-- a backend with regions in more logs than it has room for: the one used
-- longest ago let go of, never appended to again, VACUUM's to let go of
CREATE FUNCTION make_logs(n int) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	FOR i IN 1 .. n LOOP
		EXECUTE format('CREATE TABLE ol%s () USING prolly_chunks', i);
		EXECUTE format('CREATE TABLE oi%s () USING prolly_chunk_index', i);
	END LOOP;
END $$;
SELECT make_logs(9);
SELECT count(*) FILTER (WHERE test_chunk_put(('ol' || i)::regclass, ('oi' || i)::regclass,
	ah(ablob('o' || i)), ablob('o' || i))) AS put FROM generate_series(1, 9) i;
SELECT test_chunk_log_regions('ol1') AS first_let_go, test_chunk_log_regions('ol9') AS last_held;
SELECT test_chunk_put('ol1', 'oi1', ah(ablob('o1b')), ablob('o1b')) AS put_again;
SELECT pages, full_pages FROM test_chunk_log_space('ol1');
VACUUM oi1;
SELECT pages, full_pages AS old_region_let_go FROM test_chunk_log_space('ol1');
SELECT ok, chunks FROM test_chunk_check('ol1', 'oi1');

DROP TABLE al, ai, al2, ai2, al3, ai3;
DO $$BEGIN FOR i IN 1 .. 9 LOOP EXECUTE format('DROP TABLE ol%s, oi%s', i, i); END LOOP; END $$;
DROP FUNCTION make_logs(int);
DROP FUNCTION ah(bytea), ablob(text);
