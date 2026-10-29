/* contrib/prolly_chunkpages/prolly_chunkpages--1.0.sql */

\echo Use "CREATE EXTENSION prolly_chunkpages" to load this file. \quit

CREATE FUNCTION prolly_chunks_handler(internal) RETURNS table_am_handler
AS 'MODULE_PATHNAME' LANGUAGE C;
CREATE ACCESS METHOD prolly_chunks TYPE TABLE HANDLER prolly_chunks_handler;

CREATE FUNCTION cp_append(rel regclass, data bytea) RETURNS int8
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION cp_read(rel regclass, loc int8, len int4) RETURNS bytea
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION cp_end(rel regclass) RETURNS int8
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- The log, and the index from a chunk's hash to its bytes in it: made by
-- setup(), not by this script, so they are the database's own tables
-- (compaction replaces the log).
CREATE FUNCTION setup() RETURNS void LANGUAGE sql AS $$
	CREATE TABLE cp.pages () USING prolly_chunks;
	CREATE TABLE cp.idx (hash bytea PRIMARY KEY, loc int8 NOT NULL, len int4 NOT NULL);
$$;

CREATE FUNCTION put(d bytea) RETURNS bytea LANGUAGE sql AS $$
	INSERT INTO cp.idx SELECT h, cp.cp_append('cp.pages', d), length(d)
	  FROM (SELECT sha256(d) AS h) s
	 WHERE NOT EXISTS (SELECT FROM cp.idx WHERE hash = s.h);
	SELECT sha256(d)
$$;

CREATE FUNCTION get(h bytea) RETURNS bytea LANGUAGE sql STABLE AS $$
	SELECT cp.cp_read('cp.pages', loc, len) FROM cp.idx WHERE hash = h
$$;

-- Space of chunks dropped from the index comes back by rewriting the live
-- ones into a new log, in log order, and dropping the old.
CREATE FUNCTION compact() RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	CREATE TABLE cp.pages_new () USING prolly_chunks;
	CREATE TEMP TABLE moved ON COMMIT DROP AS
	SELECT hash, cp.cp_append('cp.pages_new', cp.cp_read('cp.pages', loc, len)) AS loc
	  FROM cp.idx ORDER BY loc;
	UPDATE cp.idx i SET loc = m.loc FROM moved m WHERE m.hash = i.hash;
	DROP TABLE cp.pages;
	ALTER TABLE cp.pages_new RENAME TO pages;
END $$;
