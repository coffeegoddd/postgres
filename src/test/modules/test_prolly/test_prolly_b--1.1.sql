/* src/test/modules/test_prolly/test_prolly_b--1.1.sql */

\echo Use "CREATE EXTENSION test_prolly_b" to load this file. \quit

CREATE FUNCTION test_prolly_b_one() RETURNS int LANGUAGE sql AS 'SELECT 1';
CREATE FUNCTION test_prolly_b_two() RETURNS int LANGUAGE sql AS 'SELECT 2';
