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

-- A child's own column changes with its link in place; a move that does
-- change what they share (a parent's column's type, a CHECK the parent
-- gains) links it again, its inherited columns and constraints inherited
-- only, as they were.
CREATE VIEW locality AS
SELECT 'column ' || attname AS what, attislocal AS local, attinhcount AS inherited
  FROM pg_attribute WHERE attrelid = to_regclass('inh.c') AND attnum > 0 AND NOT attisdropped
UNION ALL
SELECT 'constraint ' || conname, conislocal, coninhcount
  FROM pg_constraint WHERE conrelid = to_regclass('inh.c') AND contype = 'c'
ORDER BY 1;
SELECT dolt_reset('--hard', :'linked');
ALTER TABLE inh.c ADD COLUMN x int;
SELECT (dolt_commit('-Am', 'own column'))[1] AS own \gset
SELECT dolt_reset('--hard', :'linked');
SELECT * FROM dolt_status();
SELECT * FROM locality;
SELECT dolt_reset('--hard', :'own');
SELECT * FROM locality;
SELECT dolt_reset('--hard', :'linked');
ALTER TABLE inh.p ALTER COLUMN a TYPE bigint;
ALTER TABLE inh.p ADD CONSTRAINT p_b CHECK (b > 0);
SELECT (dolt_commit('-Am', 'parent changed'))[1] AS parent_changed \gset
SELECT dolt_reset('--hard', :'linked');
SELECT test_fold_root() = prolly_root_hash(:'linked') AS same_root;
SELECT * FROM locality;
SELECT dolt_reset('--hard', :'parent_changed');
SELECT test_fold_root() = prolly_root_hash(:'parent_changed') AS same_root;
SELECT * FROM dolt_status();
SELECT * FROM locality;
ALTER TABLE inh.p DROP COLUMN b;			-- reaches the child, and its check
SELECT * FROM locality;

-- A child the move makes again (its own columns in another order) is
-- linked again; one the move makes is linked, none of what it inherits its
-- own, as CREATE TABLE ... () INHERITS makes it.
SELECT dolt_reset('--hard', :'own');
ALTER TABLE inh.c DROP COLUMN x;
ALTER TABLE inh.c ADD COLUMN y int;
ALTER TABLE inh.c ADD COLUMN x int;
INSERT INTO inh.c (a, b, y, x) VALUES (1, 2, 3, 4);
SELECT (dolt_commit('-Am', 'reordered'))[1] AS reordered \gset
SELECT dolt_reset('--hard', :'own');
SELECT dolt_reset('--hard', :'reordered');
SELECT inhrelid::regclass, inhparent::regclass FROM pg_inherits WHERE inhrelid = to_regclass('inh.c');
SELECT test_fold_root() = prolly_root_hash(:'reordered') AS same_root;
SELECT * FROM dolt_status();
SELECT * FROM inh.p;
SELECT * FROM locality;
SELECT dolt_reset('--hard', :'parent_changed');
DROP TABLE inh.c;
SELECT (dolt_commit('-Am', 'no child'))[1] AS no_child \gset
SELECT dolt_reset('--hard', :'parent_changed');
SELECT test_fold_root() = prolly_root_hash(:'parent_changed') AS same_root;
SELECT * FROM locality;
ALTER TABLE inh.p DROP COLUMN b;			-- reaches the child, and its check
SELECT * FROM locality;

-- Which inherited columns and constraints are a child's own too is
-- described (its "local" section), and a move makes them so: a child that
-- declares an inherited column and check itself keeps them through a
-- parent's DROP COLUMN, as a table made fresh does; a table made again and
-- linked has what it only inherits inherited only.
SELECT dolt_reset('--hard', :'linked');
CREATE TABLE inh.q2 (a int, b int, CONSTRAINT qa CHECK (a > 0));
CREATE TABLE inh.p3 (a int, b int);
CREATE TABLE inh.c3 (y int);
SELECT (dolt_commit('-Am', 'parents'))[1] AS parents \gset
CREATE TABLE inh.r (a int, z int, CONSTRAINT qa CHECK (a > 0)) INHERITS (inh.q2);
INSERT INTO inh.r VALUES (1, 2, 3);
DROP TABLE inh.c3;
CREATE TABLE inh.c3 (y int) INHERITS (inh.p3);
SELECT (dolt_commit('-Am', 'children'))[1] AS children \gset
CREATE VIEW localities AS
SELECT c.relname::text AS rel, 'column ' || attname AS what, attislocal AS local, attinhcount AS inherited
  FROM pg_attribute JOIN pg_class c ON c.oid = attrelid
  WHERE c.relnamespace = to_regnamespace(current_setting('test.nsp')) AND c.relname IN ('r', 'c3')
    AND attnum > 0 AND NOT attisdropped
UNION ALL
SELECT c.relname::text, 'constraint ' || conname, conislocal, coninhcount
  FROM pg_constraint JOIN pg_class c ON c.oid = conrelid
  WHERE c.relnamespace = to_regnamespace(current_setting('test.nsp')) AND c.relname IN ('r', 'c3')
    AND contype = 'c'
ORDER BY 1, 2;
SET test.nsp = 'inh';
SELECT * INTO TEMP made FROM localities;
SELECT dolt_reset('--hard', :'parents');
SELECT dolt_reset('--hard', :'children');
SELECT test_fold_root() = prolly_root_hash(:'children') AS same_root;
SELECT * FROM dolt_status();
SELECT * FROM localities;
-- (as tables made fresh have them)
CREATE SCHEMA fresh;
CREATE TABLE fresh.q2 (a int, b int, CONSTRAINT qa CHECK (a > 0));
CREATE TABLE fresh.r (a int, z int, CONSTRAINT qa CHECK (a > 0)) INHERITS (fresh.q2);
CREATE TABLE fresh.p3 (a int, b int);
CREATE TABLE fresh.c3 (y int) INHERITS (fresh.p3);
SET test.nsp = 'fresh';
(SELECT * FROM localities EXCEPT SELECT * FROM made)
UNION ALL (SELECT * FROM made EXCEPT SELECT * FROM localities);
DROP SCHEMA fresh CASCADE;
SET test.nsp = 'inh';
ALTER TABLE inh.q2 DROP COLUMN a;			-- r keeps its own a
ALTER TABLE inh.p3 DROP COLUMN b;			-- c3 doesn't keep b
SELECT * FROM inh.r;
SELECT * FROM localities;
SELECT dolt_reset('--hard');
-- (locality alone is a change)
ALTER TABLE inh.c3 NO INHERIT inh.p3;
ALTER TABLE inh.c3 INHERIT inh.p3;
SELECT * FROM dolt_status();
SELECT dolt_reset('--hard');
DROP VIEW localities;
RESET test.nsp;
SELECT dolt_reset('--hard', :'linked');
DROP VIEW locality;

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

-- A typed table's composite type can change across a move.  Detach it
-- before replacing the type, and attach it after its columns match.
CREATE SCHEMA typed VERSIONED;
CREATE TYPE typed.ty AS (a int, b text);
CREATE TABLE typed.tt OF typed.ty (PRIMARY KEY (a));
INSERT INTO typed.tt VALUES (1, 'one');
SELECT (dolt_commit('-Am', 'typed base'))[1] AS typed_base \gset
SELECT dolt_branch('typed_base');
ALTER TYPE typed.ty ADD ATTRIBUTE c int CASCADE;
UPDATE typed.tt SET c = 10;
SELECT (dolt_commit('-Am', 'typed added'))[1] AS typed_added \gset
SELECT dolt_reset('--hard', :'typed_base');
SELECT test_fold_root() = prolly_root_hash(:'typed_base') AS same_root;
SELECT * FROM typed.tt;
SELECT dolt_reset('--hard', :'typed_added');
SELECT test_fold_root() = prolly_root_hash(:'typed_added') AS same_root;
SELECT * FROM typed.tt;
SELECT reloftype = 'typed.ty'::regtype AS still_typed
FROM pg_class WHERE oid = 'typed.tt'::regclass;
SELECT * FROM dolt_status();

-- A merge across the added attribute keeps an independent row edit.
SELECT dolt_branch('typed_rows', :'typed_base');
SELECT dolt_checkout('typed_rows');
UPDATE typed.tt SET b = 'edited';
SELECT dolt_commit('-Am', 'typed row edit') IS NOT NULL AS committed;
SELECT (dolt_merge('main'))[2:4];
SELECT * FROM typed.tt ORDER BY a;
SELECT reloftype = 'typed.ty'::regtype AS still_typed
FROM pg_class WHERE oid = 'typed.tt'::regclass;
SELECT test_fold_root() = prolly_root_hash('HEAD') AS same_root;
SELECT * FROM dolt_status();
SELECT dolt_checkout('main');
SELECT dolt_branch('-D', 'typed_rows');

-- Retyping and reordering attributes, including a table made again.
ALTER TYPE typed.ty ALTER ATTRIBUTE a TYPE bigint CASCADE;
ALTER TYPE typed.ty DROP ATTRIBUTE b CASCADE;
ALTER TYPE typed.ty ADD ATTRIBUTE b text COLLATE "C" CASCADE;
UPDATE typed.tt SET b = 'reordered';
SELECT (dolt_commit('-Am', 'typed reordered'))[1] AS typed_reordered \gset
SELECT dolt_checkout('typed_base');
SELECT test_fold_root() = prolly_root_hash(:'typed_base') AS same_root;
SELECT * FROM typed.tt;
SELECT dolt_checkout('main');
SELECT test_fold_root() = prolly_root_hash(:'typed_reordered') AS same_root;
SELECT * FROM typed.tt;
SELECT reloftype = 'typed.ty'::regtype AS still_typed
FROM pg_class WHERE oid = 'typed.tt'::regclass;
SELECT * FROM dolt_status();

SELECT dolt_branch('-D', 'typed_base');

-- An outside table still can't depend on a versioned type: refused as it
-- is made, so no move meets one.
CREATE SCHEMA typed_external;
CREATE TABLE typed_external.tt OF typed.ty;
SELECT test_fold_root() = prolly_root_hash(:'typed_reordered') AS same_root;
SELECT to_regclass('typed_external.tt') IS NULL AS refused;
SELECT * FROM dolt_status();
DROP SCHEMA typed_external;

-- An indirectly replaced composite type: its definition and its typed
-- table's definition are unchanged when the attribute's domain changes.
CREATE DOMAIN typed.pos AS int CHECK (VALUE > 0);
CREATE TYPE typed.indirect AS (a typed.pos);
CREATE TABLE typed.dt OF typed.indirect;
INSERT INTO typed.dt VALUES (20);
SELECT (dolt_commit('-Am', 'typed domain base'))[1] AS typed_domain_base \gset
ALTER DOMAIN typed.pos DROP CONSTRAINT pos_check;
ALTER DOMAIN typed.pos ADD CONSTRAINT pos_check CHECK (VALUE > 10);
SELECT (dolt_commit('-Am', 'typed domain changed'))[1] AS typed_domain_changed \gset
SELECT dolt_reset('--hard', :'typed_domain_base');
SELECT test_fold_root() = prolly_root_hash(:'typed_domain_base') AS same_root;
SELECT dolt_reset('--hard', :'typed_domain_changed');
SELECT test_fold_root() = prolly_root_hash(:'typed_domain_changed') AS same_root;
SELECT * FROM typed.dt;
SELECT reloftype = 'typed.indirect'::regtype AS still_typed
FROM pg_class WHERE oid = 'typed.dt'::regclass;
INSERT INTO typed.dt VALUES (5);
SELECT * FROM dolt_status();

-- The target may leave a table untyped, attach it to a different type,
-- or drop it while replacing the old type.
CREATE TYPE typed.other AS (a typed.pos);
CREATE TABLE typed.untyped OF typed.indirect;
CREATE TABLE typed.switched OF typed.indirect;
CREATE TABLE typed.gone OF typed.indirect;
SELECT (dolt_commit('-Am', 'typed links base'))[1] AS typed_links_base \gset
ALTER TABLE typed.untyped NOT OF;
ALTER TABLE typed.switched OF typed.other;
DROP TABLE typed.gone;
ALTER TYPE typed.indirect ADD ATTRIBUTE b text CASCADE;
SELECT (dolt_commit('-Am', 'typed links changed'))[1] AS typed_links_changed \gset
SELECT dolt_reset('--hard', :'typed_links_base');
SELECT test_fold_root() = prolly_root_hash(:'typed_links_base') AS same_root;
SELECT dolt_reset('--hard', :'typed_links_changed');
SELECT test_fold_root() = prolly_root_hash(:'typed_links_changed') AS same_root;
SELECT relname, reloftype::regtype FROM pg_class
WHERE relnamespace = 'typed'::regnamespace AND relkind = 'r' ORDER BY relname;
SELECT to_regclass('typed.gone') IS NULL AS gone;
SELECT * FROM dolt_status();

-- A partitioned typed table detaches too, while its partitions follow
-- the parent's column changes.
CREATE TYPE typed.pty AS (a int);
CREATE TABLE typed.pt OF typed.pty PARTITION BY RANGE (a);
CREATE TABLE typed.pt1 PARTITION OF typed.pt FOR VALUES FROM (0) TO (10);
INSERT INTO typed.pt VALUES (1);
SELECT (dolt_commit('-Am', 'typed partition base'))[1] AS typed_partition_base \gset
ALTER TYPE typed.pty ADD ATTRIBUTE b text CASCADE;
UPDATE typed.pt SET b = 'partition';
SELECT (dolt_commit('-Am', 'typed partition added'))[1] AS typed_partition_added \gset
SELECT dolt_reset('--hard', :'typed_partition_base');
SELECT test_fold_root() = prolly_root_hash(:'typed_partition_base') AS same_root;
SELECT dolt_reset('--hard', :'typed_partition_added');
SELECT test_fold_root() = prolly_root_hash(:'typed_partition_added') AS same_root;
SELECT * FROM typed.pt;
SELECT reloftype = 'typed.pty'::regtype AS still_typed
FROM pg_class WHERE oid = 'typed.pt'::regclass;
SELECT * FROM dolt_status();

-- A partitioned typed table whose type is replaced through a domain: the
-- parent's column is converted, its partition's follows it.  A domain in
-- the partition key can't be converted: refused.
CREATE DOMAIN typed.ppos AS int CHECK (VALUE > 0);
CREATE TYPE typed.pdy AS (k int, a typed.ppos);
CREATE TABLE typed.pd OF typed.pdy PARTITION BY RANGE (k);
CREATE TABLE typed.pd1 PARTITION OF typed.pd FOR VALUES FROM (0) TO (100);
INSERT INTO typed.pd VALUES (1, 50);
SELECT (dolt_commit('-Am', 'typed partition domain base'))[1] AS typed_pd_base \gset
ALTER DOMAIN typed.ppos DROP CONSTRAINT ppos_check;
ALTER DOMAIN typed.ppos ADD CONSTRAINT ppos_check CHECK (VALUE > 20);
SELECT (dolt_commit('-Am', 'typed partition domain changed'))[1] AS typed_pd_changed \gset
SELECT dolt_reset('--hard', :'typed_pd_base');
SELECT test_fold_root() = prolly_root_hash(:'typed_pd_base') AS same_root;
INSERT INTO typed.pd VALUES (2, 15);
SELECT dolt_reset('--hard', :'typed_pd_changed');
SELECT test_fold_root() = prolly_root_hash(:'typed_pd_changed') AS same_root;
SELECT * FROM typed.pd ORDER BY k;
SELECT reloftype = 'typed.pdy'::regtype AS still_typed
FROM pg_class WHERE oid = 'typed.pd'::regclass;
INSERT INTO typed.pd VALUES (3, 15);
SELECT * FROM dolt_status();
CREATE DOMAIN typed.pkey AS int CHECK (VALUE > 0);
CREATE TYPE typed.pky AS (k typed.pkey);
CREATE TABLE typed.pk OF typed.pky PARTITION BY RANGE (k);
CREATE TABLE typed.pk1 PARTITION OF typed.pk FOR VALUES FROM (1) TO (100);
SELECT (dolt_commit('-Am', 'typed partition key base'))[1] AS typed_pk_base \gset
ALTER DOMAIN typed.pkey DROP CONSTRAINT pkey_check;
ALTER DOMAIN typed.pkey ADD CONSTRAINT pkey_check CHECK (VALUE > 20);
SELECT (dolt_commit('-Am', 'typed partition key changed'))[1] AS typed_pk_changed \gset
SELECT dolt_reset('--hard', :'typed_pk_base');
SELECT test_fold_root() = prolly_root_hash(:'typed_pk_changed') AS unmoved;
SELECT * FROM dolt_status();

DROP VIEW described;
SELECT dolt_reset('--hard', :'base');
DROP SCHEMA app CASCADE;
