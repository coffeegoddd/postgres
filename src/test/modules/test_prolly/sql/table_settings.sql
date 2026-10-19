-- Explicit table tuning and replication settings are versioned, including
-- resets to defaults.  Catalog order and physical placement aren't identity.
-- Keep the remote history independent of other regression tests.
SELECT current_database() AS settings_home \gset
CREATE DATABASE regress_table_settings;
\c regress_table_settings
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;
CREATE SCHEMA settings VERSIONED;
CREATE TABLE settings.opts (id int PRIMARY KEY, value text);
CREATE TABLE settings.repl (id int PRIMARY KEY, value text);
CREATE TABLE settings.storage (value text);
CREATE TABLE settings.compression (value text);
CREATE TABLE settings.stats (value text);
INSERT INTO settings.opts VALUES (1, 'one');
SELECT (dolt_commit('-Am', 'defaults'))[1] AS defaults \gset

ALTER TABLE settings.opts SET (fillfactor = 70, autovacuum_enabled = false,
  toast.autovacuum_enabled = false, parallel_workers = 2, vacuum_truncate = false);
ALTER TABLE settings.repl REPLICA IDENTITY FULL;
ALTER TABLE settings.storage ALTER COLUMN value SET STORAGE EXTERNAL;
ALTER TABLE settings.compression ALTER COLUMN value SET COMPRESSION pglz;
ALTER TABLE settings.stats ALTER COLUMN value SET STATISTICS 1000;
SELECT * FROM dolt_status() ORDER BY table_name;
SELECT (dolt_commit('-Am', 'settings'))[1] AS changed \gset
SELECT test_fold_root() = prolly_root_hash(:'defaults') AS same_root;
SELECT key, substring(value from position(E'columns\n' in value)) AS definition
  FROM test_fold_items() WHERE key LIKE 't:settings.%' ORDER BY key;

-- Option ordering is immaterial, including separately stored TOAST options.
ALTER TABLE settings.opts RESET (fillfactor, autovacuum_enabled,
  toast.autovacuum_enabled, parallel_workers, vacuum_truncate);
ALTER TABLE settings.opts SET (vacuum_truncate = false, parallel_workers = 2,
  toast.autovacuum_enabled = false, autovacuum_enabled = false, fillfactor = 70);
SELECT test_fold_root() = prolly_root_hash(:'changed') AS same_root;
SELECT * FROM dolt_status();
SELECT dolt_reset('--hard', :'defaults');
SELECT test_fold_root() = prolly_root_hash(:'defaults') AS same_root;
SELECT * FROM dolt_status();
SELECT dolt_reset('--hard', :'changed');
SELECT test_fold_root() = prolly_root_hash(:'changed') AS same_root;
SELECT * FROM dolt_status();
SELECT * FROM settings.opts;

-- Creating the table again restores settings too.
DROP SCHEMA settings CASCADE;
SELECT dolt_commit('-Am', 'removed') IS NOT NULL AS committed;
SELECT dolt_reset('--hard', :'changed');
SELECT test_fold_root() = prolly_root_hash(:'changed') AS same_root;
SELECT * FROM dolt_status();

-- An explicitly set default remains explicit; RESET goes back to inheriting
-- the server's policy.  A type change resets compression/storage in Postgres.
ALTER TABLE settings.opts SET (fillfactor = 100);
ALTER TABLE settings.compression ALTER COLUMN value TYPE varchar(100);
ALTER TABLE settings.compression ALTER COLUMN value SET COMPRESSION pglz;
ALTER TABLE settings.storage ALTER COLUMN value TYPE varchar(100);
ALTER TABLE settings.storage ALTER COLUMN value SET STORAGE EXTERNAL;
SELECT (dolt_commit('-Am', 'retyped'))[1] AS retyped \gset
SELECT dolt_reset('--hard', :'changed');
SELECT test_fold_root() = prolly_root_hash(:'changed') AS same_root;
SELECT dolt_reset('--hard', :'retyped');
SELECT test_fold_root() = prolly_root_hash(:'retyped') AS same_root;

-- An identity index is restored after indexes and NOT NULL constraints.
-- Quoted names exercise field escaping; rebuilding the same named index
-- must restore its identity marking even though the identity didn't change.
CREATE UNIQUE INDEX "identity\index" ON settings.repl (id);
ALTER TABLE settings.repl REPLICA IDENTITY USING INDEX "identity\index";
SELECT (dolt_commit('-Am', 'index identity'))[1] AS indexed \gset
DROP TABLE settings.repl;
SELECT dolt_commit('-Am', 'removed identity table') IS NOT NULL AS committed;
SELECT dolt_reset('--hard', :'indexed');
SELECT test_fold_root() = prolly_root_hash(:'indexed') AS same_root;
DROP INDEX settings."identity\index";
CREATE UNIQUE INDEX "identity\index" ON settings.repl (id) WITH (fillfactor = 80);
ALTER TABLE settings.repl REPLICA IDENTITY USING INDEX "identity\index";
SELECT (dolt_commit('-Am', 'rebuilt index'))[1] AS rebuilt \gset
SELECT dolt_reset('--hard', :'indexed');
SELECT test_fold_root() = prolly_root_hash(:'indexed') AS same_root;
SELECT indexrelid::regclass FROM pg_index WHERE indrelid = 'settings.repl'::regclass AND indisreplident;
SELECT dolt_reset('--hard', :'rebuilt');
SELECT test_fold_root() = prolly_root_hash(:'rebuilt') AS same_root;
ALTER TABLE settings.repl REPLICA IDENTITY NOTHING;
SELECT (dolt_commit('-Am', 'nothing identity'))[1] AS nothing \gset
SELECT dolt_reset('--hard', :'indexed');
SELECT dolt_reset('--hard', :'nothing');
SELECT test_fold_root() = prolly_root_hash(:'nothing') AS same_root;
SELECT dolt_reset('--hard', :'defaults');
SELECT test_fold_root() = prolly_root_hash(:'defaults') AS same_root;
SELECT dolt_reset('--hard', :'indexed');
DROP INDEX settings."identity\index";
-- With its index gone, INDEX behaves as NOTHING and is described as such.
SELECT (dolt_commit('-Am', 'dropped identity index'))[1] AS dropped_index \gset
SELECT dolt_reset('--hard', :'indexed');
SELECT dolt_reset('--hard', :'dropped_index');
SELECT test_fold_root() = prolly_root_hash(:'dropped_index') AS same_root;

-- A parent's settings must not overwrite a child's explicit defaults.
CREATE TABLE settings.parent (id int, value text) PARTITION BY RANGE (id);
ALTER TABLE settings.parent ALTER COLUMN value SET STORAGE EXTERNAL;
ALTER TABLE settings.parent ALTER COLUMN value SET COMPRESSION pglz;
ALTER TABLE settings.parent ALTER COLUMN value SET STATISTICS 100;
CREATE TABLE settings.child PARTITION OF settings.parent FOR VALUES FROM (0) TO (10);
ALTER TABLE settings.child ALTER COLUMN value SET STORAGE DEFAULT;
ALTER TABLE settings.child ALTER COLUMN value SET COMPRESSION default;
ALTER TABLE settings.child ALTER COLUMN value SET STATISTICS -1;
ALTER TABLE settings.child SET (fillfactor = 80, toast.autovacuum_enabled = false);
INSERT INTO settings.parent VALUES (1, 'child');
CREATE TABLE settings.inhparent (value text);
CREATE TABLE settings.inhchild () INHERITS (settings.inhparent);
ALTER TABLE ONLY settings.inhparent ALTER COLUMN value SET STORAGE EXTERNAL;
ALTER TABLE ONLY settings.inhchild ALTER COLUMN value SET COMPRESSION pglz;
SELECT (dolt_commit('-Am', 'parents'))[1] AS parents \gset
SELECT dolt_reset('--hard', :'dropped_index');
SELECT dolt_reset('--hard', :'parents');
SELECT test_fold_root() = prolly_root_hash(:'parents') AS same_root;
SELECT * FROM dolt_status();
SELECT * FROM settings.parent;
ALTER TABLE ONLY settings.parent ALTER COLUMN value SET STORAGE MAIN;
ALTER TABLE ONLY settings.parent ALTER COLUMN value SET COMPRESSION default;
ALTER TABLE ONLY settings.parent ALTER COLUMN value SET STATISTICS 0;
SELECT (dolt_commit('-Am', 'parent settings'))[1] AS parent_changed \gset
SELECT dolt_reset('--hard', :'parents');
SELECT test_fold_root() = prolly_root_hash(:'parents') AS same_root;
SELECT dolt_reset('--hard', :'parent_changed');
SELECT test_fold_root() = prolly_root_hash(:'parent_changed') AS same_root;

-- Rebuilding a named type resets its columns and can rebuild an identity
-- index, even when no table definition changed.
CREATE DOMAIN settings.word AS text;
CREATE TABLE settings.words (value settings.word NOT NULL);
CREATE UNIQUE INDEX words_identity ON settings.words (value);
ALTER TABLE settings.words ALTER COLUMN value SET STORAGE MAIN;
ALTER TABLE settings.words ALTER COLUMN value SET COMPRESSION pglz;
ALTER TABLE settings.words ALTER COLUMN value SET STATISTICS 42;
ALTER TABLE settings.words REPLICA IDENTITY USING INDEX words_identity;
INSERT INTO settings.words VALUES ('word');
SELECT (dolt_commit('-Am', 'word type'))[1] AS word_base \gset
ALTER DOMAIN settings.word ADD CONSTRAINT nonempty CHECK (length(VALUE) > 0);
SELECT (dolt_commit('-Am', 'word check'))[1] AS word_check \gset
SELECT dolt_reset('--hard', :'word_base');
SELECT test_fold_root() = prolly_root_hash(:'word_base') AS same_root;
SELECT * FROM dolt_status();
SELECT dolt_reset('--hard', :'word_check');
SELECT test_fold_root() = prolly_root_hash(:'word_check') AS same_root;
SELECT * FROM dolt_status();

-- Settings merge by option name; column settings merge with that column.
SELECT dolt_branch('settings_side');
ALTER TABLE settings.opts SET (fillfactor = 60);
ALTER TABLE settings.storage ALTER COLUMN value SET STORAGE MAIN;
SELECT dolt_commit('-Am', 'ours') IS NOT NULL AS committed;
SELECT dolt_checkout('settings_side');
ALTER TABLE settings.opts SET (parallel_workers = 4);
ALTER TABLE settings.repl REPLICA IDENTITY FULL;
SELECT dolt_commit('-Am', 'theirs') IS NOT NULL AS committed;
SELECT dolt_checkout('main');
SELECT (dolt_merge('settings_side'))[2:4];
SELECT test_fold_root() = prolly_root_hash('main') AS same_root;
SELECT * FROM dolt_status();
SELECT relname, relreplident, (SELECT string_agg(o, ', ' ORDER BY o) FROM unnest(reloptions) o) AS options
  FROM pg_class WHERE oid IN ('settings.opts'::regclass, 'settings.repl'::regclass) ORDER BY relname;
SELECT dolt_branch('settings_conflict');
ALTER TABLE settings.opts SET (fillfactor = 50);
SELECT dolt_commit('-Am', 'ours conflict') IS NOT NULL AS committed;
SELECT dolt_checkout('settings_conflict');
ALTER TABLE settings.opts SET (fillfactor = 40);
SELECT dolt_commit('-Am', 'theirs conflict') IS NOT NULL AS committed;
SELECT dolt_checkout('main');
SELECT dolt_merge('settings_conflict');
SELECT test_fold_root() = prolly_root_hash('main') AS same_root;
SELECT * FROM dolt_status();

-- Competing replica identity modes are one setting, hence a conflict.
SELECT dolt_branch('identity_conflict');
ALTER TABLE settings.repl REPLICA IDENTITY DEFAULT;
SELECT dolt_commit('-Am', 'default identity') IS NOT NULL AS committed;
SELECT dolt_checkout('identity_conflict');
ALTER TABLE settings.repl REPLICA IDENTITY NOTHING;
SELECT dolt_commit('-Am', 'nothing identity') IS NOT NULL AS committed;
SELECT dolt_checkout('main');
SELECT dolt_merge('identity_conflict');
SELECT test_fold_root() = prolly_root_hash('main') AS same_root;
SELECT * FROM dolt_status();
SELECT dolt_branch('-D', 'identity_conflict');

-- A clone creates the definitions from scratch in another database.
SELECT current_database() AS db \gset
\getenv abs_builddir PG_ABS_BUILDDIR
SELECT 'file://' || :'abs_builddir' || '/tmp_check/remotes/settings-' || txid_current() AS url \gset
SELECT dolt_remote('add', 'settings_remote', :'url');
SELECT dolt_push('settings_remote', 'main');
CREATE DATABASE regress_settings_clone;
\c regress_settings_clone
CREATE EXTENSION test_prolly;
SELECT dolt_clone(:'url');
SELECT test_fold_root() = prolly_root_hash('main') AS same_root;
SELECT * FROM dolt_status();
SELECT * FROM settings.opts;
SELECT * FROM settings.parent;
\c :db
DROP DATABASE regress_settings_clone;
SELECT dolt_remote('remove', 'settings_remote');

-- Format 9 omitted these properties; it cannot be moved to.
SELECT test_commit_of(test_root_with_format(prolly_root_hash('HEAD'), '9'), 'format 9') AS old9 \gset
SELECT dolt_reset('--hard', :'old9');
SELECT dolt_branch('-D', 'settings_side');
SELECT dolt_branch('-D', 'settings_conflict');
DROP SCHEMA settings CASCADE;

\c :settings_home
DROP DATABASE regress_table_settings;
