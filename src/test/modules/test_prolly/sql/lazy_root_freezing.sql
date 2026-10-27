-- Under dolt.transaction_commit a commit's roots are captured among the
-- writers and kept unfrozen, with the commit's snapshot, to be frozen
-- later (the lazy root freezer).  A move to such a commit freezes them
-- first, as of that snapshot, from the relation's nearest frozen root
-- before: its pages then hold the commit's rows, frozen, whichever
-- session moves and whatever its own setting.

SELECT current_database() AS home \gset
CREATE DATABASE lazy_root_freezing;
ALTER DATABASE lazy_root_freezing SET prolly.restore_min_changes = 0;
ALTER DATABASE lazy_root_freezing SET prolly.kept_commits = 100;
\c lazy_root_freezing
SET client_min_messages = warning;
CREATE EXTENSION pageinspect;
RESET client_min_messages;

CREATE FUNCTION counted() RETURNS jsonb LANGUAGE sql AS $$
	SELECT jsonb_object_agg(name, value) FROM pg_stat_prolly
	WHERE name IN ('apply_rows', 'relations_restored', 'roots_frozen_later')
$$;
CREATE FUNCTION since(before jsonb) RETURNS TABLE (name text, value bigint)
LANGUAGE sql AS $$
	SELECT key, value::bigint - (before ->> key)::bigint
	FROM jsonb_each_text(counted()) ORDER BY 1
$$;

CREATE SCHEMA lz VERSIONED;
CREATE TABLE lz.t (id int PRIMARY KEY, v int NOT NULL, w text NOT NULL)
	WITH (autovacuum_enabled = off);
CREATE INDEX t_v ON lz.t (v);
INSERT INTO lz.t SELECT g, 0, repeat('w', 40) FROM generate_series(1, 3000) g;
SELECT dolt_commit('-Am', 'base') IS NOT NULL AS committed;
-- the versions not frozen on the table's pages
CREATE FUNCTION not_frozen() RETURNS bigint LANGUAGE sql AS $$
	SELECT count(*) FILTER (WHERE lp_flags = 1 AND (t_infomask & 768) <> 768)
	FROM generate_series(0, pg_relation_size('lz.t') / 8192 - 1) b,
		LATERAL heap_page_items(get_raw_page('lz.t', b::int))
$$;

-- each transaction a commit, each commit's roots kept unfrozen
SET dolt.transaction_commit = on;
SET prolly.root_interval = 0;
UPDATE lz.t SET v = 1 WHERE id % 10 = 1;
UPDATE lz.t SET v = 2 WHERE id % 10 = 2;
SELECT md5(string_agg(t::text, ',' ORDER BY id)) AS at_c2 FROM lz.t t \gset
UPDATE lz.t SET v = 3 WHERE id % 10 = 3;
RESET prolly.root_interval;
RESET dolt.transaction_commit;

-- a hard reset to the commit before the head, with a dirty working set:
-- its roots frozen then, its pages given back, no row written
UPDATE lz.t SET v = -1 WHERE id <= 2000;
SELECT pg_stat_force_next_flush();
SELECT counted() AS before \gset
SELECT dolt_reset('--hard', 'HEAD~1');
SELECT pg_stat_force_next_flush();
SELECT * FROM since(:'before');
SELECT md5(string_agg(t::text, ',' ORDER BY id)) = :'at_c2' AS at_c2 FROM lz.t t;
SELECT not_frozen();
SET enable_seqscan = off;
SELECT count(*) AS by_index FROM lz.t WHERE v = 2;
RESET enable_seqscan;

-- and back to it later: frozen once, its frozen roots kept
UPDATE lz.t SET v = 4 WHERE id <= 10;
SELECT dolt_commit('-am', 'c4') IS NOT NULL AS committed;
SELECT pg_stat_force_next_flush();
SELECT counted() AS before \gset
SELECT dolt_reset('--hard', 'HEAD~1');
SELECT pg_stat_force_next_flush();
SELECT * FROM since(:'before');
SELECT md5(string_agg(t::text, ',' ORDER BY id)) = :'at_c2' AS at_c2 FROM lz.t t;

\c :home
DROP DATABASE lazy_root_freezing;
