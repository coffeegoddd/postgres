/* src/test/modules/test_prolly/test_prolly--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_prolly" to load this file. \quit

-- Every registered implementation of every prolly interface.
CREATE FUNCTION prolly_implementations(
	OUT interface text,
	OUT name text,
	OUT impl_id int4,
	OUT format_version int4,
	OUT description text)
RETURNS SETOF record
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Run the conformance suite of each interface against its implementations.
-- NULL arguments mean all interfaces / all implementations.
CREATE FUNCTION prolly_conformance(
	only_interface text DEFAULT NULL,
	only_implementation text DEFAULT NULL,
	OUT interface text,
	OUT implementation text,
	OUT test text,
	OUT result text)
RETURNS SETOF record
AS 'MODULE_PATHNAME' LANGUAGE C;
