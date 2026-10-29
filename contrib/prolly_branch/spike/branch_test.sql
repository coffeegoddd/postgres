-- Experiment A: branches as relation files, switched in the catalog.
-- Every check raises on failure.  Run in a fresh database.
\set ON_ERROR_STOP 1
SET client_min_messages = warning;
CREATE EXTENSION amcheck;
CREATE EXTENSION pg_trgm;
CREATE EXTENSION prolly_branch;
CREATE SCHEMA bt;
CREATE TABLE bt.item (
	id int GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
	v int NOT NULL,
	tags int[] NOT NULL,
	pt point NOT NULL,
	word text NOT NULL,
	big text NOT NULL
);
CREATE INDEX item_v ON bt.item (v);
CREATE INDEX item_v_hash ON bt.item USING hash (v);
CREATE INDEX item_tags ON bt.item USING gin (tags);
CREATE INDEX item_pt ON bt.item USING gist (pt);
CREATE INDEX item_word ON bt.item USING spgist (word);
CREATE INDEX item_id_brin ON bt.item USING brin (id);
CREATE INDEX item_word_trgm ON bt.item USING gin (word gin_trgm_ops);

-- What a branch holds, answered through each index and with indexes off;
-- both must agree.
CREATE FUNCTION bt.fingerprint(indexes bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	out text;
BEGIN
	PERFORM set_config('enable_seqscan', CASE WHEN indexes THEN 'off' ELSE 'on' END, true);
	PERFORM set_config('enable_indexscan', CASE WHEN indexes THEN 'on' ELSE 'off' END, true);
	PERFORM set_config('enable_bitmapscan', CASE WHEN indexes THEN 'on' ELSE 'off' END, true);
	PERFORM set_config('enable_indexonlyscan', CASE WHEN indexes THEN 'on' ELSE 'off' END, true);
	SELECT concat_ws(' ',
		(SELECT count(*) FROM bt.item WHERE id BETWEEN 50 AND 900),
		(SELECT count(*) FROM bt.item WHERE v BETWEEN 100 AND 700),
		(SELECT count(*) FROM bt.item WHERE v = 333),
		(SELECT count(*) FROM bt.item WHERE tags @> ARRAY[7]),
		(SELECT count(*) FROM bt.item WHERE pt <@ box '((0,0),(50,50))'),
		(SELECT count(*) FROM bt.item WHERE word LIKE 'w1%'),
		(SELECT count(*) FROM bt.item WHERE word % 'w123'),
		(SELECT sum(length(big)) FROM bt.item),
		(SELECT md5(string_agg(id || ':' || v || ':' || word, ',' ORDER BY id)) FROM bt.item))
	  INTO out;
	RETURN out;
END $$;

CREATE FUNCTION bt.check(label text, want text) RETURNS void LANGUAGE plpgsql AS $$
DECLARE
	seq text := bt.fingerprint(false);
	idx text := bt.fingerprint(true);
	r record;
BEGIN
	IF seq <> idx THEN
		RAISE EXCEPTION '%: through indexes % but scanning %', label, idx, seq;
	END IF;
	IF want IS NOT NULL AND seq <> want THEN
		RAISE EXCEPTION '%: got %, want %', label, seq, want;
	END IF;
	FOR r IN SELECT c.oid, am.amname FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid
			   JOIN pg_am am ON am.oid = c.relam
			  WHERE i.indrelid = 'bt.item'::regclass LOOP
		IF r.amname = 'btree' THEN
			PERFORM bt_index_check(r.oid, true);
		ELSIF r.amname = 'gin' THEN
			PERFORM gin_index_check(r.oid);
		END IF;
	END LOOP;
	PERFORM verify_heapam('bt.item');
END $$;

CREATE TABLE bt.seen (label text PRIMARY KEY, fp text);
CREATE FUNCTION bt.write(n int, seed int) RETURNS void LANGUAGE sql AS $$
	INSERT INTO bt.item (v, tags, pt, word, big)
	SELECT (g * seed) % 1000, ARRAY[g % 10, seed % 10], point(g % 100, (g * seed) % 100),
		   'w' || ((g * seed) % 5000), repeat(md5((g * seed)::text), 100 + seed)
	  FROM generate_series(1, n) g;
$$;

-- 1. A branch is a copy; each side then changes on its own.
SELECT bt.write(2000, 3);
SELECT prolly_branch.init('bt');
INSERT INTO bt.seen VALUES ('base', bt.fingerprint(false));
SELECT prolly_branch.create_branch('bt', 'b');
SELECT bt.write(500, 7);
UPDATE bt.item SET v = v + 1 WHERE id % 5 = 0;
DELETE FROM bt.item WHERE id % 11 = 0;
INSERT INTO bt.seen VALUES ('main', bt.fingerprint(false));
SELECT bt.check('main written', (SELECT fp FROM bt.seen WHERE label = 'main'));

SELECT prolly_branch.checkout('bt', 'b');
SELECT bt.check('b is the base', (SELECT fp FROM bt.seen WHERE label = 'base'));
SELECT bt.write(300, 11);
DELETE FROM bt.item WHERE id % 7 = 0;
INSERT INTO bt.seen VALUES ('b', bt.fingerprint(false));
SELECT bt.check('b written', (SELECT fp FROM bt.seen WHERE label = 'b'));

SELECT prolly_branch.checkout('bt', 'main');
SELECT bt.check('main again', (SELECT fp FROM bt.seen WHERE label = 'main'));
SELECT prolly_branch.checkout('bt', 'b');
SELECT bt.check('b again', (SELECT fp FROM bt.seen WHERE label = 'b'));

-- 2. Identity sequences are per branch: b's ids continue b's own.
DO $$
DECLARE
	b_next int;
	m_next int;
BEGIN
	INSERT INTO bt.item (v, tags, pt, word, big) VALUES (1, '{1}', '(1,1)', 'x', 'x') RETURNING id INTO b_next;
	PERFORM prolly_branch.checkout('bt', 'main');
	INSERT INTO bt.item (v, tags, pt, word, big) VALUES (1, '{1}', '(1,1)', 'x', 'x') RETURNING id INTO m_next;
	IF b_next <> 2301 OR m_next <> 2501 THEN
		RAISE EXCEPTION 'identities: b %, main %', b_next, m_next;
	END IF;
END $$;
UPDATE bt.seen SET fp = bt.fingerprint(false) WHERE label = 'main';
SELECT prolly_branch.checkout('bt', 'b');
UPDATE bt.seen SET fp = bt.fingerprint(false) WHERE label = 'b';

-- 3. A checkout rolled back leaves the branch it found.
BEGIN;
SELECT prolly_branch.checkout('bt', 'main');
SELECT bt.check('inside the checkout', (SELECT fp FROM bt.seen WHERE label = 'main'));
ROLLBACK;
SELECT bt.check('rolled back', (SELECT fp FROM bt.seen WHERE label = 'b'));
DO $$ BEGIN
	IF (SELECT branch FROM prolly_branch.checked_out) <> 'b' THEN
		RAISE EXCEPTION 'checked_out after rollback: %', (SELECT branch FROM prolly_branch.checked_out);
	END IF;
END $$;

-- 4. A branch made and dropped in one transaction rolled back leaves nothing.
BEGIN;
SELECT prolly_branch.create_branch('bt', 'tmp');
ROLLBACK;
DO $$ BEGIN
	IF EXISTS (SELECT FROM prolly_branch.files WHERE branch = 'tmp') THEN
		RAISE EXCEPTION 'tmp left behind';
	END IF;
END $$;

-- 5. VACUUM and ANALYZE on a branch, then switching away and back.
DELETE FROM bt.item WHERE id % 3 = 0;
VACUUM ANALYZE bt.item;
UPDATE bt.seen SET fp = bt.fingerprint(false) WHERE label = 'b';
SELECT prolly_branch.checkout('bt', 'main');
SELECT bt.check('main after b vacuumed', (SELECT fp FROM bt.seen WHERE label = 'main'));
SELECT prolly_branch.checkout('bt', 'b');
SELECT bt.check('b vacuumed', (SELECT fp FROM bt.seen WHERE label = 'b'));

-- 6. A third branch from b, then dropping main's sibling.
SELECT prolly_branch.create_branch('bt', 'c');
SELECT prolly_branch.checkout('bt', 'c');
SELECT bt.check('c is b', (SELECT fp FROM bt.seen WHERE label = 'b'));
SELECT prolly_branch.checkout('bt', 'main');
SELECT prolly_branch.drop_branch('bt', 'c');
SELECT bt.check('main after c dropped', (SELECT fp FROM bt.seen WHERE label = 'main'));
\echo branch_test: all passed
