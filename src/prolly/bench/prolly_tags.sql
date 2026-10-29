-- Column tags: a column's identity across renames and retypes, as the
-- commits know it.  A tag is given when a column first appears, from its
-- table's name, its own name and its type, so two branches adding the same
-- column agree on its tag; a rebuilt database is given the tags its commit
-- holds.  Every database a working set is captured from has this.

CREATE SCHEMA prolly;

CREATE TABLE prolly.tags (
	relid oid,
	attnum int2,
	tag int8 NOT NULL,
	PRIMARY KEY (relid, attnum)
);

-- Tags for columns that have none; forgets those of dropped columns.
CREATE FUNCTION prolly.assign_tags() RETURNS void LANGUAGE plpgsql
	SET search_path = ''
AS $$
DECLARE
	c record;
	t int8;
	n int;
BEGIN
	DELETE FROM prolly.tags g WHERE NOT EXISTS (
		SELECT 1 FROM pg_attribute a
		WHERE a.attrelid = g.relid AND a.attnum = g.attnum AND NOT a.attisdropped);
	FOR c IN
		SELECT a.attrelid, a.attnum, cl.relname, a.attname,
			format_type(a.atttypid, a.atttypmod) AS typ
		FROM pg_attribute a
		JOIN pg_class cl ON cl.oid = a.attrelid
		JOIN pg_namespace ns ON ns.oid = cl.relnamespace
		WHERE ns.nspname = 'rt' AND cl.relkind IN ('r', 'p') AND a.attnum > 0
			AND NOT a.attisdropped
			AND NOT EXISTS (SELECT 1 FROM prolly.tags g
							WHERE g.relid = a.attrelid AND g.attnum = a.attnum)
		ORDER BY a.attrelid, a.attnum
	LOOP
		n := 0;
		LOOP
			t := ('x' || substr(md5(c.relname || '/' || c.attname || '/' || c.typ || '/' || n),
								1, 15))::bit(60)::int8;
			EXIT WHEN NOT EXISTS (SELECT 1 FROM prolly.tags g
								  WHERE g.relid = c.attrelid AND g.tag = t);
			n := n + 1;
		END LOOP;
		INSERT INTO prolly.tags VALUES (c.attrelid, c.attnum, t);
	END LOOP;
END
$$;

-- A value as COPY text writes it, given its output function's text.
CREATE FUNCTION prolly.esc(is_null bool, t text) RETURNS text LANGUAGE sql IMMUTABLE AS $$
	SELECT CASE WHEN is_null THEN '\N' ELSE
		replace(replace(replace(replace(replace(replace(replace(t,
			'\', '\\'), chr(10), '\n'), chr(13), '\r'), chr(9), '\t'),
			chr(8), '\b'), chr(12), '\f'), chr(11), '\v') END
$$;

-- Constraint violations a merge left in this working set: a row per
-- violating row (rowkey: its primary key values, comma separated, or the
-- whole row for a table without one), and a row per constraint (rowkey
-- NULL) whose detail is its object key and definition, columns by tag, to
-- apply once the rows are fixed.  Foreign keys and checks stay as NOT VALID
-- meanwhile, so new rows are checked; unique constraints wait.
CREATE TABLE prolly.violations (
	tbl text NOT NULL,
	cons text NOT NULL,
	kind text NOT NULL,
	rowkey text,
	detail text NOT NULL
);
