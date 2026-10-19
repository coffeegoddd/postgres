-- What changes how a definition behaves is part of its description, so
-- part of the hash: a table's inheritance and type (typed tables), when
-- its triggers and rules fire, a domain's default and collation, a
-- composite type's attributes' collations, a view's options and a
-- materialized view's indexes and whether it is populated.  Each is a
-- change (dolt_status() has it), and a move to a commit makes it (the
-- branch folds to the commit's root again).

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;
CREATE SCHEMA app VERSIONED;
CREATE TABLE app.p (a int CHECK (a > 0), note text NOT NULL DEFAULT '');
CREATE TABLE app.c (a int CONSTRAINT p_a_check CHECK (a > 0), note text NOT NULL DEFAULT '',
                    extra int);
CREATE TYPE app.ty AS (a int, b text);
CREATE TABLE app.tt (a int, b text);
CREATE DOMAIN app.d AS int;
CREATE DOMAIN app.dc AS text;
CREATE TYPE app.co AS (a text);
CREATE VIEW app.v AS SELECT a FROM app.p;
CREATE VIEW app.w AS SELECT a FROM app.p WHERE a < 10;
CREATE MATERIALIZED VIEW app.m AS SELECT a FROM app.p;
CREATE MATERIALIZED VIEW app.m2 AS SELECT a FROM app.p WHERE false WITH NO DATA;
CREATE FUNCTION app.mark() RETURNS trigger LANGUAGE plpgsql AS
$$BEGIN NEW.extra := -1; RETURN NEW; END$$;
CREATE TRIGGER tr BEFORE INSERT ON app.c FOR EACH ROW EXECUTE FUNCTION app.mark();
CREATE TRIGGER tr2 BEFORE UPDATE ON app.c FOR EACH ROW EXECUTE FUNCTION app.mark();
CREATE RULE r AS ON DELETE TO app.tt DO INSTEAD NOTHING;
INSERT INTO app.p VALUES (1, 'p');
INSERT INTO app.c VALUES (2, 'c', 0);
INSERT INTO app.tt VALUES (1, 'one');
SELECT (dolt_commit('-Am', 'base'))[1] AS base \gset

-- Each one changed.
ALTER TABLE app.c INHERIT app.p;
ALTER TABLE app.tt OF app.ty;
ALTER DOMAIN app.d SET DEFAULT 5;
DROP DOMAIN app.dc;
CREATE DOMAIN app.dc AS text COLLATE "C";
ALTER TYPE app.co ALTER ATTRIBUTE a TYPE text COLLATE "C";
ALTER VIEW app.v SET (security_invoker = on);
ALTER VIEW app.w SET (check_option = local);
CREATE UNIQUE INDEX m_a ON app.m (a);
REFRESH MATERIALIZED VIEW app.m2;		-- populated, still empty
ALTER TABLE app.c DISABLE TRIGGER tr;
ALTER TABLE app.c ENABLE REPLICA TRIGGER tr2;
ALTER TABLE app.tt DISABLE RULE r;
SELECT * FROM dolt_status() ORDER BY table_name;
SELECT (dolt_commit('-Am', 'changed'))[1] AS changed \gset

-- How each one is, as the branch has it.
CREATE VIEW described AS
SELECT 'inherits' AS what, string_agg(inhrelid::regclass || ' ' || inhparent::regclass, ', '
                                      ORDER BY inhrelid::regclass::text, inhseqno) AS how
  FROM pg_inherits
UNION ALL SELECT 'typed', string_agg(oid::regclass || ' ' || reloftype::regtype, ', ')
  FROM pg_class WHERE relnamespace = to_regnamespace('app') AND reloftype <> 0
UNION ALL SELECT 'firing', string_agg(tgname || ' ' || tgenabled::text, ', ' ORDER BY tgname)
  FROM pg_trigger WHERE tgrelid = to_regclass('app.c') AND NOT tgisinternal
UNION ALL SELECT 'rule', string_agg(rulename || ' ' || ev_enabled::text, ', ')
  FROM pg_rewrite WHERE ev_class = to_regclass('app.tt')
UNION ALL SELECT 'domains', string_agg(typname || ' ' || coalesce(typdefault, '-') || ' ' ||
                                       coalesce(typcollation::regcollation::text, '-'), ', ' ORDER BY typname)
  FROM pg_type WHERE typnamespace = to_regnamespace('app') AND typtype = 'd'
UNION ALL SELECT 'composite', string_agg(attname || ' ' || attcollation::regcollation, ', ')
  FROM pg_attribute WHERE attrelid = to_regclass('app.co')
UNION ALL SELECT 'views', string_agg(relname || ' ' || coalesce(reloptions::text, '-'), ', ' ORDER BY relname)
  FROM pg_class WHERE relnamespace = to_regnamespace('app') AND relkind = 'v'
UNION ALL SELECT 'matviews', string_agg(relname || ' ' || relispopulated || ' ' ||
                                        coalesce((SELECT string_agg(indexrelid::regclass::text, ' ')
                                                  FROM pg_index WHERE indrelid = c.oid), '-'), ', '
                                        ORDER BY relname)
  FROM pg_class c WHERE relnamespace = to_regnamespace('app') AND relkind = 'm';

-- Back to the commit before: each one is as it was.
SELECT dolt_reset('--hard', :'base');
SELECT test_fold_root() = prolly_root_hash(:'base') AS same_root;
SELECT * FROM dolt_status();
SELECT * FROM described;
SELECT count(*) FROM app.p;					-- not the child's rows

-- And to the commit after: each one is made again.
SELECT dolt_reset('--hard', :'changed');
SELECT test_fold_root() = prolly_root_hash(:'changed') AS same_root;
SELECT * FROM dolt_status();
SELECT * FROM described;
SELECT * FROM app.p ORDER BY a;				-- the child's rows too
INSERT INTO app.c VALUES (3, 'c3', 0);		-- the disabled trigger doesn't fire
SELECT extra FROM app.c WHERE a = 3;
DELETE FROM app.tt;							-- nor the disabled rule
SELECT count(*) FROM app.tt;
INSERT INTO app.w VALUES (20);				-- the view's check option
SELECT dolt_reset('--hard');

-- The tables inherited from change (columns added to a parent reach its
-- children), and a type: the links go before, and come back after.
CREATE TYPE app.mood AS ENUM ('a', 'b');
ALTER TABLE app.c ADD COLUMN mood app.mood;
SELECT (dolt_commit('-Am', 'mood'))[1] AS mood \gset
ALTER TABLE app.p ADD COLUMN added int DEFAULT 3;
ALTER TYPE app.mood RENAME VALUE 'b' TO 'bee';
CREATE TABLE app.p2 (z int);
ALTER TABLE app.c ADD COLUMN z int;
ALTER TABLE app.c INHERIT app.p2;
INSERT INTO app.c (a, note, extra, z, mood) VALUES (5, 'five', 1, 9, 'bee');
SELECT (dolt_commit('-Am', 'grown'))[1] AS grown \gset
SELECT dolt_reset('--hard', :'mood');
SELECT test_fold_root() = prolly_root_hash(:'mood') AS same_root;
SELECT what, how FROM described WHERE what = 'inherits';
SELECT dolt_reset('--hard', :'grown');
SELECT test_fold_root() = prolly_root_hash(:'grown') AS same_root;
SELECT * FROM dolt_status();
SELECT what, how FROM described WHERE what = 'inherits';
SELECT * FROM app.p ORDER BY a;
SELECT * FROM app.p2 ORDER BY z;

-- Merged: one side's table inherits from one table fewer, the other's
-- trigger fires again.
SELECT dolt_branch('feature');
SELECT dolt_checkout('feature');
ALTER TABLE app.c NO INHERIT app.p2;
SELECT dolt_commit('-Am', 'unlinked') IS NOT NULL AS committed;
SELECT dolt_checkout('main');
ALTER TABLE app.c ENABLE TRIGGER tr;
SELECT dolt_commit('-Am', 'enabled') IS NOT NULL AS committed;
SELECT (dolt_merge('feature'))[2:4];
SELECT * FROM dolt_status();
SELECT test_fold_root() = prolly_root_hash('main') AS same_root;
SELECT * FROM described WHERE what IN ('inherits', 'firing');

-- A commit of an older format, which described none of these, isn't moved
-- to (nor merged): a fold of this format wouldn't match its descriptions.
SELECT test_commit_of(test_root_with_format(prolly_root_hash('HEAD'), '8'),
	'format 8') AS old8 \gset
SELECT dolt_reset('--hard', :'old8');
SELECT test_commit_of(test_root_with_format(prolly_root_hash('HEAD'), '7'),
	'format 7') AS old7 \gset
SELECT dolt_reset('--hard', :'old7');

-- A move that changes neither a child's nor its parent's columns or
-- constraints, nor a type they use, leaves their link alone: the child's
-- inherited columns stay inherited, and the parent's DDL still reaches
-- them.
CREATE SCHEMA inh VERSIONED;
CREATE TABLE inh.p (a int, b int);
CREATE TABLE inh.c () INHERITS (inh.p);
CREATE TYPE inh.mood AS ENUM ('x', 'y');
CREATE TABLE inh.other (m inh.mood);
SELECT (dolt_commit('-Am', 'linked'))[1] AS linked \gset
ALTER TYPE inh.mood RENAME VALUE 'y' TO 'why';
CREATE INDEX p_a ON inh.p (a);
SELECT dolt_commit('-Am', 'unrelated') IS NOT NULL AS committed;
SELECT dolt_reset('--hard', :'linked');
SELECT * FROM dolt_status();
SELECT attname, attislocal, attinhcount FROM pg_attribute
  WHERE attrelid = 'inh.c'::regclass AND attnum > 0 ORDER BY attnum;
ALTER TABLE inh.p DROP COLUMN b;
SELECT attname FROM pg_attribute
  WHERE attrelid = 'inh.c'::regclass AND attnum > 0 AND NOT attisdropped ORDER BY attnum;
SELECT dolt_reset('--hard');

-- Merged inheritance or a table's type against the other side's columns:
-- a schema conflict where the merged definitions can't be made.
CREATE TABLE inh.q (a int);
CREATE TABLE inh.d (a int);
CREATE TYPE inh.ty AS (a int, b text);
CREATE TABLE inh.tt (a int, b text);
SELECT (dolt_commit('-Am', 'unlinked'))[1] AS unlinked \gset
SELECT dolt_branch('parent_grows');
SELECT dolt_branch('table_grows');
SELECT dolt_branch('both_grow');
ALTER TABLE inh.d INHERIT inh.q;
ALTER TABLE inh.tt OF inh.ty;
SELECT (dolt_commit('-Am', 'linked'))[1] AS relinked \gset
SELECT dolt_checkout('parent_grows');
ALTER TABLE inh.q ADD COLUMN z int;
SELECT dolt_commit('-Am', 'z') IS NOT NULL AS committed;
SELECT dolt_checkout('table_grows');
ALTER TABLE inh.tt ADD COLUMN x int;
SELECT dolt_commit('-Am', 'x') IS NOT NULL AS committed;
SELECT dolt_checkout('both_grow');
ALTER TABLE inh.q ADD COLUMN z int;
ALTER TABLE inh.d ADD COLUMN z int;
SELECT dolt_commit('-Am', 'z both') IS NOT NULL AS committed;
SELECT dolt_checkout('main');
SELECT dolt_merge('parent_grows');
SELECT dolt_merge('table_grows');
SELECT * FROM dolt_status();
SELECT test_fold_root() = prolly_root_hash(:'relinked') AS same_root;
-- (a child given the parent's new column too merges)
SELECT (dolt_merge('both_grow'))[2:4];
SELECT * FROM dolt_status();
SELECT test_fold_root() = prolly_root_hash('main') AS same_root;
SELECT attname, attislocal, attinhcount FROM pg_attribute
  WHERE attrelid = 'inh.d'::regclass AND attnum > 0 ORDER BY attnum;
SELECT dolt_branch('-D', 'parent_grows');
SELECT dolt_branch('-D', 'table_grows');
SELECT dolt_branch('-D', 'both_grow');

DROP VIEW described;
SELECT dolt_reset('--hard', :'base');
DROP SCHEMA app CASCADE;
