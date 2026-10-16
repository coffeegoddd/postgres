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

\getenv abs_srcdir PG_ABS_SRCDIR
\set vc_setup :abs_srcdir '/sql/vc_versioned_public.sql'
\i :vc_setup

SELECT 'commit start', length((dolt_commit('-Am', 'vc start', '--allow-empty'))[1]) = 64;
SELECT 'branch latin1', (dolt_branch('vc_latin1'))[1] = '0';
SELECT 'branch utf8', (dolt_branch('vc_utf8'))[1] = '0';
SELECT 'branch other', (dolt_branch('vc_other'))[1] = '0';

SELECT 'checkout latin1', (dolt_checkout('vc_latin1'))[1] = '0';
SET client_encoding = 'LATIN1';
CREATE TABLE vc_text (t text, j jsonb, n name);
-- (the raw byte 0xe9 in this line, which is Ã© in LATIN1)
INSERT INTO vc_text VALUES ('é', jsonb_build_object('k', 'é'), 'é');
RESET client_encoding;
SELECT 'commit latin1', length((dolt_commit('-Am', 'vc latin1'))[1]) = 64;

SELECT 'checkout utf8', (dolt_checkout('vc_utf8'))[1] = '0';
SET client_encoding = 'UTF8';
CREATE TABLE vc_text (t text, j jsonb, n name);
-- (Ã© as this file has it, in UTF-8)
INSERT INTO vc_text VALUES ('Ã©', jsonb_build_object('k', 'Ã©'), 'Ã©');
RESET client_encoding;
SELECT 'commit utf8', length((dolt_commit('-Am', 'vc utf8'))[1]) = 64;
SELECT 'same root across client encodings',
	   prolly_root_hash('vc_latin1') = prolly_root_hash('vc_utf8');

SELECT 'checkout other', (dolt_checkout('vc_other'))[1] = '0';
CREATE TABLE vc_text (t text, j jsonb, n name);
INSERT INTO vc_text VALUES (chr(232), jsonb_build_object('k', chr(233)), chr(233));
SELECT 'commit other', length((dolt_commit('-Am', 'vc other'))[1]) = 64;
SELECT 'other content: different root',
	   prolly_root_hash('vc_other') <> prolly_root_hash('vc_utf8');
SELECT 'back to main', (dolt_checkout('main'))[1] = '0';

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
SELECT length((dolt_commit('-Am', 'db1'))[1]) = 64 AS committed \gset
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
SELECT length((dolt_commit('-Am', 'db2'))[1]) = 64 AS committed \gset
SELECT 'different oids',
	   (SELECT oid FROM pg_class WHERE relname = 't1') <> :vc_db1_t1;
SELECT 'same root across databases', prolly_root_hash('main') = :'vc_db1_root';
UPDATE t1 SET v = 'changed' WHERE id = 1;
SELECT length((dolt_commit('-Am', 'db2 changed'))[1]) = 64 AS committed \gset
SELECT 'other database content: different root', prolly_root_hash('main') <> :'vc_db1_root';
\c :vc_home
DROP DATABASE vc_db1;
DROP DATABASE vc_db2;
