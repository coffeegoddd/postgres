-- What a block-mapped relation's changes do to pages other maps share,
-- counted by the relation's file (pg_stat_block_mapped): pages copied at
-- their first change, as another map held them; pages claimed in place,
-- once none did; nodes of the map's tree copied.  An index's are its own.
-- And the pages shared now (pg_relation_shared_pages()).

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;

CREATE SCHEMA bms;
SET debug_block_mapped_heap = on;
CREATE TABLE bms.t (k int PRIMARY KEY, n int, pad text) WITH (fillfactor = 100);
INSERT INTO bms.t SELECT g, 0, repeat('p', 100) FROM generate_series(1, 5000) g;
VACUUM (FREEZE) bms.t;
CREATE TABLE bms.c (LIKE bms.t INCLUDING ALL) WITH (fillfactor = 100);
UPDATE pg_class SET relfilenode = test_bmap_share(pg_relation_filenode('bms.t')),
	relfrozenxid = (SELECT relfrozenxid FROM pg_class WHERE oid = 'bms.t'::regclass),
	relminmxid = (SELECT relminmxid FROM pg_class WHERE oid = 'bms.t'::regclass)
	WHERE oid = 'bms.c'::regclass;
UPDATE pg_class SET relfilenode = test_bmap_share(pg_relation_filenode('bms.t_pkey'))
	WHERE oid = 'bms.c_pkey'::regclass;

-- Every page is shared, with another map, on both sides.
SELECT pages > 0 AS has_pages, shared = pages AS all_shared,
       shared_elsewhere = pages AS all_elsewhere
  FROM pg_relation_shared_pages('bms.t');
SELECT shared = pages AS all_shared, shared_elsewhere = pages AS all_elsewhere
  FROM pg_relation_shared_pages('bms.c');
SELECT shared = pages AS all_shared FROM pg_relation_shared_pages('bms.c_pkey');

-- Nothing counted yet.
SELECT relname FROM pg_stat_block_mapped WHERE schemaname = 'bms' ORDER BY relname;

-- The copy's changes copy its pages: the heap's, and the index's (the
-- updates aren't HOT, the pages being full), and its tree's nodes.
UPDATE bms.c SET n = 1 WHERE k <= 200;
SELECT pg_stat_force_next_flush();
SELECT relname, pages_copied > 0 AS copied, pages_claimed AS claimed,
       nodes_copied > 0 AS nodes, stats_reset IS NULL AS never_reset
  FROM pg_stat_block_mapped WHERE schemaname = 'bms' ORDER BY relname;
SELECT shared < pages AS some_own, shared > 0 AS some_shared
  FROM pg_relation_shared_pages('bms.c');

-- The source, changing the pages the copy copied, has them alone: it
-- claims them in place rather than copy them.
UPDATE bms.t SET n = 2 WHERE k <= 200;
SELECT pg_stat_force_next_flush();
SELECT relname, pages_claimed > 0 AS claimed
  FROM pg_stat_block_mapped WHERE relname = 't' AND schemaname = 'bms';
SELECT (SELECT pages_copied FROM pg_stat_block_mapped WHERE relid = 'bms.t'::regclass) <
       (SELECT pages_claimed FROM pg_stat_block_mapped WHERE relid = 'bms.t'::regclass)
       AS claims_outnumber_copies;

-- A rewrite gives the relation a new file, whose counts begin anew.
TRUNCATE bms.c;
SELECT * FROM pg_stat_get_block_map('bms.c'::regclass);

-- A relation on md.c has neither.
SET debug_block_mapped_heap = off;
CREATE TABLE bms.m (a int);
INSERT INTO bms.m VALUES (1);
SELECT * FROM pg_relation_shared_pages('bms.m');
SELECT * FROM pg_stat_get_block_map('bms.m'::regclass);

RESET debug_block_mapped_heap;
DROP SCHEMA bms CASCADE;
