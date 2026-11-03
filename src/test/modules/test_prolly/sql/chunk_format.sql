-- A chunk index records the format of the chunks it names as it is made
-- (chunk_format), keeps it through TRUNCATE, and the store reads its
-- chunks by it, whatever a session sets later; the history store, made by
-- initdb, records the engine's.
CREATE EXTENSION IF NOT EXISTS test_prolly;
SET chunk_free_in_background = off;
SELECT pg_chunk_index_format('pg_history.chunk_index') AS history_store;
CREATE FUNCTION fmt_h(d bytea) RETURNS bytea LANGUAGE sql IMMUTABLE
	AS 'SELECT substr(sha256(d), 1, 20)';

-- made under the test format, whose 'R' chunks name others
SELECT test_chunk_format_refs(true);
SHOW chunk_format;
CREATE TABLE fmt_log () USING prolly_chunks;
CREATE TABLE fmt_idx () USING prolly_chunk_index;
SELECT pg_chunk_index_format('fmt_idx') AS made;
-- a chunk naming one not there, refused; under the leaf format too, the
-- index's own read it
SELECT test_chunk_put('fmt_log', 'fmt_idx', fmt_h('\x5201' || fmt_h('\x01')), '\x5201' || fmt_h('\x01'));
SELECT test_chunk_format_refs(false);
SHOW chunk_format;
SELECT test_chunk_put('fmt_log', 'fmt_idx', fmt_h('\x5201' || fmt_h('\x01')), '\x5201' || fmt_h('\x01'));
-- kept through TRUNCATE
TRUNCATE fmt_log, fmt_idx;
SELECT pg_chunk_index_format('fmt_idx') AS truncated;
SELECT test_chunk_put('fmt_log', 'fmt_idx', fmt_h('\x5201' || fmt_h('\x01')), '\x5201' || fmt_h('\x01'));
-- an index made under the leaf format reads the same bytes as naming none
CREATE TABLE fmt_log2 () USING prolly_chunks;
CREATE TABLE fmt_idx2 () USING prolly_chunk_index;
SELECT pg_chunk_index_format('fmt_idx2') AS made_leaf;
SELECT test_chunk_put('fmt_log2', 'fmt_idx2', fmt_h('\x5201' || fmt_h('\x01')), '\x5201' || fmt_h('\x01'));
-- made and truncated in one transaction (its storage made again in place):
-- the same
BEGIN;
SELECT test_chunk_format_refs(true);
CREATE TABLE fmt_log4 () USING prolly_chunks;
CREATE TABLE fmt_idx4 () USING prolly_chunk_index;
TRUNCATE fmt_log4, fmt_idx4;
SELECT pg_chunk_index_format('fmt_idx4') AS made_and_truncated;
SELECT test_chunk_format_refs(false);
COMMIT;
-- a session with no module loaded, whose format the index names, reads its
-- name, truncates it, and dumps it (pg_chunk_index_format): none reads a
-- chunk; one reading a chunk's references needs the format
\c
SET chunk_free_in_background = off;
SELECT pg_chunk_index_format('fmt_idx') AS named_without_module;
TRUNCATE fmt_log, fmt_idx;
SELECT pg_chunk_index_format('fmt_idx') AS truncated_without_module;
VACUUM fmt_idx;
-- a format no one registered: refused as an index is made, not before
SET chunk_format = 'nosuch';
CREATE TABLE fmt_idx3 () USING prolly_chunk_index;
SET chunk_format = '';
SET chunk_format = 'a_name_longer_than_sixteen';
RESET chunk_format;

DROP TABLE fmt_log, fmt_idx, fmt_log2, fmt_idx2, fmt_log4, fmt_idx4;
DROP FUNCTION fmt_h(bytea);
