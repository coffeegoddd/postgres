/* src/test/modules/test_prolly/test_prolly_ext_config--1.0.sql */

\echo Use "CREATE EXTENSION test_prolly_ext_config" to load this file. \quit

-- A configuration table with values whose text forms follow the session's
-- settings, an identity, a generated column and row security; the script's
-- own row (id 1) isn't dumped.  A keyless one.  And a configuration
-- sequence.
CREATE TABLE @extschema@.test_prolly_cfg (
	id int GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
	at timestamptz,
	day date,
	ratio float8,
	doubled int GENERATED ALWAYS AS (id * 2) STORED);
INSERT INTO @extschema@.test_prolly_cfg (at, day, ratio)
	VALUES ('2000-01-01 00:00 UTC', '2000-01-01', 0.5);
ALTER TABLE @extschema@.test_prolly_cfg ENABLE ROW LEVEL SECURITY;
CREATE POLICY test_prolly_cfg_none ON @extschema@.test_prolly_cfg USING (false);
SELECT pg_catalog.pg_extension_config_dump('@extschema@.test_prolly_cfg', 'WHERE id > 1');
-- a keyless configuration table, every row dumped
CREATE TABLE @extschema@.test_prolly_cfg_tags (tag text);
SELECT pg_catalog.pg_extension_config_dump('@extschema@.test_prolly_cfg_tags', '');
CREATE SEQUENCE @extschema@.test_prolly_cfg_seq;
SELECT pg_catalog.pg_extension_config_dump('@extschema@.test_prolly_cfg_seq', '');
CREATE FUNCTION @extschema@.test_prolly_cfg_count() RETURNS bigint
  LANGUAGE sql SECURITY DEFINER AS 'SELECT count(*) FROM @extschema@.test_prolly_cfg';
