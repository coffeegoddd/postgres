/* src/test/modules/test_prolly/test_prolly_ext_readd--1.0.sql */

\echo Use "CREATE EXTENSION test_prolly_ext_readd" to load this file. \quit

CREATE FUNCTION @extschema@.test_prolly_readded() RETURNS int LANGUAGE sql AS 'SELECT 3';
ALTER EXTENSION test_prolly_ext_readd DROP FUNCTION @extschema@.test_prolly_readded();
ALTER EXTENSION test_prolly_ext_readd ADD FUNCTION @extschema@.test_prolly_readded();
