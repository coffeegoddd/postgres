-- Chunks move as stored: framed (a codec, the chunk's length, its bytes as
-- the codec left them), sent as the store holds them, compressed where they
-- were made, added as received; only the receiver's checks decode them.
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;
SELECT value AS fmt FROM test_fold_items() WHERE key = '!:format' \gset
SELECT current_database() AS db \gset

-- A frame: what compresses, compressed; what doesn't, as it is.
SELECT get_byte(test_chunk_frame(convert_to(repeat('a', 1000), 'UTF8'), true), 0)
	AS repeated_codec,
	get_byte(test_chunk_frame(test_noise('n', 1000), true), 0) AS noise_codec;

\getenv abs_builddir PG_ABS_BUILDDIR
\set dir :abs_builddir '/tmp_check/stored'
SELECT 'file://' || :'dir' || '/' || txid_current() AS run \gset
\set url_lz4 :run '/lz4'
\set url_plain :run '/plain'
\set url_back :run '/back'

-- A commit whose chunks compress: a long message, rows alike (in a
-- database of its own, its commit the only one).
\c postgres
CREATE DATABASE stored_from;
\c stored_from
CREATE EXTENSION test_prolly;
CREATE SCHEMA sc VERSIONED;
CREATE TABLE sc.t (id int PRIMARY KEY, v text);
INSERT INTO sc.t SELECT g, repeat('row ', 20) || g FROM generate_series(1, 300) g;
SELECT length(dolt_commit('-Am', repeat('a message that repeats ', 40))::text) AS committed;
SELECT dolt_hashof('HEAD') AS head \gset

-- Pushed: the remote holds each chunk as this store does.
SELECT dolt_remote('add', 'lz4', :'url_lz4');
SELECT dolt_push('lz4', 'main');
SELECT count(*) AS chunks,
	count(*) FILTER (WHERE test_remote_get_stored(:'url_lz4', h) = test_chunk_stored(h))
	AS as_stored,
	count(*) FILTER (WHERE get_byte(test_chunk_stored(h), 0) = 1) > 0 AS some_compressed
FROM test_commit_chunks(:'head') h;

-- A remote holding the same chunks uncompressed, as a server built
-- without LZ4 would: fetched, they stay so (encoded again, the receiver
-- would compress them), and they still read.
SELECT count(test_remote_put_stored(:'url_plain', h, test_chunk_frame(test_chunk_get(h), false)))
	AS put
FROM test_commit_chunks(:'head') h;
SELECT test_remote_set_head(:'url_plain', 'main', :'head', :'fmt');
\c postgres
CREATE DATABASE stored_into;
\c stored_into
CREATE EXTENSION test_prolly;
SELECT dolt_remote('add', 'plain', :'url_plain');
SELECT dolt_fetch('plain');
SELECT count(*) AS chunks,
	count(*) FILTER (WHERE get_byte(test_chunk_stored(h), 0) = 0) AS uncompressed,
	count(*) FILTER (WHERE get_byte(test_chunk_frame(test_chunk_get(h), true), 0) = 1) > 0
	AS would_compress
FROM test_commit_chunks(:'head') h;
SELECT dolt_checkout('-b', 'from_plain', 'plain/main');
SELECT count(*), sum(length(v)) FROM sc.t;

-- And pushed on from there, as this store holds them: uncompressed.
SELECT dolt_remote('add', 'back', :'url_back');
SELECT dolt_push('back', 'from_plain');
SELECT count(*) FILTER (WHERE get_byte(test_remote_get_stored(:'url_back', h), 0) = 0)
	= count(*) AS pushed_as_stored
FROM test_commit_chunks(:'head') h;

-- A frame that isn't one is refused, naming the chunk.
SELECT test_remote_put_stored(:'url_back', test_content_hash('x'), '\x07'::bytea);

\c postgres
DROP DATABASE stored_into;
DROP DATABASE stored_from;
\c :db
