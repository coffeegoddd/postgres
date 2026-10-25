-- A branch's catalog rows are found by an index on each branch-aware
-- catalog's branch column: a branch's first use copies the rows on the
-- branch it was made from, deleting a branch deletes the rows on it, and a
-- commit's canonical form is built of the rows on the branch, each in work
-- of the objects on that branch, however many rows other branches and
-- shared objects have.

SELECT current_database() AS home \gset

-- Every catalog with a branch column has an index on that column alone
-- (pg_shdepend, shared by every database, on its database and branch).
SELECT c.relname, a.attname
FROM pg_class c
JOIN pg_attribute a ON a.attrelid = c.oid
WHERE c.oid < 16384 AND c.relkind = 'r' AND a.atttypid = 'oid'::regtype
  AND a.attname ~ 'branch$'
  AND NOT EXISTS (
	SELECT FROM pg_index i
	WHERE i.indrelid = c.oid
	  AND i.indkey[i.indnkeyatts - 1] = a.attnum
	  AND (i.indnkeyatts = 1 OR
		   (c.relname = 'pg_shdepend' AND i.indnkeyatts = 2)))
ORDER BY 1;

-- Two databases with the same versioned table: a, with nothing else, and
-- b, with shared tables in a plain schema and branches holding tables of
-- their own (made before the table, so they hold no copies of it).
CREATE DATABASE branch_catalog_index_a;
CREATE DATABASE branch_catalog_index_b;

\c branch_catalog_index_b
CREATE SCHEMA s VERSIONED;
CREATE TABLE s.first (id int PRIMARY KEY);
SELECT dolt_commit('-Am', 'start') IS NOT NULL AS committed;
CREATE SCHEMA plain;
DO $$ BEGIN
	FOR g IN 1..300 LOOP
		EXECUTE format('CREATE TABLE plain.p%s (id int PRIMARY KEY, a text, b int, c date)', g);
	END LOOP;
END $$;
SELECT dolt_branch('other' || g) FROM generate_series(1, 3) g;
SELECT dolt_checkout('other1');
DO $$ BEGIN
	FOR g IN 1..100 LOOP
		EXECUTE format('CREATE TABLE s.o%s (id int PRIMARY KEY, a text, b int, c date)', g);
	END LOOP;
END $$;
SELECT dolt_checkout('other2');
CREATE TABLE s.o (id int PRIMARY KEY, a text);
SELECT dolt_checkout('other3');
SELECT dolt_checkout('main');

\c branch_catalog_index_a
CREATE SCHEMA s VERSIONED;
CREATE TABLE s.first (id int PRIMARY KEY);
SELECT dolt_commit('-Am', 'start') IS NOT NULL AS committed;

-- In each: the table, and the catalogs' block fetches as statistics are
-- flushed (counted with no catalog read, so that the count itself doesn't
-- grow with the catalogs).
SELECT format($s$
	CREATE TABLE s.t (id serial PRIMARY KEY, v int NOT NULL, w text);
	CREATE INDEX t_v ON s.t (v);
	INSERT INTO s.t (v, w) SELECT g %% 10, md5(g::text) FROM generate_series(1, 200) g;
	SELECT dolt_commit('-Am', 'load') IS NOT NULL AS committed;
	CREATE FUNCTION catalog_blocks() RETURNS bigint LANGUAGE sql AS $f$
		SELECT sum(pg_stat_get_blocks_fetched(o::oid))::bigint
		FROM generate_series(1, 16383) o
	$f$;
	$s$) AS setup \gset
:setup
\c branch_catalog_index_b
:setup

-- In a fresh session of each: a branch made, used (its copies made of
-- main's rows), a row changed and committed, main checked out and the
-- branch deleted (its rows deleted).
\c branch_catalog_index_a
SELECT pg_stat_force_next_flush();
SELECT catalog_blocks() AS b0 \gset
SELECT dolt_branch('m');
SELECT dolt_checkout('m');
UPDATE s.t SET v = v + 1 WHERE id = 1;
SELECT dolt_commit('-am', 'on m') IS NOT NULL AS committed;
SELECT dolt_checkout('main');
SELECT dolt_branch('-D', 'm');
SELECT pg_stat_force_next_flush();
SELECT catalog_blocks() - :b0 AS small \gset

\c branch_catalog_index_b
SELECT pg_stat_force_next_flush();
SELECT catalog_blocks() AS b0 \gset
SELECT dolt_branch('m');
SELECT dolt_checkout('m');
UPDATE s.t SET v = v + 1 WHERE id = 1;
SELECT dolt_commit('-am', 'on m') IS NOT NULL AS committed;
SELECT dolt_checkout('main');
SELECT dolt_branch('-D', 'm');
SELECT pg_stat_force_next_flush();
SELECT catalog_blocks() - :b0 AS large \gset

-- About as many catalog blocks with the unrelated objects as without:
-- a few more, for b's larger indexes over the same rows (its 401
-- unrelated tables deepen them), fewer than 2.25 a table (about 1.9 now).
-- A scan of each catalog's every row would read several times as many in
-- b.  (Bounded by the objects added, not by a's count, which other work,
-- the caches a checkout keeps, changes.)
SELECT :large - :small < 401 * 9 / 4 AS flat, :small > 0 AS counted;

\c :home
DROP DATABASE branch_catalog_index_a;
DROP DATABASE branch_catalog_index_b;
