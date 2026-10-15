--
-- The prolly index AM on engine D1 tables: one entry per row, the index
-- columns then the row's key, kept exactly in step with the table.
--
SET default_table_access_method = prolly_d1;
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;

-- An index tree holds exactly its table's rows: the same tree as an index
-- built afresh on the same columns, however the rows got there.
CREATE FUNCTION ix_same_tree(idx regclass) RETURNS bool
LANGUAGE plpgsql AS $$
DECLARE
	def text := pg_get_indexdef(idx);
	fresh regclass;
	same bool;
BEGIN
	EXECUTE replace(def, ' INDEX ' || quote_ident(idx::text) || ' ON ',
					' INDEX ix_fresh ON ');
	fresh := 'ix_fresh'::regclass;
	same := prolly_tree_digest(idx) = prolly_tree_digest(fresh);
	EXECUTE 'DROP INDEX ix_fresh';
	RETURN same;
END $$;

-- Every index answers as a sequential scan does, and holds exactly the
-- table's rows (an index-only scan counts them).
CREATE TABLE ix (id int PRIMARY KEY, k int, t text, n int);
INSERT INTO ix SELECT g, g % 10, 'text ' || (g % 37), CASE WHEN g % 7 = 0 THEN NULL ELSE g END
	FROM generate_series(1, 2000) g;
CREATE INDEX ix_k ON ix USING prolly (k);
CREATE INDEX ix_kt ON ix USING prolly (k, t DESC);
CREATE INDEX ix_n ON ix USING prolly (n NULLS FIRST);
CREATE INDEX ix_lower ON ix USING prolly (lower(t));
CREATE INDEX ix_part ON ix USING prolly (id) WHERE k = 7;
CREATE UNIQUE INDEX ix_tid ON ix USING prolly (t, id);

CREATE FUNCTION ix_answers() RETURNS TABLE (q text, answer text)
LANGUAGE sql AS $$
	SELECT 'k = 3', count(*)::text FROM ix WHERE k = 3
	UNION ALL SELECT 'k range', count(*)::text FROM ix WHERE k > 2 AND k <= 5
	UNION ALL SELECT 'k, t', count(*)::text FROM ix WHERE k = 3 AND t = 'text 3'
	UNION ALL SELECT 'k, t range', string_agg(id::text, ',' ORDER BY id) FROM
		(SELECT id FROM ix WHERE k = 1 AND t < 'text 2' ORDER BY id LIMIT 5) s
	UNION ALL SELECT 'n null', count(*)::text FROM ix WHERE n IS NULL
	UNION ALL SELECT 'n not null', count(*)::text FROM ix WHERE n IS NOT NULL
	UNION ALL SELECT 'n order', string_agg(coalesce(n::text, 'null'), ',') FROM
		(SELECT n FROM ix ORDER BY n NULLS FIRST LIMIT 4) s
	UNION ALL SELECT 'lower', count(*)::text FROM ix WHERE lower(t) = 'text 5'
	UNION ALL SELECT 'partial', sum(id)::text FROM ix WHERE k = 7 AND id > 1000
	UNION ALL SELECT 'kt desc', string_agg(t, ',') FROM
		(SELECT t FROM ix WHERE k = 2 ORDER BY k, t DESC LIMIT 3) s
	UNION ALL SELECT 'backward', string_agg(id::text, ',') FROM
		(SELECT id FROM ix WHERE k = 4 ORDER BY k DESC, t ASC, id LIMIT 3) s
	UNION ALL SELECT 'index only', count(k)::text FROM ix WHERE k BETWEEN 1 AND 3
$$;

-- Compare index scans with sequential scans.
CREATE FUNCTION ix_agree() RETURNS TABLE (q text, agree bool)
LANGUAGE plpgsql AS $$
BEGIN
	SET LOCAL enable_indexscan = off;
	SET LOCAL enable_indexonlyscan = off;
	SET LOCAL enable_bitmapscan = off;
	CREATE TEMP TABLE ix_seq USING heap ON COMMIT DROP AS SELECT * FROM ix_answers();
	SET LOCAL enable_indexscan = on;
	SET LOCAL enable_indexonlyscan = on;
	SET LOCAL enable_seqscan = off;
	RETURN QUERY SELECT s.q, s.answer IS NOT DISTINCT FROM i.answer
		FROM ix_seq s JOIN ix_answers() i USING (q) ORDER BY s.q COLLATE "C";
	DROP TABLE ix_seq;
END $$;

-- The number of entries in an index, read with an index-only scan.
CREATE FUNCTION ix_entries(idx regclass) RETURNS bigint
LANGUAGE plpgsql AS $$
DECLARE
	cols text;
	n bigint;
BEGIN
	SELECT string_agg(quote_ident(a.attname), ',') INTO cols
		FROM pg_index i JOIN pg_attribute a ON a.attrelid = i.indrelid
			AND a.attnum = ANY (i.indkey)
		WHERE i.indexrelid = idx;
	SET LOCAL enable_seqscan = off;
	SET LOCAL enable_bitmapscan = off;
	EXECUTE format('SELECT count(*) FROM (SELECT %s FROM ix ORDER BY %s) s', cols, cols)
		INTO n;
	RETURN n;
END $$;

BEGIN;
SELECT * FROM ix_agree();
COMMIT;
SELECT ix_entries('ix_k') = (SELECT count(*) FROM ix) AS ix_k_exact,
	   ix_entries('ix_n') = (SELECT count(*) FROM ix) AS ix_n_exact;

-- Updates, deletes, inserts and rollbacks keep every index exact.
UPDATE ix SET k = (k + 1) % 10 WHERE id % 3 = 0;
UPDATE ix SET t = upper(t) WHERE id % 11 = 0;
UPDATE ix SET n = n WHERE id % 5 = 0;
DELETE FROM ix WHERE id % 13 = 0;
INSERT INTO ix SELECT g, g % 10, 'new ' || g, g FROM generate_series(3001, 3100) g;
BEGIN;
DELETE FROM ix WHERE k = 1;
SAVEPOINT s;
UPDATE ix SET k = 99;
ROLLBACK TO s;
INSERT INTO ix VALUES (5000, 1, 'kept', 1);
COMMIT;
BEGIN;
DELETE FROM ix;
ROLLBACK;
BEGIN;
SELECT * FROM ix_agree();
COMMIT;
SELECT ix_entries('ix_k') = (SELECT count(*) FROM ix) AS ix_k_exact,
	   ix_entries('ix_n') = (SELECT count(*) FROM ix) AS ix_n_exact;
SELECT i::text, ix_same_tree(i) FROM unnest(ARRAY['ix_k', 'ix_kt', 'ix_n',
	'ix_lower', 'ix_part', 'ix_tid']::regclass[]) i;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT count(*) FROM ix WHERE k = 99;
SELECT id, t FROM ix WHERE id = 5000;
RESET enable_seqscan;
RESET enable_bitmapscan;

-- Unique prolly indexes: duplicates refused, within a statement too, and
-- as ON CONFLICT arbiters.
CREATE TABLE ux (id int PRIMARY KEY, u int, v text);
CREATE UNIQUE INDEX ux_u ON ux USING prolly (u);
INSERT INTO ux VALUES (1, 10, 'a'), (2, 20, 'b'), (3, NULL, 'c'), (4, NULL, 'd');
INSERT INTO ux VALUES (5, 10, 'dup');
UPDATE ux SET u = 20 WHERE id = 1;
INSERT INTO ux VALUES (6, 30, 'x'), (7, 30, 'y');
INSERT INTO ux VALUES (8, 10, 'up') ON CONFLICT (u) DO UPDATE SET v = excluded.v;
INSERT INTO ux VALUES (9, 20, 'nothing') ON CONFLICT (u) DO NOTHING;
SELECT * FROM ux ORDER BY id;
CREATE UNIQUE INDEX ux_v ON ux USING prolly (v);
INSERT INTO ux VALUES (10, 100, 'a');
CREATE TABLE ud (a int);
INSERT INTO ud VALUES (1), (1);
CREATE UNIQUE INDEX ud_a ON ud USING prolly (a);
DELETE FROM ud WHERE ctid = (SELECT min(ctid) FROM ud);
CREATE UNIQUE INDEX ud_a ON ud USING prolly (a);

-- Keyless tables: identical rows have one entry each.
CREATE TABLE kx (a int, b text);
INSERT INTO kx VALUES (1, 'x'), (1, 'x'), (1, 'x'), (2, 'y');
CREATE INDEX kx_a ON kx USING prolly (a);
DELETE FROM kx WHERE ctid = (SELECT min(ctid) FROM kx WHERE a = 1);
UPDATE kx SET b = 'z' WHERE ctid = (SELECT max(ctid) FROM kx WHERE a = 1);
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT a, b, count(*) FROM kx WHERE a = 1 GROUP BY a, b ORDER BY b;
SELECT count(*) FROM (SELECT a FROM kx ORDER BY a) s;
RESET enable_seqscan;
RESET enable_bitmapscan;
SELECT ix_same_tree('kx_a');

-- Re-keying the table (a primary key added or dropped, columns dropped)
-- rebuilds its prolly indexes, whose entries hold row keys.
ALTER TABLE kx ADD COLUMN id serial;
ALTER TABLE kx ADD PRIMARY KEY (id);
INSERT INTO kx (a, b) VALUES (3, 'w');
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT a, b FROM kx WHERE a IN (1, 3) ORDER BY a, b;
DELETE FROM kx WHERE a = 2;
SELECT ix_same_tree('kx_a');
ALTER TABLE kx DROP CONSTRAINT kx_pkey;
ALTER TABLE kx DROP COLUMN b;
INSERT INTO kx (a) VALUES (4);
SELECT a, count(*) FROM kx WHERE a >= 1 GROUP BY a ORDER BY a;
RESET enable_seqscan;
RESET enable_bitmapscan;
SELECT ix_same_tree('kx_a');

-- Range keys on a DESC column bound the other end of the index order.
CREATE TABLE dx (k int, t text COLLATE "C");
INSERT INTO dx SELECT g % 3, chr(97 + g % 26) FROM generate_series(1, 300) g;
CREATE INDEX dx_kt ON dx USING prolly (k, t DESC);
SET enable_seqscan = off;
SET enable_bitmapscan = off;
EXPLAIN (COSTS OFF) SELECT count(*) FROM dx WHERE k = 1 AND t < 'm';
SELECT count(*) FROM dx WHERE k = 1 AND t < 'm';
SELECT count(*) FROM dx WHERE k = 1 AND t > 'm';
SELECT count(*) FROM dx WHERE k = 1 AND t >= 'c' AND t <= 'e';
SELECT string_agg(t, '' ORDER BY t DESC) FROM (SELECT DISTINCT t FROM dx WHERE k = 2 AND t > 'w') s;
RESET enable_seqscan;
RESET enable_bitmapscan;
SELECT count(*) FILTER (WHERE t < 'm'), count(*) FILTER (WHERE t > 'm'),
	   count(*) FILTER (WHERE t BETWEEN 'c' AND 'e') FROM dx WHERE k = 1;

-- Merge joins mark and restore positions; cursors scan backward.
CREATE TABLE mj (k int, v int);
INSERT INTO mj SELECT g % 5, g FROM generate_series(1, 20) g;
CREATE INDEX mj_k ON mj USING prolly (k);
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET enable_hashjoin = off;
SET enable_nestloop = off;
SET enable_sort = off;
EXPLAIN (COSTS OFF) SELECT count(*) FROM mj a JOIN mj b ON a.k = b.k;
SELECT count(*) FROM mj a JOIN mj b ON a.k = b.k;
BEGIN;
DECLARE c SCROLL CURSOR FOR SELECT k FROM mj WHERE k >= 3 ORDER BY k;
FETCH 3 FROM c;
FETCH BACKWARD 2 FROM c;
FETCH LAST FROM c;
FETCH BACKWARD 1 FROM c;
COMMIT;
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET enable_hashjoin;
RESET enable_nestloop;
RESET enable_sort;

-- Turning around at either end of a scan returns the row at that end.
CREATE TABLE ends (k int, v text);
INSERT INTO ends VALUES (1, 'a'), (2, 'b'), (3, 'c');
CREATE INDEX ends_k ON ends USING prolly (k);
SET enable_seqscan = off;
SET enable_bitmapscan = off;
BEGIN;
DECLARE io SCROLL CURSOR FOR SELECT k FROM ends ORDER BY k;
FETCH ALL FROM io;
FETCH BACKWARD 1 FROM io;
FETCH BACKWARD ALL FROM io;
FETCH FORWARD 1 FROM io;
DECLARE pl SCROLL CURSOR FOR SELECT k, v FROM ends WHERE k >= 1 ORDER BY k;
FETCH ALL FROM pl;
FETCH BACKWARD 1 FROM pl;
FETCH BACKWARD ALL FROM pl;
FETCH FORWARD 1 FROM pl;
COMMIT;
RESET enable_seqscan;
RESET enable_bitmapscan;

-- A polymorphic operator class indexes a column of a concrete type.
CREATE TYPE ix_mood AS ENUM ('sad', 'ok', 'happy');
CREATE OPERATOR CLASS ix_mood_ops DEFAULT FOR TYPE anyenum USING prolly AS
	OPERATOR 1 < (anyenum, anyenum), OPERATOR 2 <= (anyenum, anyenum),
	OPERATOR 3 = (anyenum, anyenum), OPERATOR 4 >= (anyenum, anyenum),
	OPERATOR 5 > (anyenum, anyenum), FUNCTION 1 enum_cmp(anyenum, anyenum);
CREATE TABLE moods (id int PRIMARY KEY, m ix_mood);
INSERT INTO moods VALUES (1, 'happy'), (2, 'sad'), (3, 'ok'), (4, 'sad');
CREATE INDEX moods_m ON moods USING prolly (m);
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT m, count(*) FROM moods WHERE m >= 'ok' GROUP BY m ORDER BY m;
SELECT id FROM moods ORDER BY m, id;
RESET enable_seqscan;
RESET enable_bitmapscan;
DROP TABLE moods, ends;
DROP OPERATOR CLASS ix_mood_ops USING prolly;
DROP OPERATOR FAMILY ix_mood_ops USING prolly;
DROP TYPE ix_mood;

-- REINDEX, CREATE INDEX CONCURRENTLY, TRUNCATE.
REINDEX INDEX mj_k;
CREATE INDEX CONCURRENTLY mj_v ON mj USING prolly (v);
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT count(*) FROM mj WHERE v > 10;
TRUNCATE mj;
INSERT INTO mj VALUES (1, 1);
SELECT count(*) FROM mj WHERE k = 1;
SELECT count(*) FROM mj WHERE v = 1;
RESET enable_seqscan;
RESET enable_bitmapscan;

-- A btree index on a prolly table isn't built in parallel: the table has no
-- parallel scans (workers can't see a transaction's private writes).
BEGIN;
SET LOCAL max_parallel_maintenance_workers = 4;
SET LOCAL min_parallel_table_scan_size = 0;
INSERT INTO mj VALUES (2, 2);
CREATE INDEX mj_btree ON mj USING btree (v);
CREATE INDEX mj_expr ON mj USING btree ((k + 1));
COMMIT;
SET enable_seqscan = off;
SELECT k, v FROM mj WHERE v = 2;
RESET enable_seqscan;

-- A table's size estimate counts the transaction's writes once they are
-- spilled too (else the planner thinks it empty).
CREATE FUNCTION d1_plan_rows(q text) RETURNS float8 LANGUAGE plpgsql AS $$
DECLARE p json;
BEGIN
  EXECUTE 'EXPLAIN (FORMAT JSON) ' || q INTO p;
  RETURN (p -> 0 -> 'Plan' ->> 'Plan Rows')::float8;
END $$;
BEGIN;
SET LOCAL prolly_write_set_memory = 64;
CREATE TABLE spilled AS SELECT generate_series(1, 5000) AS id;
CREATE TABLE more AS SELECT generate_series(1, 20000) AS id, repeat('x', 50) AS pad;
SELECT d1_plan_rows('SELECT * FROM spilled') > 4000 AS counted;
ROLLBACK;
DROP FUNCTION d1_plan_rows(text);

-- Not supported: prolly indexes on heap tables, INCLUDE columns.
CREATE TABLE hx (a int) USING heap;
CREATE INDEX hx_a ON hx USING prolly (a);
CREATE INDEX mj_inc ON mj USING prolly (k) INCLUDE (v);

DROP TABLE ix, ux, ud, kx, mj, hx, dx;
DROP FUNCTION ix_answers(), ix_agree(), ix_entries(regclass), ix_same_tree(regclass);
