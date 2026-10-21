-- What the database's prolly storage takes, by component (#500):
-- prolly_storage_breakdown() gives the block-mapped relations' main forks
-- as mapped and their other forks, the page store, the block maps, the
-- change logs (live and dead), the journal and the table files.  Its
-- files' columns, with the catalogs, are pg_database_size().
\c postgres
CREATE DATABASE breakdown_db;
\c breakdown_db
CREATE EXTENSION test_prolly;

-- The columns that are files, summed.
CREATE FUNCTION files_bytes() RETURNS bigint LANGUAGE sql AS $$
	SELECT vm_bytes + fsm_bytes + page_store_bytes + block_map_bytes +
		journal_bytes + settled_bytes + unsettled_bytes +
		chunk_store_other_bytes
	FROM prolly_storage_breakdown()
$$;
-- The catalogs' share: every heap relation of the database's own (not
-- shared), every fork, with its indexes and TOAST (none here is a user's).
CREATE FUNCTION catalog_bytes() RETURNS bigint LANGUAGE sql AS $$
	SELECT sum(pg_total_relation_size(c.oid))::bigint FROM pg_class c
	WHERE c.relkind IN ('r', 'm') AND NOT c.relisshared
		AND c.relam = (SELECT oid FROM pg_am WHERE amname = 'heap')
$$;
-- The files no relation owns, nor the chunk store: the relation map,
-- PG_VERSION and the relcache's init file.
CREATE FUNCTION unowned_bytes() RETURNS bigint LANGUAGE sql AS $$
	SELECT coalesce(sum((pg_stat_file(d || '/' || f, true)).size), 0)::bigint
	FROM (SELECT 'base/' || oid AS d FROM pg_database
		  WHERE datname = current_database()) db,
		LATERAL pg_ls_dir(d, true, false) f
	WHERE f ~* '^pg_'
$$;
-- What's left once those are counted: nothing, but for files made or
-- removed between the reads.  A column counting a file twice would make it
-- negative; one left out (the journal's index, a page store, a fork of
-- each table) would put it past 8 kB.
CREATE FUNCTION sums_to_database() RETURNS boolean LANGUAGE sql AS $$
	SELECT pg_database_size(current_database()) - files_bytes() -
		catalog_bytes() - unowned_bytes() BETWEEN 0 AND 8192
$$;
-- No prolly table yet: no page store, no chunks.
SELECT * FROM prolly_storage_breakdown();
SELECT sums_to_database();

CREATE SCHEMA s VERSIONED;
CREATE TABLE s.t (id int8 PRIMARY KEY, v text);
INSERT INTO s.t SELECT g, repeat('x', 100) FROM generate_series(1, 20000) g;
CHECKPOINT;

-- Loaded: the table's and its index's pages are mapped; its change log
-- holds an entry a row, all live.
SELECT heap_main_bytes >= pg_relation_size('s.t') AS heap_mapped,
	index_main_bytes >= pg_relation_size('s.t_pkey') AS index_mapped,
	page_store_allocated_bytes <= page_store_bytes AS allocated_in_store,
	page_store_mapped_bytes + page_store_listed_bytes
		<= page_store_allocated_bytes AS held_allocated,
	block_map_bytes > 0 AS maps,
	change_log_live_bytes > 0 AS log_live, change_log_dead_bytes AS log_dead,
	change_log_bytes >= change_log_live_bytes AS log_holds,
	journal_bytes AS journal
FROM prolly_storage_breakdown();
SELECT sums_to_database();

-- Committed: the fold wrote chunks, and consumed the log's entries, dead
-- until a vacuum removes them.
SELECT dolt_commit('-Am', 'load') IS NOT NULL AS committed;
CHECKPOINT;
SELECT journal_bytes > 0 AS journal, change_log_live_bytes AS log_live,
	change_log_dead_bytes > 0 AS log_dead
FROM prolly_storage_breakdown();
SELECT sums_to_database();
VACUUM s.t;
SELECT change_log_live_bytes AS log_live, change_log_dead_bytes AS log_dead
FROM prolly_storage_breakdown();

-- An update on main after a branch is made: the pages it adds and
-- changes are mapped.
SELECT dolt_branch('other') IS NOT NULL AS branched;
SELECT heap_main_bytes AS heap_before, page_store_mapped_bytes AS mapped_before
FROM prolly_storage_breakdown() \gset
UPDATE s.t SET v = repeat('y', 100) WHERE id <= 2000;
CHECKPOINT;
SELECT heap_main_bytes > :heap_before AS grew,
	page_store_mapped_bytes > :mapped_before AS copies_mapped,
	change_log_live_bytes > 0 AS log_live
FROM prolly_storage_breakdown();
SELECT sums_to_database();

-- A collection and a sweep: the files settle; the sums still hold.
SELECT dolt_commit('-Am', 'update') IS NOT NULL AS committed;
SELECT dolt_gc() IS NOT NULL AS collected;
CHECKPOINT;
SELECT freed >= 0 AS swept FROM prolly_page_sweep();
SELECT journal_bytes >= 0 AS journal, settled_bytes > 0 AS settled
FROM prolly_storage_breakdown();
SELECT sums_to_database();

\c postgres
DROP DATABASE breakdown_db;
