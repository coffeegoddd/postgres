-- A versioned table's values out of line go to its database's chunk store,
-- named by content (hash pointers), not to its TOAST relation.  The
-- toaster's passes are as for heap: what fits stays inline (compressed if
-- it compresses), what is moved out is stored uncompressed, a pointer a
-- row is given is kept as it is, and only a value bigger than a pointer is
-- moved out.  A collection keeps every value the working set names.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;

CREATE SCHEMA tc VERSIONED;
CREATE TABLE tc.docs (id int PRIMARY KEY, n int, t text, b bytea, j jsonb);
INSERT INTO tc.docs
SELECT g, 0,
	   string_agg(md5(g || '/' || i), '' ORDER BY i),
	   decode(string_agg(md5(g || '|' || i), '' ORDER BY i), 'hex'),
	   jsonb_build_object('g', g, 'parts', jsonb_agg(md5(g || '-' || i)))
FROM generate_series(1, 3) g, generate_series(1, 3000) i GROUP BY g;
CREATE VIEW tc.pointers AS
SELECT id, (test_toast_pointer(t)).root AS t, (test_toast_pointer(b)).root AS b,
	   (test_toast_pointer(j)).root AS j
FROM tc.docs;
CREATE VIEW tc.digest AS
SELECT md5(string_agg(id || ':' || n || ':' || md5(t) || ':' || md5(b) || ':' || md5(j::text),
					  ',' ORDER BY id)) AS d
FROM tc.docs;

-- Each moved out by content, uncompressed, in the form its type stores;
-- nothing in the TOAST relation.
SELECT id, (test_toast_pointer(t)).form AS t_form, (test_toast_pointer(b)).form AS b_form,
	   (test_toast_pointer(j)).form AS j_form,
	   (test_toast_pointer(t)).stored = octet_length(t) AS t_uncompressed,
	   pg_column_compression(t) IS NULL AS no_compression
FROM tc.docs ORDER BY id;
SELECT pg_relation_size(reltoastrelid) AS toast_size FROM pg_class
WHERE oid = 'tc.docs'::regclass;
SELECT substr(t, 9000, 12) = substr(md5('2/282'), 8, 12) AS slice FROM tc.docs WHERE id = 2;

-- An update of another column keeps the pointers; a value copied to
-- another row, or another table, is the same pointer, not stored again.
CREATE TEMP TABLE before_update AS SELECT * FROM tc.pointers;
UPDATE tc.docs SET n = n + 1;
SELECT count(*) AS kept FROM tc.pointers p JOIN before_update b USING (id, t, b, j);
CREATE TABLE tc.copies (id int PRIMARY KEY, t text);
INSERT INTO tc.copies SELECT id, t FROM tc.docs;
INSERT INTO tc.copies SELECT 10 + id, t FROM tc.docs;
SELECT count(*) AS same_tree FROM tc.copies c JOIN tc.pointers p ON p.id = c.id % 10
WHERE (test_toast_pointer(c.t)).root = p.t;
-- a stock table gets the value itself
CREATE TABLE public.stock (id int, t text);
INSERT INTO public.stock SELECT id, t FROM tc.docs;
SELECT count(*) FILTER (WHERE test_toast_pointer(t) IS NULL) AS not_by_content,
	   bool_and(pg_column_toast_chunk_id(t) IS NOT NULL) AS in_toast
FROM public.stock;
DROP TABLE public.stock;

-- A value that fits stays inline, compressed if it compresses; one that
-- doesn't is moved out uncompressed (the toaster gives the store the
-- value it compressed, not the compressed bytes).
CREATE TABLE tc.fit (id int PRIMARY KEY, t text);
INSERT INTO tc.fit VALUES (1, repeat('a', 1500)), (2, repeat('ab', 50000)),
	(3, (SELECT string_agg(md5(i::text), '') FROM generate_series(1, 40) i)),
	(4, (SELECT string_agg(repeat(md5(i::text), 4), '') FROM generate_series(1, 2000) i));
SELECT id, test_toast_pointer(t) IS NULL AS inline, pg_column_compression(t) AS compressed,
	   (test_toast_pointer(t)).stored AS stored, octet_length(t) AS raw
FROM tc.fit ORDER BY id;

-- MAIN values go out only past the larger MAIN target; PLAIN never.
CREATE TABLE tc.main (id int PRIMARY KEY, a text STORAGE MAIN, p text STORAGE PLAIN);
INSERT INTO tc.main
SELECT 1, string_agg(md5(i::text), ''), 'p' FROM generate_series(1, 100) i;
INSERT INTO tc.main
SELECT 2, string_agg(md5(i::text), ''), 'p' FROM generate_series(1, 400) i;
SELECT id, test_toast_pointer(a) IS NOT NULL AS out_of_line FROM tc.main ORDER BY id;

-- A value no bigger than a hash pointer (42 bytes) isn't moved out into
-- one, however long its row.
CREATE TABLE tc.narrow (id int PRIMARY KEY);
DO $$
BEGIN
	EXECUTE (SELECT format('ALTER TABLE tc.narrow %s',
						   string_agg(format('ADD COLUMN c%s text STORAGE EXTERNAL', g), ', '))
			 FROM generate_series(1, 150) g);
	EXECUTE (SELECT format('INSERT INTO tc.narrow VALUES (1, %s)',
						   string_agg(quote_literal(repeat(chr(64 + g % 26), 40)), ', '))
			 FROM generate_series(1, 150) g);
END
$$;
SELECT count(*) FILTER (WHERE test_toast_pointer(c) IS NOT NULL) AS moved_out
FROM tc.narrow, LATERAL (VALUES (c1), (c75), (c150)) v(c);

-- Committed and reset, the values come back; a collection keeps every
-- value the working set names.
SELECT d AS before FROM tc.digest \gset
SELECT length(dolt_commit('-Am', 'by content')::text) AS committed;
UPDATE tc.docs SET t = 'changed', b = 'changed', j = '{}';
SELECT dolt_reset('--hard') IS NOT NULL AS reset;
SELECT d = :'before' AS same FROM tc.digest;
UPDATE tc.docs SET t = t || 'x' WHERE id = 1;
SELECT kept > 0 AS collected FROM test_chunk_gc(false);
SELECT md5(t) = md5((SELECT string_agg(md5('1/' || i), '' ORDER BY i)
					 FROM generate_series(1, 3000) i) || 'x') AS working_value_kept
FROM tc.docs WHERE id = 1;

DROP SCHEMA tc CASCADE;
