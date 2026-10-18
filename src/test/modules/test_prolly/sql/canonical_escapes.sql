-- Names and expressions hold tabs, newlines and backslashes as legal
-- Postgres: a commit holds them, and a move (which makes a branch's objects
-- from a commit's canonical form) makes them as they were.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;

CREATE SCHEMA esc VERSIONED;
CREATE TABLE esc.base (id int PRIMARY KEY);
SELECT length(dolt_commit('-Am', 'base')::text) AS committed;
SELECT dolt_branch('esc_reset');
SELECT dolt_branch('esc_merge');

CREATE TYPE esc.U&"tab\0009type" AS ENUM (E'a\tlabel', E'new\nline', E'back\\slash');
CREATE TYPE esc.pair AS (U&"tab\0009field" int, U&"new\000Aline" text);
CREATE TABLE esc.U&"t\000Abl" (
	id int PRIMARY KEY,
	U&"col\0009umn" esc.U&"tab\0009type" DEFAULT E'a\tlabel',
	note text DEFAULT E'line one\nline two\\',
	p esc.pair,
	CONSTRAINT U&"chk\000Aname" CHECK (note <> E'\t'),
	CONSTRAINT enum_chk CHECK (U&"col\0009umn" <> E'new\nline')
);
CREATE INDEX U&"idx\0009name" ON esc.U&"t\000Abl" (note) WHERE note <> E'x\ty';
CREATE VIEW esc.U&"v\0009iew" AS SELECT id, note || E'\t' AS tabbed FROM esc.U&"t\000Abl";
CREATE FUNCTION esc.U&"f\0009n"(U&"a\000Arg" esc.U&"tab\0009type") RETURNS text
  LANGUAGE sql AS $$ SELECT E'\t' || $1::text $$;
CREATE SEQUENCE esc.U&"s\0009eq";
CREATE DOMAIN esc.tabbed AS text CONSTRAINT U&"no\0009tab" CHECK (VALUE <> E'\t');
CREATE COLLATION esc.ruled (provider = icu, locale = 'und', rules = E'&a < b\n&c < d');
CREATE TABLE esc.keyed (k text COLLATE esc.ruled PRIMARY KEY, d esc.tabbed);
INSERT INTO esc.keyed VALUES ('a', 'x'), ('b', 'y');
-- (a rule's definition is written over several lines)
CREATE RULE U&"r\0009ule" AS ON INSERT TO esc.base WHERE new.id < 0 DO INSTEAD NOTHING;
INSERT INTO esc.U&"t\000Abl" (id, p) VALUES (1, ROW(2, E'z\tz'));
SELECT length(dolt_commit('-Am', 'escapes')::text) AS committed;

CREATE FUNCTION pg_temp.show() RETURNS SETOF text LANGUAGE sql AS $$
	SELECT 'enum ' || enum_range(NULL::esc.U&"tab\0009type")::text
	UNION ALL
	SELECT 'attrs ' || string_agg(attname, ',' ORDER BY attnum) FROM pg_attribute
	  WHERE attrelid = 'esc.pair'::regclass AND attnum > 0
	UNION ALL
	SELECT 'row ' || t::text FROM esc.U&"t\000Abl" t
	UNION ALL
	SELECT 'check ' || conname || ' ' || pg_get_constraintdef(c.oid) FROM pg_constraint c
	  JOIN pg_class r ON r.oid = c.conrelid
	  WHERE r.relname = E't\nbl' AND contype = 'c'
	UNION ALL
	SELECT 'index ' || indexdef FROM pg_indexes WHERE schemaname = 'esc'
	UNION ALL
	SELECT 'view ' || v::text FROM esc.U&"v\0009iew" v
	UNION ALL
	SELECT 'fn ' || esc.U&"f\0009n"(E'new\nline')
	UNION ALL
	SELECT 'seq ' || relname FROM pg_class WHERE relkind = 'S' AND relnamespace = 'esc'::regnamespace
	UNION ALL
	SELECT 'rule ' || definition FROM pg_rules WHERE schemaname = 'esc'
	UNION ALL
	SELECT 'domain ' || conname || ' ' || pg_get_constraintdef(oid) FROM pg_constraint
	  WHERE contypid = 'esc.tabbed'::regtype
	UNION ALL
	SELECT 'collation ' || collname || ' ' || colllocale || ' ' || collicurules
	  FROM pg_collation WHERE collnamespace = 'esc'::regnamespace
	UNION ALL
	SELECT 'keyed ' || string_agg((k COLLATE "C") || '=' || d, ',' ORDER BY k) FROM esc.keyed
	ORDER BY 1
$$;
SELECT * FROM pg_temp.show();

-- a reset to the commit makes every object from its canonical form
SELECT dolt_checkout('esc_reset');
SELECT dolt_reset('--hard', 'main');
SELECT * FROM dolt_status;
SELECT * FROM pg_temp.show();
INSERT INTO esc.U&"t\000Abl" (id) VALUES (2);
SELECT note FROM esc.U&"t\000Abl" WHERE id = 2;

-- a merge that brings them too
SELECT dolt_checkout('esc_merge');
CREATE TABLE esc.other (id int PRIMARY KEY);
SELECT length(dolt_commit('-Am', 'other')::text) AS committed;
SELECT dolt_merge('main') IS NOT NULL AS merged;
SELECT * FROM dolt_status;
SELECT * FROM pg_temp.show();
INSERT INTO esc.base VALUES (-1);		-- (the rule)
SELECT count(*) FROM esc.base;

-- names in dolt_status
CREATE TABLE esc.U&"tab\0009le" (id int PRIMARY KEY);
SELECT * FROM dolt_status;

SELECT dolt_checkout('main');

-- a commit of an older format (made before names were escaped) is
-- neither moved to nor merged
SELECT test_commit_of(test_root_without_format(test_fold_root()), 'older') AS older \gset
SELECT dolt_reset('--hard', :'older');

-- the type changed under a constraint that uses it: the constraint made
-- again, as it was
SELECT dolt_branch('esc_type');
ALTER TYPE esc.U&"tab\0009type" ADD VALUE 'more';
SELECT length(dolt_commit('-Am', 'more')::text) AS committed;
SELECT dolt_checkout('esc_type');
SELECT dolt_reset('--hard', 'main');
SELECT pg_get_constraintdef(c.oid) FROM pg_constraint c
  JOIN pg_class r ON r.oid = c.conrelid
  WHERE r.relname = E't\nbl' AND conname = 'enum_chk';
SELECT dolt_checkout('main');
SELECT dolt_branch('-D', 'esc_type');
SELECT dolt_branch('-D', 'esc_reset');
SELECT dolt_branch('-D', 'esc_merge');
DROP SCHEMA esc CASCADE;
SELECT length(dolt_commit('-Am', 'esc gone')::text) AS committed;
