/* src/test/modules/test_prolly/test_prolly_a--1.0.sql */

\echo Use "CREATE EXTENSION test_prolly_a" to load this file. \quit

CREATE FUNCTION test_prolly_a_one() RETURNS int LANGUAGE sql AS 'SELECT 1';
