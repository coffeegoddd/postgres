-- Each database's history store, from initdb, and its pointers: a pointer
-- made takes a root on the chunk it names, moved moves it, dropped lets it
-- go, in the pointer's transaction; one to a chunk the store hasn't, and
-- TRUNCATE of the pointers, are refused.
CREATE EXTENSION IF NOT EXISTS test_prolly;
-- (chunks nothing holds kept across VACUUM: no freer in the background)
SET chunk_free_in_background = off;
SELECT c.relname, a.amname FROM pg_class c JOIN pg_am a ON a.oid = c.relam
WHERE c.relnamespace = 'pg_history'::regnamespace AND c.relkind = 'r' ORDER BY 1;
CREATE FUNCTION h(d bytea) RETURNS bytea LANGUAGE sql IMMUTABLE
	AS 'SELECT substr(sha256(d), 1, 20)';
CREATE FUNCTION put(d bytea) RETURNS bool LANGUAGE sql
	AS $$SELECT test_chunk_put('pg_history.chunks', 'pg_history.chunk_index', h(d), d)$$;
CREATE FUNCTION roots(d bytea) RETURNS int8 LANGUAGE sql
	AS $$SELECT roots FROM test_chunk_index_entry('pg_history.chunk_index', h(d))$$;

SELECT put('\x01') AS one, put('\x02') AS two;

-- made: a root on its chunk
INSERT INTO pg_history.pointers VALUES ('main', h('\x01'));
SELECT roots('\x01') AS one_roots, roots('\x02') AS two_roots;
-- renamed: nothing moves
UPDATE pg_history.pointers SET name = 'trunk' WHERE name = 'main';
SELECT roots('\x01') AS one_roots;
-- moved: the root goes with it
UPDATE pg_history.pointers SET hash = h('\x02') WHERE name = 'trunk';
SELECT roots('\x01') AS one_roots, roots('\x02') AS two_roots;
-- a second pointer to the same chunk: two roots
INSERT INTO pg_history.pointers VALUES ('feature', h('\x02'));
SELECT roots('\x02') AS two_roots;
-- dropped: let go of
DELETE FROM pg_history.pointers WHERE name = 'feature';
SELECT roots('\x02') AS two_roots;

-- each of these in a transaction rolled back changes nothing
BEGIN;
INSERT INTO pg_history.pointers VALUES ('tmp', h('\x01'));
UPDATE pg_history.pointers SET hash = h('\x01') WHERE name = 'trunk';
DELETE FROM pg_history.pointers WHERE name = 'trunk';
SELECT roots('\x01') AS one_roots_in, roots('\x02') AS two_roots_in;
ROLLBACK;
SELECT roots('\x01') AS one_roots, roots('\x02') AS two_roots;

-- refused: a pointer to a chunk the store hasn't, and TRUNCATE
INSERT INTO pg_history.pointers VALUES ('nowhere', h('\x7a'));
UPDATE pg_history.pointers SET hash = h('\x7a') WHERE name = 'trunk';
INSERT INTO pg_history.pointers VALUES ('short', '\x01');
TRUNCATE pg_history.pointers;
SELECT name, hash = h('\x02') AS at_two FROM pg_history.pointers;

-- a chunk put and pointed to in one transaction are one: both, or neither
BEGIN;
SELECT put('\x03') AS three;
INSERT INTO pg_history.pointers VALUES ('new', h('\x03'));
ROLLBACK;
SELECT test_chunk_has('pg_history.chunks', 'pg_history.chunk_index', h('\x03')) AS three_there,
	count(*) AS new_pointers FROM pg_history.pointers WHERE name = 'new';
SELECT ok FROM test_chunk_check('pg_history.chunks', 'pg_history.chunk_index');

DELETE FROM pg_history.pointers;
SELECT roots('\x01') AS one_roots, roots('\x02') AS two_roots;
DROP FUNCTION roots(bytea), put(bytea), h(bytea);
