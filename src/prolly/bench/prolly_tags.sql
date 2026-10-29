-- Column tags: a column's identity across renames and retypes, as the
-- commits know it.  A tag is given when a column first appears, from its
-- table's name, its own name and its type, so two branches adding the same
-- column agree on its tag; a rebuilt database is given the tags its commit
-- holds.  Every database a working set is captured from has this.

-- Installing this runs no user code either: in one transaction, with the
-- user's event triggers enabled ALWAYS or REPLICA off until it commits (as
-- prolly.quiet() does once it exists, at the end of this file).
BEGIN;
SET LOCAL session_replication_role = replica;
DO $$
DECLARE
	r record;
	off text := '';
BEGIN
	FOR r IN SELECT evtname, evtenabled FROM pg_event_trigger WHERE evtenabled IN ('A', 'R') LOOP
		EXECUTE format('ALTER EVENT TRIGGER %I DISABLE', r.evtname);
		off := off || r.evtenabled::text || r.evtname || chr(10);
	END LOOP;
	PERFORM set_config('prolly.install_quieted', off, true);
END $$;

-- Rows are written back through its row apply, which runs no user code.
SET LOCAL client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS prolly_pages SCHEMA public;
RESET client_min_messages;

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

-- Version control runs no user code.  A checkout, merge or move writes rows
-- and DDL that commits already hold, trigger effects included; firing user
-- triggers, rules or event triggers again would apply them twice.  Rows go
-- through prolly_apply_insert and prolly_apply_delete, which run none.  For
-- everything else, prolly.quiet() turns off, inside the caller's
-- transaction, what session_replication_role = replica (which it also sets)
-- leaves on: triggers, rules and event triggers enabled ALWAYS or REPLICA.
-- prolly.unquiet() turns them back on before the transaction commits, so no
-- other session ever sees them off and the catalog ends as it began.  It
-- goes by object id: one the operation dropped stays dropped, and one it
-- created again keeps the state its definition gives it.
CREATE TABLE prolly.quieted (
	kind "char" NOT NULL,			-- t trigger, r rule, e event trigger
	objid oid NOT NULL,
	mode "char" NOT NULL			-- A always or R replica, as found
);

CREATE FUNCTION prolly.quiet() RETURNS void LANGUAGE plpgsql AS $$
DECLARE
	r record;
BEGIN
	PERFORM set_config('session_replication_role', 'replica', true);
	-- event triggers first: turning the others off is DDL they would see
	FOR r IN SELECT e.oid, e.evtname, e.evtenabled FROM pg_event_trigger e
			 WHERE e.evtenabled IN ('A', 'R') AND e.evtname NOT LIKE 'prolly\_%' LOOP
		INSERT INTO prolly.quieted VALUES ('e', r.oid, r.evtenabled);
		EXECUTE format('ALTER EVENT TRIGGER %I DISABLE', r.evtname);
	END LOOP;
	FOR r IN SELECT t.oid, t.tgrelid, t.tgname, t.tgenabled FROM pg_trigger t
			 WHERE NOT t.tgisinternal AND t.tgenabled IN ('A', 'R') LOOP
		INSERT INTO prolly.quieted VALUES ('t', r.oid, r.tgenabled);
		EXECUTE format('ALTER TABLE %s DISABLE TRIGGER %I', r.tgrelid::regclass, r.tgname);
	END LOOP;
	FOR r IN SELECT w.oid, w.ev_class, w.rulename, w.ev_enabled FROM pg_rewrite w
			 WHERE w.rulename <> '_RETURN' AND w.ev_enabled IN ('A', 'R') LOOP
		INSERT INTO prolly.quieted VALUES ('r', r.oid, r.ev_enabled);
		EXECUTE format('ALTER TABLE %s DISABLE RULE %I', r.ev_class::regclass, r.rulename);
	END LOOP;
END $$;

CREATE FUNCTION prolly.unquiet() RETURNS void LANGUAGE plpgsql AS $$
DECLARE
	r record;
BEGIN
	FOR r IN SELECT q.mode, t.tgrelid, t.tgname FROM prolly.quieted q
			 JOIN pg_trigger t ON t.oid = q.objid
			 WHERE q.kind = 't' AND t.tgenabled = 'D' LOOP
		EXECUTE format('ALTER TABLE %s ENABLE %s TRIGGER %I', r.tgrelid::regclass,
					   CASE r.mode WHEN 'A' THEN 'ALWAYS' ELSE 'REPLICA' END, r.tgname);
	END LOOP;
	FOR r IN SELECT q.mode, w.ev_class, w.rulename FROM prolly.quieted q
			 JOIN pg_rewrite w ON w.oid = q.objid
			 WHERE q.kind = 'r' AND w.ev_enabled = 'D' LOOP
		EXECUTE format('ALTER TABLE %s ENABLE %s RULE %I', r.ev_class::regclass,
					   CASE r.mode WHEN 'A' THEN 'ALWAYS' ELSE 'REPLICA' END, r.rulename);
	END LOOP;
	-- event triggers last, for the same reason they went first
	FOR r IN SELECT q.mode, e.evtname FROM prolly.quieted q
			 JOIN pg_event_trigger e ON e.oid = q.objid
			 WHERE q.kind = 'e' AND e.evtenabled = 'D' LOOP
		EXECUTE format('ALTER EVENT TRIGGER %I ENABLE %s', r.evtname,
					   CASE r.mode WHEN 'A' THEN 'ALWAYS' ELSE 'REPLICA' END);
	END LOOP;
	DELETE FROM prolly.quieted;
END $$;

DO $$
DECLARE
	line text;
BEGIN
	FOREACH line IN ARRAY string_to_array(rtrim(current_setting('prolly.install_quieted'), chr(10)), chr(10)) LOOP
		EXECUTE format('ALTER EVENT TRIGGER %I ENABLE %s', substr(line, 2),
					   CASE left(line, 1) WHEN 'A' THEN 'ALWAYS' ELSE 'REPLICA' END);
	END LOOP;
END $$;
COMMIT;
