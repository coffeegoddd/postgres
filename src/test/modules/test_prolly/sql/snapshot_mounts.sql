-- Snapshot mounts: a commit's tables as read-only foreign tables
-- (prolly_mount()), read from the commit's trees.  Mounting reads no row;
-- a scan seeks by the primary key's equalities and range, streams in key
-- order and stops when its consumer does, decodes only the columns used,
-- and reads only what its mount pins.
CREATE DATABASE regress_snapshot_mounts;
\c regress_snapshot_mounts
CREATE EXTENSION test_prolly;

-- since(counter): pg_stat_prolly's counter's growth since it was last
-- asked of (the session's counts flushed by the statement before).
CREATE TEMP TABLE seen (name text PRIMARY KEY, n int8);
CREATE FUNCTION pg_temp.since(counter text) RETURNS int8 LANGUAGE sql AS $$
	INSERT INTO seen SELECT name, value FROM pg_stat_prolly WHERE name = counter
	ON CONFLICT (name) DO UPDATE SET n = excluded.n
	RETURNING n - coalesce((SELECT s.n FROM seen s WHERE s.name = counter), 0)
$$;

CREATE SCHEMA ci VERSIONED;
CREATE TABLE ci.users (
	tenant_id int, id int, email text, payload text,
	PRIMARY KEY (tenant_id, id));
INSERT INTO ci.users
SELECT g / 1000, g % 1000, 'person' || lpad((g % 1000)::text, 3, '0'),
	CASE WHEN g % 7 = 0 THEN NULL ELSE repeat('x', 20) END
FROM generate_series(0, 9999) g;
-- a text key under the C collation
CREATE TABLE ci.words (w text COLLATE "C" PRIMARY KEY, n int);
INSERT INTO ci.words VALUES ('apple', 1), ('Banana', 2), ('cherry', 3), ('date', 4);
-- keyless, with copies
CREATE TABLE ci.bag (v int, note text);
INSERT INTO ci.bag VALUES (7, 'seven'), (7, 'seven'), (8, 'eight');
-- long values out of line
CREATE TABLE ci.docs (id int PRIMARY KEY, title text, body text STORAGE EXTERNAL);
INSERT INTO ci.docs SELECT g, 'doc ' || g, repeat(md5(g::text), 500) FROM generate_series(1, 20) g;
SELECT length(dolt_commit('-Am', 'first')::text) > 0 AS committed;
SELECT dolt_hashof('HEAD') AS c1 \gset
-- later changes: the mount keeps the commit's rows
UPDATE ci.users SET email = 'changed' WHERE tenant_id = 4 AND id = 42;
DELETE FROM ci.users WHERE tenant_id = 9;
ALTER TABLE ci.words ADD COLUMN extra int;
SELECT length(dolt_commit('-Am', 'second')::text) > 0 AS committed;

-- Mounting reads the commit's root items alone: no row decoded, no value
-- read, nothing folded.
SELECT pg_stat_force_next_flush() \gset
SELECT pg_temp.since('columns_decoded') >= 0 AS counting,
	pg_temp.since('fold_rows_read') >= 0 AS counting,
	pg_temp.since('nodes_read') >= 0 AS counting;
SELECT mount, commit = :'c1' AS at_c1 FROM prolly_mount(:'c1', 'ci', 'old');
SELECT pg_stat_force_next_flush() \gset
SELECT pg_temp.since('columns_decoded') AS decoded,
	pg_temp.since('fold_rows_read') AS folded,
	pg_temp.since('nodes_read') < 50 AS few_nodes;
SELECT mount, source_ref = :'c1' AS ref, source_schema, tables FROM prolly_mounts();
-- each foreign table as the commit describes the table
SELECT c.relname, a.attname, format_type(a.atttypid, a.atttypmod) AS type,
	a.attcollation <> 0 AND a.attcollation <> 100 AS own_collation,
	a.attnotnull
FROM pg_class c JOIN pg_attribute a ON a.attrelid = c.oid
WHERE c.relnamespace = 'old'::regnamespace AND a.attnum > 0
ORDER BY c.relname, a.attnum;
SELECT c.relname, s.srvname, o.option_value = :'c1' AS at_c1,
	(SELECT option_value FROM pg_options_to_table(ft.ftoptions) WHERE option_name = 'source') AS source
FROM pg_foreign_table ft JOIN pg_class c ON c.oid = ft.ftrelid
JOIN pg_foreign_server s ON s.oid = ft.ftserver,
	pg_options_to_table(ft.ftoptions) o
WHERE c.relnamespace = 'old'::regnamespace AND o.option_name = 'commit'
ORDER BY 1;

-- A point lookup by the whole key reads its row alone; an old value.
SELECT * FROM old.users WHERE tenant_id = 4 AND id = 42;
SELECT * FROM ci.users WHERE tenant_id = 4 AND id = 42;
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
SELECT * FROM old.users WHERE tenant_id = 4 AND id = 42;
SELECT count(*) FROM old.users WHERE tenant_id = 9;
-- equality on the leading column and a range after it; the key's order
-- serves ORDER BY, and LIMIT stops the scan
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
SELECT * FROM old.users WHERE tenant_id = 4 AND id >= 40 AND id < 50
ORDER BY id LIMIT 5;
SELECT tenant_id, id, email FROM old.users WHERE tenant_id = 4 AND id > 40 AND id <= 45 ORDER BY id;
SELECT tenant_id, id FROM old.users WHERE tenant_id = 4 AND id < 3 ORDER BY id DESC;
SELECT tenant_id, id FROM old.users WHERE tenant_id = 4 AND id > 997;
-- a range on the leading column alone
SELECT count(*), min(tenant_id), max(tenant_id) FROM old.users WHERE tenant_id >= 8;
-- backward, for ORDER BY ... DESC
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
SELECT tenant_id, id FROM old.users ORDER BY tenant_id DESC, id DESC LIMIT 3;
SELECT tenant_id, id FROM old.users ORDER BY tenant_id DESC, id DESC LIMIT 3;
-- a key column the query bounds otherwise (a function of it, another
-- operator) is a filter only
EXPLAIN (COSTS OFF) SELECT * FROM old.users WHERE tenant_id + 0 = 4 AND id = 1;
SELECT count(*) FROM old.users WHERE tenant_id + 0 = 4 AND id = 1;
SELECT count(*) FROM old.users WHERE tenant_id = 4 AND id = NULL::int;
SELECT count(*) FROM old.users WHERE tenant_id = 4 AND id = ANY (ARRAY[1, 2, 3]);
-- a parameterized nested loop re-seeks for each outer row
EXPLAIN (COSTS OFF)
SELECT u.* FROM (VALUES (1, 5), (4, 42), (9, 1)) v(t, i)
JOIN old.users u ON u.tenant_id = v.t AND u.id = v.i;
SELECT u.tenant_id, u.id, u.email FROM (VALUES (1, 5), (4, 42), (9, 1)) v(t, i)
JOIN old.users u ON u.tenant_id = v.t AND u.id = v.i ORDER BY 1;
PREPARE byid(int, int) AS SELECT email FROM old.users WHERE tenant_id = $1 AND id = $2;
EXECUTE byid(4, 42);
EXECUTE byid(2, 7);
DEALLOCATE byid;

-- text keys: the C collation's order, bounds under it; a comparison under
-- another collation is a filter only
SELECT w FROM old.words ORDER BY w;
SELECT w FROM old.words WHERE w >= 'b';
EXPLAIN (COSTS OFF) SELECT w FROM old.words WHERE w = 'date';
EXPLAIN (COSTS OFF) SELECT w FROM old.words WHERE w = 'date' COLLATE "POSIX";
SELECT w FROM old.words WHERE w > 'b' COLLATE "POSIX";
-- the commit's columns, not the table's now (it has extra now)
SELECT * FROM old.words WHERE w = 'apple';
-- keyless: each copy of a row
SELECT v, note, count(*) FROM old.bag GROUP BY v, note ORDER BY v;
SELECT count(*) FROM old.bag;

-- A narrow projection reads no long value of a column it doesn't use.
SELECT pg_stat_force_next_flush() \gset
SELECT pg_temp.since('values_read') >= 0 AS counting;
SELECT count(*), max(title) FROM old.docs;
SELECT pg_stat_force_next_flush() \gset
SELECT pg_temp.since('values_read') AS long_values_read;
SELECT id, length(body) FROM old.docs WHERE id = 3;
SELECT pg_stat_force_next_flush() \gset
SELECT pg_temp.since('values_read') AS long_values_read;
SELECT sum(length(body)) FROM old.docs;
SELECT pg_stat_force_next_flush() \gset
SELECT pg_temp.since('values_read') AS long_values_read;

-- Mounting reads no more as the table grows, nor does a selective lookup:
-- the columns decoded by a key lookup and a LIMIT, at two sizes.
CREATE TABLE ci.grow (id int PRIMARY KEY, v int);
INSERT INTO ci.grow SELECT g, g FROM generate_series(1, 1000) g;
SELECT length(dolt_commit('-Am', 'small')::text) > 0 AS committed;
SELECT pg_stat_force_next_flush() \gset
SELECT pg_temp.since('nodes_read') >= 0 AS counting;
SELECT mount FROM prolly_mount('HEAD', 'ci', 'small', ARRAY['grow']);
SELECT pg_stat_force_next_flush() \gset
SELECT pg_temp.since('nodes_read') AS small_mount_nodes \gset
INSERT INTO ci.grow SELECT g, g FROM generate_series(1001, 100000) g;
SELECT length(dolt_commit('-Am', 'large')::text) > 0 AS committed;
SELECT pg_stat_force_next_flush() \gset
SELECT pg_temp.since('nodes_read') >= 0 AS counting;
SELECT mount FROM prolly_mount('HEAD', 'ci', 'large', ARRAY['grow']);
SELECT pg_stat_force_next_flush() \gset
-- (the root's items and the table's description: flat in the rows)
SELECT pg_temp.since('nodes_read') <= :small_mount_nodes + 2 AS mount_flat_in_rows;
SELECT pg_stat_force_next_flush() \gset
SELECT pg_temp.since('columns_decoded') >= 0 AS counting;
SELECT v FROM small.grow WHERE id = 500;
SELECT pg_stat_force_next_flush() \gset
SELECT pg_temp.since('columns_decoded') AS small_lookup_decoded;
SELECT v FROM large.grow WHERE id = 500;
SELECT pg_stat_force_next_flush() \gset
SELECT pg_temp.since('columns_decoded') AS large_lookup_decoded;
SELECT v FROM large.grow ORDER BY id LIMIT 3;
SELECT pg_stat_force_next_flush() \gset
SELECT pg_temp.since('columns_decoded') AS large_limit_decoded;
SELECT count(*) FROM large.grow;
SELECT pg_stat_force_next_flush() \gset
SELECT pg_temp.since('columns_decoded') AS count_decoded;
SELECT prolly_unmount('small'), prolly_unmount('large');

-- Refused: the working set, a versioned target, a mount there already, a
-- table the commit hasn't, a foreign table of the server without a mount.
SELECT * FROM prolly_mount('WORKING', 'ci', 'w');
SELECT * FROM prolly_mount(:'c1', 'ci', 'ci');
SELECT * FROM prolly_mount(:'c1', 'ci', 'old');
SELECT * FROM prolly_mount('main', 'ci', 'none', ARRAY['missing']);
SELECT count(*) FROM pg_namespace WHERE nspname = 'none';
SELECT * FROM prolly_mount('nosuchref', 'ci', 'none');
CREATE SCHEMA rogue;
CREATE FOREIGN TABLE rogue.t (tenant_id int, id int, email text, payload text)
	SERVER prolly_local OPTIONS (mount 'nosuch', commit :'c1', source 'ci.users');
SELECT * FROM rogue.t;
CREATE FOREIGN TABLE rogue.u (a int) SERVER prolly_local;
SELECT * FROM rogue.u;
-- a mount's table whose columns were changed: refused, not misread
ALTER FOREIGN TABLE old.bag ADD COLUMN more int;
SELECT * FROM old.bag;
ALTER FOREIGN TABLE old.bag DROP COLUMN more;
SELECT * FROM old.bag;
-- an extension's type keying a table: refused at mount
CREATE EXTENSION test_prolly_ordering VERSION '1.0';
CREATE TABLE ci.extkey (k test_ordering PRIMARY KEY);
SELECT length(dolt_commit('-Am', 'extkey')::text) > 0 AS committed;
SELECT * FROM prolly_mount('HEAD', 'ci', 'ext', ARRAY['extkey']);
DROP TABLE ci.extkey;
SELECT length(dolt_commit('-Am', 'no extkey')::text) > 0 AS committed;

-- Who may read: SELECT on the table of that name here, row security not
-- applying, both at mount and at each scan.
CREATE ROLE regress_mount_reader;
GRANT USAGE ON SCHEMA ci, old TO regress_mount_reader;
GRANT SELECT ON ALL TABLES IN SCHEMA old TO regress_mount_reader;
SET ROLE regress_mount_reader;
SELECT count(*) FROM old.users;
SELECT * FROM prolly_mount(:'c1', 'ci', 'mine', ARRAY['bag']);
RESET ROLE;
GRANT SELECT ON ci.users TO regress_mount_reader;
SET ROLE regress_mount_reader;
SELECT count(*) FROM old.users;
SELECT prolly_unmount('old');
RESET ROLE;
ALTER TABLE ci.users ENABLE ROW LEVEL SECURITY;
SET ROLE regress_mount_reader;
SELECT count(*) FROM old.users;
RESET ROLE;
ALTER TABLE ci.users DISABLE ROW LEVEL SECURITY;
REVOKE ALL ON ci.users FROM regress_mount_reader;
REVOKE ALL ON SCHEMA ci, old FROM regress_mount_reader;
REVOKE ALL ON ALL TABLES IN SCHEMA old FROM regress_mount_reader;
DROP ROLE regress_mount_reader;

-- Refresh moves the mount to what its ref names now; a cached plan
-- reads the new commit's tables.
SELECT mount FROM prolly_mount('HEAD', 'ci', 'head', ARRAY['users']);
PREPARE n AS SELECT count(*) FROM head.users;
EXECUTE n;
DELETE FROM ci.users WHERE tenant_id = 8;
SELECT length(dolt_commit('-Am', 'third')::text) > 0 AS committed;
EXECUTE n;
SELECT prolly_refresh_mount('head') = dolt_hashof('HEAD') AS refreshed;
EXECUTE n;
SELECT prolly_refresh_mount('head', :'c1') = :'c1' AS back_to_c1;
EXECUTE n;
DEALLOCATE n;

-- A collection keeps what a mount pins, though no branch names it; an
-- unmount drops the pin.
SELECT dolt_branch('scratch');
SELECT dolt_checkout('scratch');
INSERT INTO ci.bag VALUES (99, 'only on scratch');
SELECT length(dolt_commit('-Am', 'scratch only')::text) > 0 AS committed;
SELECT dolt_hashof('HEAD') AS scratch_commit \gset
SELECT mount FROM prolly_mount('HEAD', 'ci', 'pinned', ARRAY['bag']);
SELECT dolt_checkout('main');
SELECT dolt_branch('-D', 'scratch');
SELECT dolt_gc('--full');
SELECT count(*) FROM pinned.bag WHERE v = 99;
SELECT test_chunk_has(:'scratch_commit') AS pinned_commit_kept;
SELECT prolly_unmount('pinned');
SELECT count(*) FROM prolly_mounts() WHERE mount = 'pinned';
SELECT count(*) FROM pg_class WHERE relnamespace = 'pinned'::regnamespace;

-- Keys bounded by another integer type's operators (as btree is): a bigint
-- key by an int4 value; values out of a smaller key's range are equal to
-- no key, and bound all or none.
CREATE TABLE ci.wide (id bigint PRIMARY KEY, s smallint, v int);
CREATE UNIQUE INDEX ON ci.wide (s);
INSERT INTO ci.wide SELECT g, g - 100, g FROM generate_series(1, 200) g;
CREATE TABLE ci.small (s smallint PRIMARY KEY, v int);
INSERT INTO ci.small SELECT g, g FROM generate_series(-100, 100) g;
SELECT length(dolt_commit('-Am', 'integer keys')::text) > 0 AS committed;
SELECT mount FROM prolly_mount('HEAD', 'ci', 'ints', ARRAY['wide', 'small']);
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
SELECT v FROM ints.wide WHERE id = 42;
SELECT v FROM ints.wide WHERE id = 42::smallint;
SELECT count(*) FROM ints.wide WHERE id > 190::int4 AND id <= 195::int2;
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
SELECT v FROM ints.small WHERE s = 7::int8;
SELECT count(*) FROM ints.small WHERE s = 100000::int8;
SELECT count(*) FROM ints.small WHERE s < 100000::int8;
SELECT count(*) FROM ints.small WHERE s > 100000::int8;
SELECT count(*) FROM ints.small WHERE s > -100000::int8 AND s < -95;
SELECT count(*) FROM ints.small WHERE s < -100000::int8;

-- Estimates from the tree: keys between constant bounds counted exactly.
CREATE FUNCTION pg_temp.plan_rows(q text) RETURNS float8 LANGUAGE plpgsql AS $$
DECLARE j json;
BEGIN
	EXECUTE 'EXPLAIN (FORMAT JSON) ' || q INTO j;
	RETURN (j->0->'Plan'->>'Plan Rows')::float8;
END $$;
SELECT pg_temp.plan_rows('SELECT * FROM old.users WHERE tenant_id = 4') AS tenant_rows,
	pg_temp.plan_rows('SELECT * FROM old.users WHERE tenant_id = 4 AND id < 250') AS range_rows,
	pg_temp.plan_rows('SELECT * FROM old.users WHERE tenant_id >= 8') AS last_two,
	pg_temp.plan_rows('SELECT * FROM ints.wide WHERE id BETWEEN 11 AND 30') AS between_rows;

-- Appendrel members: a union, an inheritance child and a partition of
-- mounted tables, ordered and filtered by their parents' plans.
SELECT count(*) FROM (SELECT v FROM ints.wide UNION ALL SELECT v FROM ints.wide) u;
SELECT id FROM (SELECT id FROM ints.wide UNION ALL SELECT id FROM ints.wide) u
ORDER BY id LIMIT 3;
CREATE TABLE public.wide_parent (id bigint, s smallint, v int);
ALTER FOREIGN TABLE ints.wide INHERIT public.wide_parent;
SELECT count(*), max(id) FROM public.wide_parent;
SELECT id FROM public.wide_parent WHERE id < 4 ORDER BY id;
ALTER FOREIGN TABLE ints.wide NO INHERIT public.wide_parent;
CREATE TABLE public.small_parts (s smallint, v int) PARTITION BY RANGE (s);
ALTER TABLE public.small_parts ATTACH PARTITION ints.small FOR VALUES FROM (-1000) TO (1000);
SELECT count(*), min(s) FROM public.small_parts;
SELECT s FROM public.small_parts WHERE s > 97 ORDER BY s;
ALTER TABLE public.small_parts DETACH PARTITION ints.small;
DROP TABLE public.small_parts, public.wide_parent;

-- A rescan's bounds are freed for the next: memory stays flat over many.
CREATE FUNCTION pg_temp.mem() RETURNS int8 LANGUAGE sql VOLATILE AS
	$$SELECT sum(total_bytes)::int8 FROM pg_backend_memory_contexts$$;
SELECT max(m) FILTER (WHERE g > 19000) - max(m) FILTER (WHERE g BETWEEN 900 AND 1000)
	< 1000000 AS flat
FROM (SELECT g, pg_temp.mem() AS m
	  FROM generate_series(1, 20000) g,
		   LATERAL (SELECT v FROM ints.wide w WHERE w.id = g % 200 OFFSET 0) x) t;
SELECT prolly_unmount('ints');

-- A foreign table naming a mount's pin and commit, but a table the mount
-- didn't mount (so didn't check), is refused.
SELECT mount FROM prolly_mount('HEAD', 'ci', 'justbag', ARRAY['bag']);
SELECT f.option_value AS bag_commit FROM pg_foreign_table ft,
	pg_options_to_table(ft.ftoptions) f
WHERE ft.ftrelid = 'justbag.bag'::regclass AND f.option_name = 'commit' \gset
CREATE FOREIGN TABLE justbag.users (tenant_id int, id int, email text, payload text)
	SERVER prolly_local OPTIONS (mount 'justbag', commit :'bag_commit', source 'ci.users');
SELECT count(*) FROM justbag.users;
DROP FOREIGN TABLE justbag.users;

-- Who may mount where: not into a system schema or another's, not with no
-- tables; only the mount's owner unmounts it; dropping a mount's schema
-- releases its pin.
SELECT * FROM prolly_mount('HEAD', 'ci', 'pg_catalog', ARRAY['bag']);
SELECT * FROM prolly_mount('HEAD', 'ci', 'pg_mine', ARRAY['bag']);
SELECT * FROM prolly_mount('HEAD', 'ci', 'information_schema', ARRAY['bag']);
SELECT * FROM prolly_mount('HEAD', 'ci', 'nothing', ARRAY[]::text[]);
CREATE SCHEMA empty_src;
SELECT * FROM prolly_mount('HEAD', 'empty_src', 'nothing');
CREATE ROLE regress_mount_other;
CREATE SCHEMA owned_elsewhere;
GRANT USAGE, CREATE ON SCHEMA owned_elsewhere TO regress_mount_other;
SET ROLE regress_mount_other;
SELECT * FROM prolly_mount('HEAD', 'ci', 'other', ARRAY['bag']);
SELECT * FROM prolly_mount('HEAD', 'ci', 'owned_elsewhere', ARRAY['bag']);
SELECT prolly_unmount('justbag');
RESET ROLE;
SELECT count(*) FROM prolly_mounts() WHERE mount = 'justbag';
DROP SCHEMA justbag CASCADE;
SELECT count(*) FROM prolly_mounts() WHERE mount = 'justbag';
-- (rolled back: the pin stays)
SELECT mount FROM prolly_mount('HEAD', 'ci', 'kept', ARRAY['bag']);
BEGIN;
DROP SCHEMA kept CASCADE;
SELECT count(*) FROM prolly_mounts() WHERE mount = 'kept';
ROLLBACK;
SELECT count(*) FROM prolly_mounts() WHERE mount = 'kept';
SELECT count(*) FROM kept.bag;
SELECT prolly_unmount('kept');
REVOKE ALL ON SCHEMA owned_elsewhere FROM regress_mount_other;
DROP SCHEMA kept, owned_elsewhere, empty_src;
DROP ROLE regress_mount_other;

SELECT prolly_unmount('old');
SELECT prolly_unmount('head');
SELECT count(*) FROM prolly_mounts();
\c contrib_regression
DROP DATABASE regress_snapshot_mounts;
