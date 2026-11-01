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

DROP TABLE al, ai, al2, ai2;
DROP FUNCTION ah(bytea), ablob(text);
