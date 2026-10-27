-- Every index access method under roots: a commit's root holds each index
-- as its pages were, with the table's pages frozen beside them.  A hard
-- reset or a checkout to the commit gives the relations those pages back,
-- moving no row, and every index finds just the rows a sequential scan
-- does, before and after writes and VACUUM: GiST, GIN, BRIN, SP-GiST,
-- hash, bloom (generic WAL) and btree.

SELECT current_database() AS home \gset
CREATE DATABASE root_index_ams;
ALTER DATABASE root_index_ams SET prolly.restore_min_changes = 0;
\c root_index_ams
SET client_min_messages = warning;
CREATE EXTENSION test_prolly;
CREATE EXTENSION pageinspect;
CREATE EXTENSION amcheck;
CREATE EXTENSION bloom;
CREATE EXTENSION pg_buffercache;
RESET client_min_messages;

-- since(counter): pg_stat_prolly's counter's growth since it was last
-- asked, the session's counts flushed first (:flush, a statement of its
-- own)
\set flush 'SELECT pg_stat_force_next_flush();'
CREATE TABLE seen (name text PRIMARY KEY, n int8);
CREATE FUNCTION since(counter text) RETURNS int8 LANGUAGE sql AS $$
	INSERT INTO seen SELECT name, value FROM pg_stat_prolly WHERE name = counter
	ON CONFLICT (name) DO UPDATE SET n = excluded.n
	RETURNING n - coalesce((SELECT s.n FROM seen s WHERE s.name = counter), 0)
$$;

CREATE SCHEMA ri VERSIONED;
CREATE TABLE ri.t (id int PRIMARY KEY, v int NOT NULL, p point NOT NULL,
	q point NOT NULL, r int4range NOT NULL, tags int[] NOT NULL,
	doc tsvector NOT NULL, txt text NOT NULL, s int NOT NULL, b1 int NOT NULL,
	b2 int NOT NULL, bt int NOT NULL, w text NOT NULL)
	WITH (fillfactor = 70, autovacuum_enabled = off);
CREATE INDEX t_gist_p ON ri.t USING gist (p);
CREATE INDEX t_gist_r ON ri.t USING gist (r);
CREATE INDEX t_gin_tags ON ri.t USING gin (tags) WITH (fastupdate = off);
-- (a pending list, as well: entries not yet in the tree)
CREATE INDEX t_gin_doc ON ri.t USING gin (doc) WITH (fastupdate = on);
CREATE INDEX t_brin_s ON ri.t USING brin (s) WITH (pages_per_range = 2);
CREATE INDEX t_spgist_q ON ri.t USING spgist (q);
CREATE INDEX t_spgist_txt ON ri.t USING spgist (txt);
CREATE INDEX t_hash_v ON ri.t USING hash (v);
CREATE INDEX t_bloom ON ri.t USING bloom (b1, b2);
CREATE INDEX t_btree ON ri.t (bt);

CREATE FUNCTION fill(lo int, hi int, tag text) RETURNS void LANGUAGE sql AS $$
	INSERT INTO ri.t SELECT g, g % 50, point(g % 97, g / 97),
		point(g / 97, g % 89), int4range(g, g + g % 20 + 1),
		ARRAY[g % 31, g % 37 + 100],
		to_tsvector('simple', 'w' || (g % 41) || ' x' || (g % 43) || ' ' || tag),
		'k' || (g % 61) || tag, g, g % 53, g % 59, g % 71, repeat('w', 30)
	FROM generate_series(lo, hi) g
$$;
SELECT fill(1, 4000, 'base');
SELECT dolt_commit('-Am', 'c0') IS NOT NULL AS committed;

-- Queries each index answers, and the index it is to be answered by.
CREATE TABLE probes (idx text, q text);
INSERT INTO probes VALUES
	('t_gist_p', $$SELECT id FROM ri.t WHERE p <@ box '((10,5),(40,30))'$$),
	('t_gist_r', $$SELECT id FROM ri.t WHERE r && int4range(1500, 1530)$$),
	('t_gin_tags', $$SELECT id FROM ri.t WHERE tags @> ARRAY[7]$$),
	('t_gin_doc', $$SELECT id FROM ri.t WHERE doc @@ to_tsquery('simple', 'w3 & x5')$$),
	('t_brin_s', $$SELECT id FROM ri.t WHERE s BETWEEN 1200 AND 1900$$),
	('t_spgist_q', $$SELECT id FROM ri.t WHERE q <@ box '((10,20),(40,60))'$$),
	('t_spgist_txt', $$SELECT id FROM ri.t WHERE txt ^@ 'k17'$$),
	('t_hash_v', $$SELECT id FROM ri.t WHERE v = 13$$),
	('t_bloom', $$SELECT id FROM ri.t WHERE b1 = 21 AND b2 = 30$$),
	('t_btree', $$SELECT id FROM ri.t WHERE bt BETWEEN 3 AND 5$$);

-- disagree(): the probes whose index finds other rows than a sequential
-- scan, or whose plan doesn't use the index (none: every index agrees);
-- each index's scan forced, then a bitmap scan of it
CREATE FUNCTION disagree() RETURNS SETOF text LANGUAGE plpgsql AS $$
DECLARE
	pr record;
	seq text;
	got text;
	plan text;
	line text;
	kind text;
BEGIN
	FOR pr IN SELECT * FROM probes ORDER BY idx LOOP
		SET LOCAL enable_indexscan = off;
		SET LOCAL enable_bitmapscan = off;
		SET LOCAL enable_indexonlyscan = off;
		SET LOCAL enable_seqscan = on;
		EXECUTE format('SELECT coalesce(md5(string_agg(id::text, '','' ORDER BY id)), '''') FROM (%s) s', pr.q) INTO seq;
		FOREACH kind IN ARRAY ARRAY['index', 'bitmap'] LOOP
			SET LOCAL enable_seqscan = off;
			IF kind = 'index' THEN
				-- (bloom, BRIN and GIN scan only as bitmaps)
				CONTINUE WHEN pr.idx IN ('t_bloom', 't_brin_s', 't_gin_tags', 't_gin_doc');
				SET LOCAL enable_indexscan = on;
				SET LOCAL enable_bitmapscan = off;
			ELSE
				SET LOCAL enable_indexscan = off;
				SET LOCAL enable_bitmapscan = on;
			END IF;
			plan := '';
			FOR line IN EXECUTE 'EXPLAIN (COSTS OFF) ' || pr.q LOOP
				plan := plan || line || ' ';
			END LOOP;
			IF plan NOT LIKE '%' || pr.idx || '%' THEN
				RETURN NEXT pr.idx || ' (' || kind || '): not used: ' || plan;
				CONTINUE;
			END IF;
			EXECUTE format('SELECT coalesce(md5(string_agg(id::text, '','' ORDER BY id)), '''') FROM (%s) s', pr.q) INTO got;
			IF got <> seq THEN
				RETURN NEXT pr.idx || ' (' || kind || '): other rows';
			END IF;
		END LOOP;
		SET LOCAL enable_indexscan = on;
		SET LOCAL enable_bitmapscan = on;
		SET LOCAL enable_seqscan = on;
	END LOOP;
END $$;

-- checked(): amcheck's verdict on the btree (each heap row found in it)
-- and GIN indexes; errors if not
CREATE FUNCTION checked() RETURNS bool LANGUAGE sql AS $$
	SELECT bool_and(CASE am.amname
		WHEN 'btree' THEN bt_index_check(c.oid, true) IS NOT NULL OR true
		WHEN 'gin' THEN gin_index_check(c.oid) IS NOT NULL OR true
		ELSE true END)
	FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid
		JOIN pg_am am ON am.oid = c.relam
	WHERE i.indrelid = 'ri.t'::regclass
$$;

-- pages(): each index's pages' digest, as stored (its buffers written
-- out and evicted first: a page an access method changes in its buffer
-- without marking it dirty, as SP-GiST's new pages, is as stored only once
-- evicted, either way)
CREATE TABLE pages_at (name text, idx text, digest text);
CREATE FUNCTION pages(at text) RETURNS void LANGUAGE sql AS $$
	SELECT count(pg_buffercache_evict_relation(indexrelid)) FROM pg_index
	WHERE indrelid = 'ri.t'::regclass;
	INSERT INTO pages_at
	SELECT at, c.relname, md5(string_agg(md5(get_raw_page(c.oid::regclass::text, b::int)), '' ORDER BY b))
	FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid,
		LATERAL generate_series(0, pg_relation_size(c.oid) / 8192 - 1) b
	WHERE i.indrelid = 'ri.t'::regclass
	GROUP BY c.relname
$$;
-- the indexes whose pages differ from those at 'at'
CREATE FUNCTION pages_differ(at text) RETURNS SETOF text LANGUAGE sql AS $$
	SELECT a.idx FROM pages_at a JOIN pages_at n ON n.idx = a.idx
	WHERE a.name = at AND n.name = 'now' AND a.digest IS DISTINCT FROM n.digest
	UNION ALL
	SELECT a.idx FROM pages_at a WHERE a.name = at AND
		NOT EXISTS (SELECT FROM pages_at n WHERE n.name = 'now' AND n.idx = a.idx)
	ORDER BY 1
$$;
CREATE FUNCTION pages_now_differ(at text) RETURNS SETOF text LANGUAGE sql AS $$
	DELETE FROM pages_at WHERE name = 'now';
	SELECT pages('now');
	SELECT pages_differ(at);
$$;

CREATE FUNCTION digest() RETURNS text LANGUAGE sql AS $$
	SELECT md5(string_agg(t::text, ',' ORDER BY id)) FROM ri.t t
$$;

SELECT * FROM disagree();
SELECT checked();

-- c1: rows updated (an indexed column, and not), deleted, an aborted
-- transaction's versions, every index's pages changed
UPDATE ri.t SET w = 'hot' || id WHERE id % 7 = 0;
UPDATE ri.t SET v = v + 100, p = point(p[0] + 0.5, p[1]), q = point(q[0], q[1] + 0.5),
	tags = tags || 999, s = s + 3, b1 = b1 + 1, bt = bt + 1,
	doc = doc || to_tsvector('simple', 'c1'), txt = txt || 'c1',
	r = int4range(lower(r) + 1, upper(r) + 2)
	WHERE id % 11 = 0;
DELETE FROM ri.t WHERE id % 13 = 0;
BEGIN;
SELECT fill(5001, 5300, 'aborted');
UPDATE ri.t SET v = -1 WHERE id % 17 = 0;
ROLLBACK;
SELECT fill(6001, 6500, 'c1');
SELECT dolt_commit('-am', 'c1') IS NOT NULL AS committed;
SELECT digest() AS at_c1 \gset
SELECT pages('c1');
SELECT * FROM disagree();

-- c2: changed again, VACUUM removing index entries, BRIN summarized, the
-- pending list flushed; a branch made of c2
UPDATE ri.t SET v = v + 1000, tags = ARRAY[1], txt = 'c2', q = point(1, 1),
	bt = 0, b2 = 0 WHERE id <= 2000;
DELETE FROM ri.t WHERE id > 3000 AND id < 4000;
SELECT fill(7001, 7400, 'c2');
VACUUM ri.t;
SELECT brin_summarize_new_values('ri.t_brin_s') >= 0 AS summarized;
SELECT gin_clean_pending_list('ri.t_gin_doc') >= 0 AS cleaned;
SELECT dolt_commit('-am', 'c2') IS NOT NULL AS committed;
SELECT digest() AS at_c2 \gset
SELECT pages('c2');
SELECT dolt_branch('at_c2');

-- a hard reset to c1: no row written, the table's rows c1's, each index's
-- pages as c1's root captured them, and each finds c1's rows
:flush
SELECT since('apply_rows') >= 0 AS asked;
SELECT dolt_reset('--hard', 'HEAD~1');
:flush
SELECT since('apply_rows') AS rows_written;
SELECT digest() = :'at_c1' AS at_c1;
SELECT * FROM pages_now_differ('c1');
SELECT * FROM disagree();
SELECT checked();

-- a checkout of the branch made at c2 (its pages kept still: no commit
-- since retired c2): its rows, its indexes' pages as c2's root captured
-- them, no row written, found; and back
:flush
SELECT since('apply_rows') >= 0 AS asked;
SELECT (dolt_checkout('at_c2'))[1] = '0' AS checked_out;
SELECT digest() = :'at_c2' AS at_c2;
:flush
SELECT since('apply_rows') AS rows_written;
SELECT * FROM pages_now_differ('c2');
SELECT * FROM disagree();
SELECT checked();
SELECT (dolt_checkout('main'))[1] = '0' AS checked_out;
SELECT * FROM disagree();

-- rows written after: found, by each index
SELECT fill(8001, 8300, 'after');
UPDATE ri.t SET v = v + 7, txt = txt || 'u', p = point(1, 2), bt = bt + 2
	WHERE id % 5 = 0;
DELETE FROM ri.t WHERE id % 29 = 0;
SELECT * FROM disagree();
SELECT checked();
-- VACUUM removes the dead versions' entries, every index's (the frozen
-- root's dead line pointers among them), then rows written again
VACUUM ri.t;
SELECT fill(9001, 9300, 'vacuumed');
SELECT * FROM disagree();
SELECT checked();
SELECT dolt_commit('-am', 'c3') IS NOT NULL AS committed;

-- reset again to c1, two commits back now: its pages as captured still,
-- whatever VACUUM did to the relations' since
SELECT dolt_reset('--hard', 'HEAD~1');
SELECT digest() = :'at_c1' AS at_c1;
SELECT * FROM pages_now_differ('c1');
SELECT * FROM disagree();
SELECT checked();

\c :home
DROP DATABASE root_index_ams;
