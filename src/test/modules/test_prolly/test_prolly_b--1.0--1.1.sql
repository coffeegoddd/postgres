/* src/test/modules/test_prolly/test_prolly_b--1.0--1.1.sql */

\echo Use "ALTER EXTENSION test_prolly_b UPDATE TO '1.1'" to load this file. \quit

CREATE FUNCTION test_prolly_b_two() RETURNS int LANGUAGE sql AS 'SELECT 2';
