--
-- Canonical form: the session's client_encoding doesn't affect what is
-- stored, and two databases holding the same content have the same root
-- hash although their objects have different OIDs (objects are identified
-- by name in canonical form, never by OID).
--
\set ECHO none
\pset tuples_only on
\pset format unaligned
SET client_min_messages = warning;

-- Branches hold the tables of versioned schemas: public is one.
SELECT NOT nspversioned AS vc_make_versioned FROM pg_namespace
WHERE nspname = 'public' \gset
\if :vc_make_versioned
DROP SCHEMA public CASCADE;
CREATE SCHEMA public VERSIONED;
\endif

SELECT 'commit start', dolt_commit('-Am', 'vc start', '--allow-empty') IS NOT NULL;
SELECT 'branch latin1', dolt_branch('vc_latin1') IS NOT NULL;
SELECT 'branch utf8', dolt_branch('vc_utf8') IS NOT NULL;
SELECT 'branch other', dolt_branch('vc_other') IS NOT NULL;

SELECT 'checkout latin1', dolt_checkout('vc_latin1') IS NOT NULL;
SET client_encoding = 'LATIN1';
CREATE TABLE vc_text (t text, j jsonb, n name);
INSERT INTO vc_text VALUES (chr(233), jsonb_build_object('k', chr(233)), chr(233));
RESET client_encoding;
SELECT 'commit latin1', dolt_commit('-Am', 'vc latin1') IS NOT NULL;

SELECT 'checkout utf8', dolt_checkout('vc_utf8') IS NOT NULL;
SET client_encoding = 'UTF8';
CREATE TABLE vc_text (t text, j jsonb, n name);
INSERT INTO vc_text VALUES (chr(233), jsonb_build_object('k', chr(233)), chr(233));
RESET client_encoding;
SELECT 'commit utf8', dolt_commit('-Am', 'vc utf8') IS NOT NULL;
SELECT 'same root across client encodings',
	   prolly_root_hash('vc_latin1') = prolly_root_hash('vc_utf8');

SELECT 'checkout other', dolt_checkout('vc_other') IS NOT NULL;
CREATE TABLE vc_text (t text, j jsonb, n name);
INSERT INTO vc_text VALUES (chr(232), jsonb_build_object('k', chr(233)), chr(233));
SELECT 'commit other', dolt_commit('-Am', 'vc other') IS NOT NULL;
SELECT 'other content: different root',
	   prolly_root_hash('vc_other') <> prolly_root_hash('vc_utf8');
SELECT 'back to main', dolt_checkout('main') IS NOT NULL;

-- Two new databases: the same content, created in different orders, with
-- extra objects created and dropped in one so that OIDs differ.
\set vc_home :DBNAME
CREATE DATABASE vc_db1 TEMPLATE template0;
CREATE DATABASE vc_db2 TEMPLATE template0;
\c vc_db1
SET client_min_messages = warning;
DROP SCHEMA public;
CREATE SCHEMA public VERSIONED;
CREATE TABLE t1 (id int PRIMARY KEY, v text);
CREATE TABLE t2 (k text, n numeric);
INSERT INTO t1 SELECT g, md5(g::text) FROM generate_series(1, 50) g;
INSERT INTO t2 VALUES ('a', 1.5), ('b', NULL);
SELECT dolt_commit('-Am', 'db1') IS NOT NULL AS committed \gset
SELECT prolly_root_hash('main') AS vc_db1_root \gset
SELECT oid AS vc_db1_t1 FROM pg_class WHERE relname = 't1' \gset
\c vc_db2
SET client_min_messages = warning;
DROP SCHEMA public;
CREATE SCHEMA public VERSIONED;
CREATE TABLE junk1 (x int);
CREATE TABLE junk2 (x int);
DROP TABLE junk1, junk2;
CREATE TABLE t2 (k text, n numeric);
INSERT INTO t2 VALUES ('b', NULL), ('a', 1.5);
CREATE TABLE t1 (id int PRIMARY KEY, v text);
INSERT INTO t1 SELECT g, md5(g::text) FROM generate_series(50, 1, -1) g;
SELECT dolt_commit('-Am', 'db2') IS NOT NULL AS committed \gset
SELECT 'different oids',
	   (SELECT oid FROM pg_class WHERE relname = 't1') <> :vc_db1_t1;
SELECT 'same root across databases', prolly_root_hash('main') = :'vc_db1_root';
UPDATE t1 SET v = 'changed' WHERE id = 1;
SELECT dolt_commit('-Am', 'db2 changed') IS NOT NULL AS committed \gset
SELECT 'other content: different root', prolly_root_hash('main') <> :'vc_db1_root';
\c :vc_home
DROP DATABASE vc_db1;
DROP DATABASE vc_db2;
