-- Every database gets these, outside the versioned schema.  Run on branch
-- 1 (PGOPTIONS='-c catalog_branch=1'), against a server with branched
-- catalogs.
CREATE EXTENSION amcheck;
CREATE EXTENSION prolly_pages;
CREATE SEQUENCE public.vc_gen_calls;

-- A decoy: branch 2's own rt, made here and then given to branch 2, with
-- tables, a type, a function and an index under the fixture's names but
-- other definitions and rows.  Branch 1 must never see any of it: not in
-- a capture, a rebuild, a move, a merge or a check.
CREATE SCHEMA rt_decoy;
CREATE TYPE rt_decoy.mood AS ENUM ('decoy');
CREATE FUNCTION rt_decoy.scale(int) RETURNS int IMMUTABLE LANGUAGE sql AS 'SELECT -1';
CREATE TABLE rt_decoy.item (id text PRIMARY KEY, junk rt_decoy.mood, g int);
INSERT INTO rt_decoy.item SELECT 'decoy' || g, 'decoy', -g FROM generate_series(1, 500) g;
CREATE INDEX item_g ON rt_decoy.item (g);
CREATE TABLE rt_decoy.byd (id int PRIMARY KEY, a int, sq text);
INSERT INTO rt_decoy.byd SELECT g, -g, 'decoy' FROM generate_series(1, 300) g;
CREATE TABLE rt_decoy.decoy_only (z int);
INSERT INTO rt_decoy.decoy_only VALUES (1);
SET allow_system_table_mods = on;
UPDATE pg_namespace SET nspname = 'rt', nspbranch = 2 WHERE nspname = 'rt_decoy';
RESET allow_system_table_mods;
