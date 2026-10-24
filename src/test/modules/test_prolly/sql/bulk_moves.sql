-- Moving a branch inserts the rows that differ many at once (the row
-- applier's insert_many()), a page's rows together, and fills a table new
-- in the transaction with its indexes built after the rows (#490).  What
-- it writes is what writing the rows one at a time wrote: the commit's
-- rows, every index valid, every constraint held, no user trigger fired.

SELECT current_database() AS home \gset
CREATE DATABASE bulk_moves;
-- (the rows moves write, not pages given back: kept_pages tests those)
ALTER DATABASE bulk_moves SET prolly.kept_commits = 0;
\c bulk_moves
SET client_min_messages = warning;
CREATE EXTENSION amcheck;
CREATE EXTENSION test_prolly;
RESET client_min_messages;
\getenv abs_builddir PG_ABS_BUILDDIR
SELECT 'file://' || :'abs_builddir' || '/tmp_check/remotes/bulk-' || txid_current() AS url \gset

-- (counters read after the session's are flushed)
CREATE FUNCTION counted(OUT batched bigint, OUT loads bigint) LANGUAGE sql AS
$$SELECT (SELECT value FROM pg_stat_prolly WHERE name = 'apply_rows_batched'),
		 (SELECT value FROM pg_stat_prolly WHERE name = 'apply_loads')$$;
-- the working set is the head's, every table alike
CREATE FUNCTION same_as_head() RETURNS bool LANGUAGE sql AS
$$SELECT NOT EXISTS (SELECT FROM dolt_status)$$;

CREATE TABLE public.fired (what text);

CREATE SCHEMA bm VERSIONED;
CREATE FUNCTION bm.fire() RETURNS trigger LANGUAGE plpgsql AS
$$BEGIN
  INSERT INTO public.fired VALUES (TG_OP || ' ' || TG_TABLE_NAME);
  RETURN coalesce(NEW, OLD);
END$$;
-- every btree index of the versioned schema's tables checked, with every
-- row of its table found in it (not a partitioned table's: it has none)
CREATE FUNCTION indexes_valid() RETURNS TABLE (index regclass, valid bool)
LANGUAGE sql AS
$$SELECT i.indexrelid::regclass, bt_index_check(i.indexrelid, true) IS NOT NULL
  FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid
  JOIN pg_am a ON a.oid = c.relam AND a.amname = 'btree'
  WHERE c.relkind = 'i' AND i.indrelid IN (SELECT oid FROM pg_class
					   WHERE relnamespace = 'bm'::regnamespace)
  ORDER BY i.indexrelid::regclass::text COLLATE "C"$$;
CREATE TABLE bm.p (id int PRIMARY KEY);
INSERT INTO bm.p SELECT g FROM generate_series(1, 10) g;
CREATE TABLE bm.t (id int PRIMARY KEY, v text NOT NULL CHECK (v <> 'bad'),
				   u int UNIQUE, p int REFERENCES bm.p, big text,
				   g int GENERATED ALWAYS AS (id * 2) STORED);
ALTER TABLE bm.t ALTER COLUMN big SET STORAGE EXTERNAL;
CREATE INDEX t_v ON bm.t (v);
CREATE INDEX t_lower ON bm.t (lower(v));
CREATE TABLE bm.k (a text, b int);		-- keyless
CREATE TRIGGER t_before BEFORE INSERT OR UPDATE OR DELETE ON bm.t
  FOR EACH ROW EXECUTE FUNCTION bm.fire();
CREATE TRIGGER t_always AFTER INSERT OR UPDATE OR DELETE ON bm.t
  FOR EACH ROW EXECUTE FUNCTION bm.fire();
ALTER TABLE bm.t ENABLE ALWAYS TRIGGER t_always;
SELECT length(dolt_commit('-Am', 'empty')::text) > 0 AS committed;
SELECT dolt_hashof('HEAD') AS c0 \gset

ALTER TABLE bm.t DISABLE TRIGGER t_before;
ALTER TABLE bm.t DISABLE TRIGGER t_always;
INSERT INTO bm.t (id, v, u, p, big)
SELECT g, 'v' || g, g, g % 10 + 1,
	   CASE WHEN g % 100 = 0 THEN (SELECT string_agg(md5(g::text || i), '')
								   FROM generate_series(1, 100) i) END
FROM generate_series(1, 3000) g;
INSERT INTO bm.k SELECT 'k' || g % 700, g % 3 FROM generate_series(1, 2000) g;
ALTER TABLE bm.t ENABLE TRIGGER t_before;
ALTER TABLE bm.t ENABLE ALWAYS TRIGGER t_always;
SELECT length(dolt_commit('-am', 'rows')::text) > 0 AS committed;
SELECT dolt_hashof('HEAD') AS c1 \gset
DELETE FROM public.fired;

-- Back to no rows, then to the rows again: each table's 3000 and 2000
-- inserted many at once (into tables that were there: no load).
SELECT dolt_reset('--hard', :'c0');
SELECT count(*) FROM bm.t;
SELECT pg_stat_force_next_flush();
SELECT batched AS batched0, loads AS loads0 FROM counted() \gset
SELECT dolt_reset('--hard', :'c1');
SELECT pg_stat_force_next_flush();
SELECT batched - :batched0 AS batched, loads - :loads0 AS loads FROM counted();
SELECT same_as_head();
SELECT count(*), sum(id), sum(g), count(big), sum(length(big)) FROM bm.t;
SELECT count(*), count(DISTINCT a), sum(b) FROM bm.k;
SELECT * FROM indexes_valid();
-- index scans find what the table holds
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT id, g FROM bm.t WHERE v = 'v2000';
SELECT id FROM bm.t WHERE lower(v) = 'v17';
SELECT id FROM bm.t WHERE u = 2999;
RESET enable_seqscan;
RESET enable_bitmapscan;
-- no user trigger fired (an ALWAYS one neither)
SELECT count(*) AS fired FROM public.fired;
-- and the constraints hold for what is written next
INSERT INTO bm.t (id, v, u) VALUES (5000, 'x', 7);
INSERT INTO bm.t (id, v, u) VALUES (7, 'x', 5007);
INSERT INTO bm.t (id, v, u, p) VALUES (5000, 'x', 5000, 99);
SELECT count(*) AS fired FROM public.fired;
DELETE FROM public.fired;

-- A move that changes rows in place and removes and inserts: only the
-- inserts are written many at once.
UPDATE bm.t SET v = 'w' || id WHERE id <= 500;
DELETE FROM bm.t WHERE id > 2500;
INSERT INTO bm.t (id, v, u) SELECT g, 'n' || g, g FROM generate_series(10001, 10400) g;
DELETE FROM bm.k WHERE b = 0;
DELETE FROM public.fired;
SELECT pg_stat_force_next_flush();
SELECT batched AS batched0, loads AS loads0 FROM counted() \gset
SELECT dolt_reset('--hard', :'c1');
SELECT pg_stat_force_next_flush();
SELECT batched - :batched0 AS batched, loads - :loads0 AS loads FROM counted();
SELECT same_as_head();
SELECT count(*), sum(id), max(v) FROM bm.t;
SELECT count(*), sum(b) FROM bm.k;
SELECT * FROM indexes_valid();
SELECT count(*) AS fired FROM public.fired;

-- Tables the move makes are filled from nothing: their indexes built
-- after the rows, the unique, exclusion and primary keys checked then.
-- Not one with a deferrable unique constraint (its rechecks are queued as
-- each row is written): its rows are still written many at once.
CREATE TABLE bm.n (id int PRIMARY KEY, v text UNIQUE, r int4range,
				   big text, EXCLUDE USING gist (r WITH &&));
ALTER TABLE bm.n ALTER COLUMN big SET STORAGE EXTERNAL;
CREATE INDEX n_v ON bm.n (v DESC);
INSERT INTO bm.n SELECT g, 'n' || g, int4range(g * 10, g * 10 + 5),
	CASE WHEN g % 50 = 0 THEN (SELECT string_agg(md5(g::text || i), '')
							   FROM generate_series(1, 100) i) END
FROM generate_series(1, 2500) g;
CREATE TABLE bm.nk (a int, b text);		-- keyless
CREATE INDEX nk_a ON bm.nk (a);
INSERT INTO bm.nk SELECT g % 300, 'b' FROM generate_series(1, 900) g;
CREATE TABLE bm.d (id int PRIMARY KEY, u int UNIQUE DEFERRABLE);
INSERT INTO bm.d SELECT g, g FROM generate_series(1, 400) g;
CREATE TABLE bm.pt (id int PRIMARY KEY, v text) PARTITION BY RANGE (id);
CREATE TABLE bm.pt1 PARTITION OF bm.pt FOR VALUES FROM (0) TO (1000);
CREATE TABLE bm.pt2 PARTITION OF bm.pt FOR VALUES FROM (1000) TO (2000);
INSERT INTO bm.pt SELECT g, 'p' || g FROM generate_series(1, 1999) g;
-- (a table of few rows keeps its indexes a row at a time)
CREATE TABLE bm.few (id int PRIMARY KEY);
INSERT INTO bm.few VALUES (1), (2);
SELECT length(dolt_commit('-Am', 'made')::text) > 0 AS committed;
SELECT dolt_hashof('HEAD') AS c2 \gset
SELECT dolt_reset('--hard', :'c1');
SELECT pg_stat_force_next_flush();
SELECT batched AS batched0, loads AS loads0 FROM counted() \gset
SELECT dolt_reset('--hard', :'c2');
SELECT pg_stat_force_next_flush();
SELECT batched - :batched0 AS batched, loads - :loads0 AS loads FROM counted();
SELECT same_as_head();
SELECT count(*), sum(id), count(big), sum(length(big)) FROM bm.n;
SELECT count(*), sum(a) FROM bm.nk;
SELECT count(*), sum(u) FROM bm.d;
SELECT tableoid::regclass, count(*) FROM bm.pt GROUP BY 1 ORDER BY tableoid::regclass::text COLLATE "C";
SELECT * FROM indexes_valid();
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT id FROM bm.n WHERE v = 'n1234';
SELECT count(*) FROM bm.nk WHERE a = 7;
SELECT id FROM bm.pt WHERE id = 1500;
RESET enable_seqscan;
RESET enable_bitmapscan;
INSERT INTO bm.n VALUES (9999, 'n5', NULL);
INSERT INTO bm.n VALUES (9999, 'x', int4range(12, 13));
INSERT INTO bm.d VALUES (9999, 3);
SELECT count(*) AS fired FROM public.fired;

-- A table emptied by TRUNCATE in the move's transaction has new storage
-- no other transaction sees: filled again by the move, it is loaded too,
-- its indexes (given new storage by the TRUNCATE) built again after.
BEGIN;
TRUNCATE bm.t;
SELECT pg_stat_force_next_flush();
SELECT batched AS batched0, loads AS loads0 FROM counted() \gset
SELECT dolt_reset('--hard', :'c2');
COMMIT;
SELECT pg_stat_force_next_flush();
SELECT batched - :batched0 AS batched, loads - :loads0 AS loads FROM counted();
SELECT same_as_head();
SELECT count(*), sum(id), sum(g), count(big), sum(length(big)) FROM bm.t;
SELECT * FROM indexes_valid();
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT id, g FROM bm.t WHERE v = 'v2000';
SELECT id FROM bm.t WHERE u = 2999;
RESET enable_seqscan;
RESET enable_bitmapscan;
INSERT INTO bm.t (id, v, u) VALUES (7, 'x', 5007);
SELECT count(*) AS fired FROM public.fired;

-- A clone fills every table from nothing too.
SELECT dolt_remote('add', 'origin', :'url');
SELECT dolt_push('origin', 'main') AS pushed;
CREATE DATABASE bulk_moves_clone;
-- (the rows moves write, not pages given back: kept_pages tests those)
ALTER DATABASE bulk_moves_clone SET prolly.kept_commits = 0;
\c bulk_moves_clone
SET client_min_messages = warning;
CREATE EXTENSION amcheck;
RESET client_min_messages;
SELECT dolt_clone(:'url');
SELECT pg_stat_force_next_flush();
SELECT value AS batched FROM pg_stat_prolly WHERE name = 'apply_rows_batched';
SELECT value AS loads FROM pg_stat_prolly WHERE name = 'apply_loads';
SELECT count(*), sum(id), sum(length(big)) FROM bm.t;
SELECT count(*), sum(id) FROM bm.n;
SELECT bool_and(bt_index_check(i.indexrelid, true) IS NOT NULL) AS indexes_valid
FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid
JOIN pg_am a ON a.oid = c.relam AND a.amname = 'btree'
WHERE c.relkind = 'i' AND i.indrelid IN (SELECT oid FROM pg_class WHERE relnamespace = 'bm'::regnamespace);

\c :home
DROP DATABASE bulk_moves_clone;
DROP DATABASE bulk_moves;
