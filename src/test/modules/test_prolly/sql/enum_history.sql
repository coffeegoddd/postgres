-- Historical enum labels are read through their saved type definitions.
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;
CREATE SCHEMA eh VERSIONED;
CREATE TYPE eh.mood AS ENUM ('sad', 'ok', 'happy');
CREATE DOMAIN eh.dmood AS eh.mood;
CREATE TYPE eh.pair AS (m eh.mood, n int);
CREATE TABLE eh.t (id int PRIMARY KEY, m eh.mood, a eh.mood[], p eh.pair, d eh.dmood);
CREATE TABLE eh.k (m eh.mood PRIMARY KEY, v int);
CREATE TABLE eh.l (m eh.mood);
CREATE TABLE eh.big (id int PRIMARY KEY, a eh.mood[]);
INSERT INTO eh.t VALUES (1, 'sad', '{sad,ok}', '(sad,1)', 'sad'),
  (2, 'ok', '{ok}', '(ok,2)', 'ok');
INSERT INTO eh.k VALUES ('sad', 1), ('ok', 2);
INSERT INTO eh.l VALUES ('sad'), ('sad'), ('ok');
INSERT INTO eh.big SELECT 1, array_agg('sad'::eh.mood) FROM generate_series(1, 2000);
SELECT (dolt_commit('-Am', 'enum base'))[1] AS base \gset
SELECT dolt_branch('old_writer');
ALTER TYPE eh.mood RENAME VALUE 'sad' TO 'blue';
SELECT length(dolt_commit('-Am', 'rename')::text) AS committed;
-- Typed diffs use the current row type, including its current enum labels.
SELECT diff_type, from_row, to_row FROM dolt_diff(:'base', 'HEAD', NULL::eh.t);
SELECT diff_type, from_row, to_row FROM dolt_diff(:'base', 'HEAD', NULL::eh.k);
SELECT diff_type, from_row, to_row FROM dolt_diff(:'base', 'HEAD', NULL::eh.l);
SELECT count(*) AS big_changes FROM dolt_diff(:'base', 'HEAD', NULL::eh.big);
SELECT dolt_checkout('old_writer');
INSERT INTO eh.t VALUES (3, 'sad', '{sad,happy}', '(sad,3)', 'sad');
INSERT INTO eh.k VALUES ('happy', 3);
INSERT INTO eh.l VALUES ('sad');
SELECT length(dolt_commit('-am', 'old writer')::text) AS committed;
SELECT dolt_checkout('main');
SELECT diff_type, from_row, to_row FROM dolt_diff(:'base', 'old_writer', NULL::eh.t);
BEGIN;
SELECT (dolt_merge('old_writer', '-m', 'merged enum'))[2:4];
SELECT * FROM eh.t ORDER BY id;
SELECT * FROM eh.k ORDER BY m;
SELECT m, count(*) FROM eh.l GROUP BY m ORDER BY m;
SELECT test_fold_root() AS merged \gset
SELECT * FROM dolt_status;
ROLLBACK;
SELECT dolt_checkout('old_writer');
BEGIN;
SELECT (dolt_merge('main', '-m', 'merged enum reverse'))[2:4];
SELECT test_fold_root() = :'merged' AS same_merge;
SELECT * FROM eh.t ORDER BY id;
ROLLBACK;
SELECT dolt_checkout('main');
-- Conflicts are also decoded and resolved through the saved definitions.
UPDATE eh.t SET m = 'ok' WHERE id = 1;
SELECT length(dolt_commit('-am', 'ours edit')::text) AS committed;
SELECT dolt_checkout('old_writer');
UPDATE eh.t SET m = 'happy' WHERE id = 1;
SELECT length(dolt_commit('-am', 'theirs edit')::text) AS committed;
SELECT dolt_checkout('main');
BEGIN;
SELECT (dolt_merge('old_writer', '-m', 'enum conflict'))[2:4];
SELECT (base_row).m, (our_row).m, (their_row).m FROM dolt_conflicts(NULL::eh.t);
SELECT dolt_conflicts_resolve('--theirs', 'eh.t');
SELECT id, m FROM eh.t ORDER BY id;
ROLLBACK;
SELECT dolt_branch('-D', 'old_writer');
-- Additions preserve old labels by name, even when inserted before them.
CREATE TYPE eh.added AS ENUM ('a', 'b');
CREATE TABLE eh.adds (id int PRIMARY KEY, e eh.added);
INSERT INTO eh.adds VALUES (1, 'a');
SELECT (dolt_commit('-Am', 'before addition'))[1] AS add_base \gset
ALTER TYPE eh.added ADD VALUE 'x' BEFORE 'a';
-- (only the type changed: its tables' rows read the same)
SELECT * FROM dolt_status();
SELECT length(dolt_commit('-Am', 'addition')::text) AS committed;
SELECT count(*) AS changed FROM dolt_diff(:'add_base', 'HEAD', NULL::eh.adds);

-- Escaping and UTF-8 apply to the saved labels, including nested arrays.
CREATE TYPE eh."odd type" AS ENUM (E'a\tb', E'c\\d', 'é');
CREATE TABLE eh.odd (id int PRIMARY KEY, e eh."odd type"[]);
INSERT INTO eh.odd VALUES (1, ARRAY[E'a\tb', E'c\\d', 'é']::eh."odd type"[]);
SELECT (dolt_commit('-Am', 'odd labels'))[1] AS odd_base \gset
ALTER TYPE eh."odd type" RENAME VALUE E'a\tb' TO E'q\n''u';
SELECT length(dolt_commit('-Am', 'renamed odd label')::text) AS committed;
SELECT count(*) AS changed FROM dolt_diff(:'odd_base', 'HEAD', NULL::eh.odd);

-- A rename mixed with additions is ambiguous, even if the old label is
-- absent from the rows.  Refuse before applying any of the other side.
CREATE TYPE eh.amb AS ENUM ('a', 'b');
CREATE TABLE eh.ambiguous (id int PRIMARY KEY, e eh.amb);
INSERT INTO eh.ambiguous VALUES (1, 'b');
SELECT (dolt_commit('-Am', 'ambiguous base'))[1] AS amb_base \gset
SELECT dolt_branch('amb_writer');
ALTER TYPE eh.amb RENAME VALUE 'a' TO 'x';
ALTER TYPE eh.amb ADD VALUE 'y';
SELECT length(dolt_commit('-Am', 'rename and add')::text) AS committed;
SELECT count(*) FROM dolt_diff(:'amb_base', 'HEAD', NULL::eh.ambiguous);
SELECT dolt_checkout('amb_writer');
INSERT INTO eh.ambiguous VALUES (2, 'b');
SELECT length(dolt_commit('-am', 'ambiguous writer')::text) AS committed;
SELECT dolt_checkout('main');
SELECT test_fold_root() AS before_refusal \gset
SELECT dolt_merge('amb_writer');
SELECT test_fold_root() = :'before_refusal' AS unchanged;
SELECT * FROM eh.ambiguous ORDER BY id;
SELECT * FROM dolt_status;
SELECT dolt_branch('-D', 'amb_writer');

-- Reused names cannot be mistaken for unchanged enum members.
CREATE TYPE eh.reused AS ENUM ('a', 'b');
CREATE TABLE eh.reuse (id int PRIMARY KEY, e eh.reused);
INSERT INTO eh.reuse VALUES (1, 'b');
SELECT (dolt_commit('-Am', 'reuse base'))[1] AS reuse_base \gset
ALTER TYPE eh.reused RENAME VALUE 'a' TO 'tmp';
ALTER TYPE eh.reused RENAME VALUE 'b' TO 'a';
ALTER TYPE eh.reused RENAME VALUE 'tmp' TO 'b';
SELECT length(dolt_commit('-Am', 'swapped names')::text) AS committed;
SELECT count(*) FROM dolt_diff(:'reuse_base', 'HEAD', NULL::eh.reuse);

-- Renaming an enum does not supply a historical layout for an enclosing
-- composite that also changed.  Refuse that combination explicitly.
CREATE TYPE eh.ce AS ENUM ('a', 'b');
CREATE TYPE eh.container AS (e eh.ce, n int);
CREATE TABLE eh.contained (id int PRIMARY KEY, c eh.container);
INSERT INTO eh.contained VALUES (1, '(a,1)');
SELECT (dolt_commit('-Am', 'container base'))[1] AS container_base \gset
ALTER TYPE eh.ce RENAME VALUE 'a' TO 'x';
ALTER TYPE eh.container DROP ATTRIBUTE n CASCADE;
SELECT length(dolt_commit('-Am', 'container changed')::text) AS committed;
SELECT count(*) FROM dolt_diff(:'container_base', 'HEAD', NULL::eh.contained);

-- Likewise, ranges use their own binary receiver, not the nested codecs.
CREATE TYPE eh.range_enum AS ENUM ('a', 'b');
CREATE TYPE eh.enum_range AS RANGE (subtype = eh.range_enum);
CREATE TABLE eh.ranged (id int PRIMARY KEY, r eh.enum_range);
INSERT INTO eh.ranged VALUES (1, '[a,b]');
SELECT (dolt_commit('-Am', 'range base'))[1] AS range_base \gset
ALTER TYPE eh.range_enum RENAME VALUE 'a' TO 'x';
SELECT length(dolt_commit('-Am', 'range enum changed')::text) AS committed;
SELECT count(*) FROM dolt_diff(:'range_base', 'HEAD', NULL::eh.ranged);

-- The lost-column check must not compare old enum keys with today's
-- decoder before the historical rows have been converted.
CREATE TYPE eh.drop_enum AS ENUM ('a', 'b');
CREATE TABLE eh.dropped (e eh.drop_enum PRIMARY KEY, v int);
INSERT INTO eh.dropped VALUES ('a', 1);
SELECT length(dolt_commit('-Am', 'drop base')::text) AS committed;
SELECT dolt_branch('drop_writer');
ALTER TYPE eh.drop_enum RENAME VALUE 'a' TO 'x';
SELECT length(dolt_commit('-Am', 'drop rename')::text) AS committed;
SELECT dolt_checkout('drop_writer');
ALTER TABLE eh.dropped DROP COLUMN v;
SELECT length(dolt_commit('-Am', 'drop column')::text) AS committed;
SELECT dolt_checkout('main');
SELECT test_fold_root() AS before_drop \gset
SELECT dolt_merge('drop_writer');
SELECT test_fold_root() = :'before_drop' AS unchanged;
SELECT * FROM eh.dropped;
SELECT dolt_branch('-D', 'drop_writer');

DROP SCHEMA eh CASCADE;
