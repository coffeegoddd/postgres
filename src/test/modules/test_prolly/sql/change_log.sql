-- The change log: each prolly table has a log of its own, and every
-- write to the table puts an entry in it, in the same transaction.  A
-- table with a primary key gets its changed keys (delta 0); one without
-- gets each version made (+1) and replaced (-1).  A fold that begins a
-- baseline consumes the entries it saw.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;

CREATE SCHEMA cl VERSIONED;
CREATE TABLE cl.keyed (id int PRIMARY KEY, v text);
CREATE TABLE cl.keyless (a int, b text);

-- entries(t): the entries of t's log this transaction sees, their deltas,
-- and whether one says to read the table in full.
CREATE FUNCTION pg_temp.entries(t regclass, OUT n int8, OUT deltas int8[],
	OUT full_read bool) AS $$
	SELECT * FROM test_log_entries(t)
$$ LANGUAGE sql;

-- The log is a TOAST-kind relation of the table's access method, part
-- of its table.
SELECT c.relkind, a.amname, d.deptype, d.refobjid = 'cl.keyed'::regclass AS of_table
FROM pg_class c
JOIN pg_am a ON a.oid = c.relam
JOIN pg_depend d ON d.classid = 'pg_class'::regclass AND d.objid = c.oid
	AND d.refclassid = 'pg_class'::regclass
WHERE c.relname = 'pg_prolly_log_' || 'cl.keyed'::regclass::oid;
SELECT * FROM pg_temp.entries('cl.keyed');

-- With a key: each key changed, once for an update that keeps the key.
INSERT INTO cl.keyed VALUES (1, 'a'), (2, 'b'), (3, 'c');
SELECT * FROM pg_temp.entries('cl.keyed');
UPDATE cl.keyed SET v = 'bb' WHERE id = 2;
SELECT * FROM pg_temp.entries('cl.keyed');
UPDATE cl.keyed SET id = 30 WHERE id = 3;
DELETE FROM cl.keyed WHERE id = 1;
SELECT * FROM pg_temp.entries('cl.keyed');

-- Without one: each version made and replaced.
INSERT INTO cl.keyless VALUES (1, 'x'), (1, 'x'), (2, 'y');
UPDATE cl.keyless SET b = 'z' WHERE a = 2;
DELETE FROM cl.keyless WHERE a = 1;
SELECT * FROM pg_temp.entries('cl.keyless');

-- A commit's fold consumes them.
SELECT length(dolt_commit('-Am', 'tables')::text) AS committed;
SELECT * FROM pg_temp.entries('cl.keyed');
SELECT * FROM pg_temp.entries('cl.keyless');

-- An entry is the write's: rolled back with it, a savepoint's too.
BEGIN;
INSERT INTO cl.keyed VALUES (4, 'd');
SAVEPOINT s;
INSERT INTO cl.keyed VALUES (5, 'e');
SELECT * FROM pg_temp.entries('cl.keyed');
ROLLBACK TO s;
SELECT * FROM pg_temp.entries('cl.keyed');
ROLLBACK;
SELECT * FROM pg_temp.entries('cl.keyed');

-- A subtransaction that writes an ancestor's entries, and one inside it
-- that ends, then the first rolled back: the entries are the ancestor's
-- again, the inner one's gone.
BEGIN;
INSERT INTO cl.keyed VALUES (4, 'd');
SAVEPOINT a;
SELECT * FROM pg_temp.entries('cl.keyed');
SAVEPOINT b;
INSERT INTO cl.keyed VALUES (5, 'e');
ROLLBACK TO b;
SELECT count(*) AS churned FROM (SELECT repeat('x', 100000) FROM generate_series(1, 50)) s;
ROLLBACK TO a;
SELECT * FROM pg_temp.entries('cl.keyed');
ROLLBACK;

-- A commit rolled back leaves the entries it consumed.
INSERT INTO cl.keyed VALUES (6, 'f');
BEGIN;
SELECT length(dolt_commit('-am', 'gone')::text) AS committed;
SELECT * FROM pg_temp.entries('cl.keyed');
ROLLBACK;
SELECT * FROM pg_temp.entries('cl.keyed');
SELECT length(dolt_commit('-am', 'six')::text) AS committed;

-- Writes after a commit in its transaction are entries for the next.
BEGIN;
INSERT INTO cl.keyed VALUES (7, 'g');
SELECT length(dolt_commit('-am', 'seven')::text) AS committed;
INSERT INTO cl.keyed VALUES (8, 'h');
COMMIT;
SELECT * FROM pg_temp.entries('cl.keyed');
SELECT length(dolt_commit('-am', 'eight')::text) AS committed;

-- A keyless row with a value out of line by content (its hash pointer):
-- held, the pointer in the entry.
INSERT INTO cl.keyless
SELECT 3, string_agg(md5(g::text), '') FROM generate_series(1, 300) g;
SELECT * FROM pg_temp.entries('cl.keyless');

-- A truncation changes rows no entry names, made anew or (storage the
-- transaction made) in place, and so does a rewrite: read in full.
BEGIN;
TRUNCATE cl.keyed;
INSERT INTO cl.keyed VALUES (9, 'i');
TRUNCATE cl.keyed;
SELECT * FROM pg_temp.entries('cl.keyed');
ROLLBACK;
BEGIN;
ALTER TABLE cl.keyed ALTER COLUMN v TYPE varchar(10);
SELECT * FROM pg_temp.entries('cl.keyed');
ROLLBACK;
SELECT length(dolt_commit('-am', 'long')::text) AS committed;

-- Leaving a branch with changes folds it as its baseline, which consumes
-- its entries; a branch's first use moves its copy of the table (and of
-- the log) to its head, and the move's fold consumes the move's.  Then
-- each branch's log is its own.
INSERT INTO cl.keyed VALUES (10, 'j');
SELECT dolt_branch('cl_other');
SELECT dolt_checkout('cl_other');
SELECT * FROM pg_temp.entries('cl.keyed');
INSERT INTO cl.keyed VALUES (11, 'k');
SELECT * FROM pg_temp.entries('cl.keyed');
SELECT dolt_checkout('main');
SELECT * FROM pg_temp.entries('cl.keyed');
INSERT INTO cl.keyed VALUES (12, 'l');
SELECT * FROM pg_temp.entries('cl.keyed');

-- A move writes rows on the session's behalf: they are entries too, and
-- the move's fold consumes them.
SELECT dolt_reset('--hard');
SELECT * FROM pg_temp.entries('cl.keyed');
SELECT count(*) AS reset_away FROM cl.keyed WHERE id = 10;

-- A key's values out of line are brought inline: the entry holds the
-- key, not a pointer to a value that may be gone when it is read.
CREATE TABLE cl.wide (a text, b text, PRIMARY KEY (a, b));
ALTER TABLE cl.wide ALTER a SET STORAGE EXTERNAL;
INSERT INTO cl.wide VALUES (repeat('a', 2500), 'b');
SELECT * FROM pg_temp.entries('cl.wide');

-- INSERT ... ON CONFLICT, and COPY's many rows at a time: an entry for
-- each.
SELECT length(dolt_commit('-Am', 'wide')::text) AS committed;
INSERT INTO cl.keyed VALUES (13, 'm'), (12, 'conflict')
	ON CONFLICT (id) DO UPDATE SET v = excluded.v;
INSERT INTO cl.keyed VALUES (13, 'n') ON CONFLICT DO NOTHING;
SELECT * FROM pg_temp.entries('cl.keyed');
COPY cl.keyless FROM STDIN;
20	t
21	u
22	v
\.
SELECT * FROM pg_temp.entries('cl.keyless');
SELECT length(dolt_commit('-am', 'conflict and copy')::text) AS committed;

-- CLUSTER and VACUUM FULL change no row: no entry, the log's entries (by
-- key or image) still hold.  A materialized view's refresh may change
-- them all: read in full.
CLUSTER cl.keyed USING keyed_pkey;
SELECT * FROM pg_temp.entries('cl.keyed');
VACUUM FULL cl.keyless;
SELECT * FROM pg_temp.entries('cl.keyless');
CREATE MATERIALIZED VIEW cl.mv AS SELECT id FROM cl.keyed;
SELECT length(dolt_commit('-Am', 'mv')::text) AS committed;
REFRESH MATERIALIZED VIEW cl.mv;
SELECT * FROM pg_temp.entries('cl.mv');

-- The log takes its table's owner, as a TOAST table does.
CREATE ROLE cl_owner;
ALTER TABLE cl.keyed OWNER TO cl_owner;
SELECT c.relowner = t.relowner AS same_owner
FROM pg_class c, pg_class t
WHERE t.oid = 'cl.keyed'::regclass
	AND c.relname = 'pg_prolly_log_' || t.oid;
ALTER TABLE cl.keyed OWNER TO CURRENT_USER;
DROP ROLE cl_owner;

-- Dropped with its table.
SELECT 'pg_prolly_log_' || 'cl.keyless'::regclass::oid AS keyless_log \gset
DROP TABLE cl.keyless;
SELECT count(*) AS gone FROM pg_class WHERE relname = :'keyless_log';

DROP SCHEMA cl CASCADE;
SELECT length(dolt_commit('-Am', 'cl gone')::text) AS committed;
SELECT dolt_branch('-D', 'cl_other');

-- A table whose log is of the first format (an entry a row, no index) is
-- taken as having none: it is written nothing, folds in full, and
-- prolly_make_change_logs() makes it a log of this format.
CREATE SCHEMA clf VERSIONED;
CREATE TABLE clf.t (id int PRIMARY KEY, v text);
INSERT INTO clf.t SELECT g, 'v' FROM generate_series(1, 10) g;
SELECT length(dolt_commit('-Am', 'clf')::text) AS committed;
SELECT test_drop_change_log('clf.t');
SELECT test_make_format1_change_log('clf.t');
UPDATE clf.t SET v = 'w' WHERE id = 1;
SELECT 'pg_toast.pg_prolly_log_' || 'clf.t'::regclass::oid AS clf_log \gset
SELECT count(*) AS format1_rows FROM :clf_log;
SELECT pg_stat_force_next_flush();
SELECT pg_stat_reset_shared('prolly');
SELECT length(dolt_commit('-am', 'clf w')::text) AS committed;
SELECT pg_stat_force_next_flush();
SELECT value AS full_without_log FROM pg_stat_prolly WHERE name = 'fold_full_tables';
SELECT * FROM prolly_make_change_logs();
SELECT attname FROM pg_attribute WHERE attrelid = :'clf_log'::regclass AND attnum > 0 ORDER BY attnum;
SELECT count(*) AS indexed FROM pg_index WHERE indrelid = :'clf_log'::regclass;
UPDATE clf.t SET v = 'x' WHERE id = 2;
SELECT * FROM pg_temp.entries('clf.t');
DROP SCHEMA clf CASCADE;
SELECT length(dolt_commit('-Am', 'clf gone')::text) AS committed;

-- An update's or a delete's entry is made from the old version the write
-- has in hand, not read again from its page: a one-row update and a
-- one-row delete touch the table's pages five times (seven, read again).
-- Autovacuum is off: an autoanalyze reading the table would count too.
CREATE SCHEMA clt VERSIONED;
CREATE TABLE clt.touch (id int PRIMARY KEY, v text)
	WITH (autovacuum_enabled = off);
INSERT INTO clt.touch SELECT g, 'v' FROM generate_series(1, 100) g;
SELECT pg_stat_force_next_flush();
SELECT heap_blks_hit + heap_blks_read AS touch0 FROM pg_statio_user_tables
WHERE relid = 'clt.touch'::regclass \gset
UPDATE clt.touch SET v = 'w' WHERE id = 50;
DELETE FROM clt.touch WHERE id = 51;
SELECT pg_stat_force_next_flush();
SELECT heap_blks_hit + heap_blks_read - :touch0 AS heap_pages_touched
FROM pg_statio_user_tables WHERE relid = 'clt.touch'::regclass;
DROP SCHEMA clt CASCADE;

-- A table without a change log (made before logs were, or restored by
-- pg_upgrade) folds in full at every commit; prolly_make_change_logs()
-- makes the logs the branch's tables lack, after which folds read them.
CREATE SCHEMA cml VERSIONED;
CREATE TABLE cml.t (id int PRIMARY KEY, v text);
CREATE TABLE cml.u (id int PRIMARY KEY);
INSERT INTO cml.t SELECT g, 'v' FROM generate_series(1, 100) g;
SELECT length(dolt_commit('-Am', 'cml')::text) AS committed;
SELECT test_drop_change_log('cml.t');
SELECT to_regclass('pg_toast.pg_prolly_log_' || 'cml.t'::regclass::oid) IS NULL AS no_log;
SELECT * FROM prolly_make_change_logs();
SELECT to_regclass('pg_toast.pg_prolly_log_' || 'cml.t'::regclass::oid) IS NOT NULL AS has_log;
SELECT count(*) FROM prolly_make_change_logs();
UPDATE cml.t SET v = 'w' WHERE id = 1;
SELECT n FROM pg_temp.entries('cml.t');
SELECT length(dolt_commit('-am', 'cml changed')::text) AS committed;
SELECT test_fold_root() = test_fold_root(true) AS same_as_full;
DROP SCHEMA cml CASCADE;
SELECT length(dolt_commit('-Am', 'cml gone')::text) AS committed;
-- VACUUM of a table vacuums its change log too, as it does its TOAST
-- table: the rows a fold consumed, dead, are gone, and the log truncated.
-- A fold reads only the rows past its baseline's position, so an empty
-- commit after a load reads none of the load's.  A log sets no autovacuum
-- options of its own: it takes its table's, and where the table leaves
-- them unset, is vacuumed soon after rows die (1000 dead, no scale; a
-- transaction's entries are a row or a few a page, so a load's are few).
-- (a database of its own: no other session's snapshot keeps the dead)
CREATE DATABASE regress_log_vacuum;
\c regress_log_vacuum
CREATE SCHEMA clv VERSIONED;
CREATE TABLE clv.vac (id int PRIMARY KEY, v text);
INSERT INTO clv.vac SELECT g, 'v' || g FROM generate_series(1, 20000) g;
SELECT length(dolt_commit('-Am', 'loaded')::text) AS committed;
SELECT c.reloptions FROM pg_class c
WHERE c.relname = 'pg_prolly_log_' || 'clv.vac'::regclass::oid;
CREATE FUNCTION pg_temp.log_vacuum_score() RETURNS float8 AS $$
	SELECT round(vacuum_score::numeric, 2)::float8
	FROM pg_stat_get_autovacuum_scores()
	WHERE oid = ('pg_toast.pg_prolly_log_' || 'clv.vac'::regclass::oid)::regclass
$$ LANGUAGE sql;
SELECT pg_relation_size('pg_toast.pg_prolly_log_' || 'clv.vac'::regclass::oid) > 0 AS log_has_pages;
SELECT pg_stat_force_next_flush();
SELECT pg_stat_reset_shared('prolly');
SELECT length(dolt_commit('--allow-empty', '-m', 'empty')::text) AS committed;
SELECT pg_stat_force_next_flush();
SELECT value AS log_rows_read FROM pg_stat_prolly WHERE name = 'log_pages_read';
-- the dead entries against the log's threshold: its own default (1000),
-- then the table's, set later (40000), then its own again
SELECT pg_temp.log_vacuum_score() AS default_score;
ALTER TABLE clv.vac SET (autovacuum_vacuum_threshold = 40000);
SELECT pg_temp.log_vacuum_score() AS table_threshold_score;
ALTER TABLE clv.vac RESET (autovacuum_vacuum_threshold);
SELECT pg_temp.log_vacuum_score() AS reset_score;
VACUUM clv.vac;
SELECT pg_relation_size('pg_toast.pg_prolly_log_' || 'clv.vac'::regclass::oid) AS log_bytes;
\c contrib_regression
DROP DATABASE regress_log_vacuum;

