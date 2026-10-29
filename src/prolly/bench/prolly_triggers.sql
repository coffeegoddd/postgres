-- What a source database's working set needs beyond prolly_tags.sql.
-- Every DDL statement gives new columns their tags and, inside its own
-- transaction, writes to the WAL stream:
--
--   prolly.layout   every table's codec layout after the statement, so rows
--                   that follow it in the stream are read with the new one
--   prolly.rows     the whole contents of each table whose rows' text the
--                   statement may have changed without decoded changes of
--                   their own (ALTER TABLE rewrites and fast defaults,
--                   CREATE TABLE AS, ALTER TYPE); the decoder replaces the
--                   table with them
--
-- DDL holds a shared advisory lock to the end of its transaction; a Dolt
-- commit takes it exclusively for the instant it exports a snapshot and
-- writes its barrier, so definitions dumped at that snapshot are those in
-- force at the barrier.  Tables without a primary key are given REPLICA
-- IDENTITY FULL, since their rows are keyed by the whole old row.
--
-- @LAYOUT@ is replaced with the layout query the capture uses.

CREATE FUNCTION prolly.ddl_start() RETURNS event_trigger LANGUAGE plpgsql AS $$
BEGIN
	PERFORM pg_advisory_xact_lock_shared(4242);
END
$$;

CREATE FUNCTION prolly.ddl_end() RETURNS event_trigger LANGUAGE plpgsql
	SET TimeZone = 'UTC' SET DateStyle = 'ISO' SET IntervalStyle = 'postgres'
	SET extra_float_digits = 1 SET bytea_output = 'hex' SET search_path = ''
AS $body$
DECLARE
	rec record;
	msg text := '';
	body text;
	cnt bigint;
BEGIN
	IF current_setting('prolly.busy', true) = 'on' THEN
		RETURN;
	END IF;
	PERFORM set_config('prolly.busy', 'on', true);
	PERFORM prolly.assign_tags();

	FOR rec IN SELECT c.oid FROM pg_class c JOIN pg_namespace ns ON ns.oid = c.relnamespace
		WHERE ns.nspname = 'rt' AND c.relkind = 'r' AND c.relpersistence = 'p'
			AND c.relreplident = 'd'
			AND NOT EXISTS (SELECT 1 FROM pg_index i WHERE i.indrelid = c.oid AND i.indisprimary)
	LOOP
		EXECUTE format('ALTER TABLE %s REPLICA IDENTITY FULL', rec.oid::regclass);
	END LOOP;

	-- Records: oid, persistence, name length, name, layout length, layout.
	FOR rec IN @LAYOUT@
	LOOP
		msg := msg || format('%s %s %s %s %s %s', rec.oid, rec.relpersistence,
							 octet_length(rec.relname), rec.relname,
							 octet_length(rec.layout), rec.layout);
	END LOOP;
	PERFORM pg_logical_emit_message(true, 'prolly.layout', msg);

	FOR rec IN
		WITH RECURSIVE hit(oid) AS (
			SELECT d.objid FROM pg_event_trigger_ddl_commands() d
			WHERE d.object_type = 'table'
				AND d.command_tag IN ('ALTER TABLE', 'CREATE TABLE AS', 'SELECT INTO')
			UNION
			SELECT a.attrelid FROM pg_event_trigger_ddl_commands() d
			JOIN pg_attribute a ON a.atttypid = d.objid
			WHERE d.object_type = 'type' AND d.command_tag = 'ALTER TYPE'
			UNION
			SELECT i.inhrelid FROM pg_inherits i JOIN hit h ON i.inhparent = h.oid)
		SELECT l.* FROM (@LAYOUT@) l
		WHERE l.oid IN (SELECT oid FROM hit) AND l.relpersistence = 'p' AND l.rowexpr IS NOT NULL
	LOOP
		EXECUTE format('SELECT count(*), coalesce(string_agg(%s, E''\n''), '''') FROM ONLY %s',
					   rec.rowexpr, rec.oid::regclass) INTO cnt, body;
		PERFORM pg_logical_emit_message(true, 'prolly.rows', rec.oid || ' ' || cnt || E'\n' || body);
	END LOOP;

	PERFORM set_config('prolly.busy', 'off', true);
END
$body$;

CREATE EVENT TRIGGER prolly_ddl_start ON ddl_command_start EXECUTE FUNCTION prolly.ddl_start();
CREATE EVENT TRIGGER prolly_ddl_end ON ddl_command_end EXECUTE FUNCTION prolly.ddl_end();

-- The commits of record: a Dolt commit exists once its row is here.  Its
-- chunks were synced to the store's journal first; the replication slot
-- lets go of the WAL behind its barrier only after.  tables is what the
-- stream's decoder needs to pick up from it.
CREATE TABLE prolly.roots (
	seq int PRIMARY KEY,
	state bytea NOT NULL,		-- the repo-state chunk
	rv bytea NOT NULL,			-- the commit's root value
	barrier pg_lsn NOT NULL,
	journal_end bigint NOT NULL,
	tables text NOT NULL
);
