/* src/test/modules/test_prolly/test_prolly--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_prolly" to load this file. \quit

-- Run the unit tests of the engine's code, or those of one suite.
CREATE FUNCTION test_prolly_unit_tests(suite_name text DEFAULT NULL,
	OUT suite text, OUT test text, OUT result text,
	OUT detail text, OUT location text)
RETURNS SETOF record
AS 'MODULE_PATHNAME' LANGUAGE C VOLATILE;

-- Run every interface's conformance suite, or one interface's, against each
-- registered implementation of it.
CREATE FUNCTION test_prolly_conformance(iface_name text DEFAULT NULL,
	OUT iface text, OUT implementation text, OUT test text,
	OUT result text, OUT detail text, OUT location text)
RETURNS SETOF record
AS 'MODULE_PATHNAME' LANGUAGE C VOLATILE;
