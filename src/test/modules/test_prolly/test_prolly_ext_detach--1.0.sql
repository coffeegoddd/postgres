/* src/test/modules/test_prolly/test_prolly_ext_detach--1.0.sql */

\echo Use "CREATE EXTENSION test_prolly_ext_detach" to load this file. \quit

CREATE FUNCTION @extschema@.test_prolly_detached() RETURNS int LANGUAGE sql AS 'SELECT 1';
CREATE FUNCTION @extschema@.test_prolly_kept() RETURNS int LANGUAGE sql AS 'SELECT 2';
ALTER EXTENSION test_prolly_ext_detach DROP FUNCTION @extschema@.test_prolly_detached();
