-- A move across a column's change of type writes every row of the table
-- afresh, the commit's: the rows the table has go first all at once, the
-- table given new storage as TRUNCATE gives it (#492), not removed one by
-- one, and the commit's are loaded into it, its indexes built after them
-- (#490).  No trigger fires, ON TRUNCATE ones neither.  A move across a
-- change of primary key rewrites no row: the new key's index is built.

SELECT current_database() AS home \gset
CREATE DATABASE retype_moves;
\c retype_moves
SET client_min_messages = warning;
CREATE EXTENSION amcheck;
RESET client_min_messages;

-- the rows the transaction wrote to a table, so far (those written before
-- are flushed first: pg_stat_force_next_flush())
CREATE FUNCTION written(t regclass, OUT ins bigint, OUT del bigint)
LANGUAGE sql AS
$$SELECT n_tup_ins, n_tup_del FROM pg_stat_xact_user_tables WHERE relid = t$$;
CREATE TABLE public.fired (what text);

CREATE SCHEMA rt VERSIONED;
CREATE FUNCTION indexes_valid() RETURNS bool LANGUAGE sql AS
$$SELECT bool_and(bt_index_check(i.indexrelid, true) IS NOT NULL)
  FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid
  JOIN pg_am a ON a.oid = c.relam AND a.amname = 'btree'
  WHERE c.relkind = 'i' AND i.indrelid IN (SELECT oid FROM pg_class
					   WHERE relnamespace = 'rt'::regnamespace)$$;
CREATE FUNCTION rt.fire() RETURNS trigger LANGUAGE plpgsql AS
$$BEGIN
  INSERT INTO public.fired VALUES (TG_OP || ' ' || TG_TABLE_NAME);
  RETURN NULL;
END$$;
CREATE TABLE rt.p (id int PRIMARY KEY);
INSERT INTO rt.p SELECT g FROM generate_series(1, 10) g;
CREATE TABLE rt.t (id int PRIMARY KEY, v text, w int UNIQUE, p int REFERENCES rt.p,
				   big text);
ALTER TABLE rt.t ALTER COLUMN big SET STORAGE EXTERNAL;
CREATE INDEX t_v ON rt.t (v);
-- (referenced by another table's foreign key too)
CREATE TABLE rt.r (id int PRIMARY KEY, t int REFERENCES rt.t);
CREATE TABLE rt.k (a int, b text);		-- keyless
CREATE INDEX k_a ON rt.k (a);
CREATE TABLE rt.pt (id int PRIMARY KEY, w int) PARTITION BY RANGE (id);
CREATE TABLE rt.pt1 PARTITION OF rt.pt FOR VALUES FROM (0) TO (1000);
CREATE TABLE rt.pt2 PARTITION OF rt.pt FOR VALUES FROM (1000) TO (2000);
INSERT INTO rt.t SELECT g, 'v' || g, g, g % 10 + 1,
	CASE WHEN g % 100 = 0 THEN (SELECT string_agg(md5(g::text || i), '')
								FROM generate_series(1, 100) i) END
FROM generate_series(1, 3000) g;
INSERT INTO rt.r SELECT g, g FROM generate_series(1, 50) g;
INSERT INTO rt.k SELECT g % 500, 'k' FROM generate_series(1, 1500) g;
INSERT INTO rt.pt SELECT g, g FROM generate_series(1, 1999) g;
CREATE TRIGGER t_row AFTER INSERT OR UPDATE OR DELETE ON rt.t
  FOR EACH ROW EXECUTE FUNCTION rt.fire();
CREATE TRIGGER t_truncate AFTER TRUNCATE ON rt.t
  FOR EACH STATEMENT EXECUTE FUNCTION rt.fire();
ALTER TABLE rt.t ENABLE ALWAYS TRIGGER t_truncate;
SELECT length(dolt_commit('-Am', 'int')::text) > 0 AS committed;
SELECT dolt_hashof('HEAD') AS c1 \gset

-- Each column changes type, and 1% of the rows otherwise.
ALTER TABLE rt.t ALTER COLUMN w TYPE bigint;
ALTER TABLE rt.k ALTER COLUMN a TYPE bigint;
ALTER TABLE rt.pt ALTER COLUMN w TYPE numeric;
UPDATE rt.t SET v = 'x' || id WHERE id % 100 = 0;
UPDATE rt.k SET b = 'x' WHERE a = 7;
SELECT length(dolt_commit('-am', 'bigint')::text) > 0 AS committed;
SELECT dolt_hashof('HEAD') AS c2 \gset
DELETE FROM public.fired;

SELECT dolt_reset('--hard', :'c1');
SELECT pg_relation_filenode('rt.t') AS t_storage \gset
SELECT pg_stat_force_next_flush();
BEGIN;
SELECT dolt_reset('--hard', :'c2');
-- no row removed one by one: the table's storage is new
SELECT * FROM written('rt.t');
SELECT * FROM written('rt.k');
SELECT * FROM written('rt.pt1');
SELECT pg_relation_filenode('rt.t') <> :t_storage AS new_storage;
COMMIT;
SELECT NOT EXISTS (SELECT FROM dolt_status) AS at_head;
SELECT format_type(atttypid, atttypmod) FROM pg_attribute
WHERE attrelid = 'rt.t'::regclass AND attname = 'w';
SELECT count(*), sum(id), sum(w), count(big), sum(length(big)),
	   count(*) FILTER (WHERE v LIKE 'x%') AS changed FROM rt.t;
SELECT count(*), sum(a), count(*) FILTER (WHERE b = 'x') AS changed FROM rt.k;
SELECT tableoid::regclass, count(*), sum(w) FROM rt.pt GROUP BY 1 ORDER BY 1;
SELECT indexes_valid();
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT id FROM rt.t WHERE w = 2999;
SELECT id FROM rt.t WHERE v = 'x300';
SELECT count(*) FROM rt.k WHERE a = 7;
RESET enable_seqscan;
RESET enable_bitmapscan;
SELECT count(*) AS fired FROM public.fired;
-- the constraints hold, the foreign keys into the table too
INSERT INTO rt.t (id, v, w) VALUES (9999, 'y', 5);
DELETE FROM rt.t WHERE id = 1;
INSERT INTO rt.t (id, v, w, p) VALUES (9999, 'y', 9999, 99);
SELECT conname, convalidated FROM pg_constraint
WHERE conrelid IN ('rt.t'::regclass, 'rt.r'::regclass) AND contype = 'f' ORDER BY 1;

-- And back.
SELECT pg_stat_force_next_flush();
BEGIN;
SELECT dolt_reset('--hard', :'c1');
SELECT * FROM written('rt.t');
COMMIT;
SELECT NOT EXISTS (SELECT FROM dolt_status) AS at_head;
SELECT count(*), sum(id), sum(w), sum(length(big)) FROM rt.t;
SELECT indexes_valid();

-- A table the session reads otherwise (a cursor open on it) can't change
-- type: the move fails, and leaves the branch as it was.  (It isn't
-- emptied all at once either.)
BEGIN;
DECLARE c CURSOR FOR SELECT id FROM rt.t ORDER BY id;
FETCH 1 FROM c;
SELECT dolt_reset('--hard', :'c2');
ROLLBACK;
SELECT NOT EXISTS (SELECT FROM dolt_status) AS at_head;
SELECT count(*), sum(w) FROM rt.t;

-- A move across a change of primary key rewrites no row.
ALTER TABLE rt.r DROP CONSTRAINT r_t_fkey;
ALTER TABLE rt.t DROP CONSTRAINT t_pkey;
ALTER TABLE rt.t ADD PRIMARY KEY (id, w);
SELECT length(dolt_commit('-am', 'rekeyed')::text) > 0 AS committed;
SELECT dolt_hashof('HEAD') AS c3 \gset
SELECT dolt_reset('--hard', :'c1');
SELECT pg_stat_force_next_flush();
BEGIN;
SELECT dolt_reset('--hard', :'c3');
SELECT * FROM written('rt.t');
COMMIT;
SELECT NOT EXISTS (SELECT FROM dolt_status) AS at_head;
SELECT indexes_valid();
SELECT count(*) AS fired FROM public.fired;

\c :home
DROP DATABASE retype_moves;
