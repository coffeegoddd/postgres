-- What a fragment's commits made is the same however it is found: walked
-- with their parents here, or, past an older fragment's sever, from the
-- delta kept for its boundary.  Each shape of history below is severed in
-- two fragments, let go of, and repaired from a remote, which verifies the
-- set found at the sever against the set found at the repair.

\getenv abs_builddir PG_ABS_BUILDDIR
\getenv abs_srcdir PG_ABS_SRCDIR
\set shape_file :abs_srcdir '/sql/history_created_shape.psql'
\set dir :abs_builddir '/tmp_check/history_created'
SELECT 'file://' || :'dir' || '/' || txid_current() AS url \gset
SELECT current_database() AS home \gset
\set VERBOSITY terse

\set shape revert
\i :shape_file
\set shape resurrect
\i :shape_file
\set shape append
\i :shape_file
\set shape merge
\i :shape_file
\set shape create_drop
\i :shape_file
\set shape amend
\i :shape_file

-- A table renamed, or moved to another schema, is the same tree under
-- another name: what the commit makes is what changed, not the table
-- again.  (The fragment's chunks count it: a few, where the table has
-- hundreds.)
CREATE DATABASE regress_history_created;
ALTER DATABASE regress_history_created SET client_min_messages = warning;
\c regress_history_created
CREATE SCHEMA hc VERSIONED;
CREATE SCHEMA hc2 VERSIONED;
CREATE TABLE hc.n (id int PRIMARY KEY, v text);
INSERT INTO hc.n SELECT g, repeat('n', 40) || g FROM generate_series(1, 20000) g;
SELECT length(dolt_commit('-Am', 'made', '--date', '2020-01-02')::text) > 0 AS committed;
ALTER TABLE hc.n RENAME TO renamed;
SELECT length(dolt_commit('-Am', 'renamed', '--date', '2020-01-03')::text) > 0 AS committed;
ALTER TABLE hc.renamed SET SCHEMA hc2;
SELECT length(dolt_commit('-Am', 'moved', '--date', '2020-01-04')::text) > 0 AS committed;
UPDATE hc2.renamed SET v = 'later' WHERE id = 1;
SELECT length(dolt_commit('-Am', 'later', '--date', '2020-01-10')::text) > 0 AS committed;
UPDATE hc2.renamed SET v = 'later' WHERE id = 2;
SELECT length(dolt_commit('-Am', 'later 2', '--date', '2020-01-11')::text) > 0 AS committed;
UPDATE hc2.renamed SET v = 'later' WHERE id = 3;
SELECT length(dolt_commit('-Am', 'later 3', '--date', '2020-01-12')::text) > 0 AS committed;
SELECT fragment, commits, chunks > 100 AS the_table FROM dolt_history_drop('2020-01-02 12:00+00');
SELECT fragment, commits, chunks < 20 AS only_what_changed FROM dolt_history_drop('2020-01-05 00:00+00');
\c :home
DROP DATABASE regress_history_created;
