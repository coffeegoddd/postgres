/* src/test/modules/test_prolly/test_prolly_ext_table--1.0.sql */

\echo Use "CREATE EXTENSION test_prolly_ext_table" to load this file. \quit

CREATE TABLE @extschema@.test_prolly_ext_rows (v int PRIMARY KEY);
INSERT INTO @extschema@.test_prolly_ext_rows VALUES (1);
SELECT pg_catalog.pg_extension_config_dump('@extschema@.test_prolly_ext_rows', 'WHERE v > 1');
CREATE FUNCTION @extschema@.test_prolly_ext_count() RETURNS bigint
  LANGUAGE sql AS 'SELECT count(*) FROM @extschema@.test_prolly_ext_rows';
