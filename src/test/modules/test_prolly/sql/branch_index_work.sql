-- A branch's making, a first use (of kept pages, of the nearest branch, or
-- of the branch the move planner estimates cheapest) and a move that
-- shares other branches' pages find the branches they need through the
-- refs' indexes (of kept commits by commit and by table, of branches by
-- their heads' and baselines' roots, of names case folded) and the
-- generation table, not by listing every branch: so branches_listed (the
-- branches listings visit) is the same with 200 branches as with 2, and
-- the estimator lists only its candidates, in name order.  The indexes
-- change with what they index: they move with a branch's rename, go with
-- its delete, and roll back with the transaction.
-- (In a database of its own: other tests' branches would be listed too.)

SELECT current_database() AS home \gset
CREATE DATABASE branch_index_work;
\c branch_index_work
SET client_min_messages = warning;
CREATE EXTENSION test_prolly;
RESET client_min_messages;

\set flush 'SELECT pg_stat_force_next_flush();'
CREATE TEMP TABLE seen (name text PRIMARY KEY, n int8);
CREATE FUNCTION pg_temp.since(counter text) RETURNS int8 LANGUAGE sql AS $$
	INSERT INTO seen SELECT name, value FROM pg_stat_prolly WHERE name = counter
	ON CONFLICT (name) DO UPDATE SET n = excluded.n
	RETURNING n - coalesce((SELECT s.n FROM seen s WHERE s.name = counter), 0)
$$;
CREATE TEMP TABLE runs (branches int, what text, listed int8);
CREATE FUNCTION pg_temp.used(int, int) RETURNS int8 LANGUAGE sql AS $$
	SELECT count(dolt_branch('bix_' || lpad(g::text, 4, '0'))) +
		   count(dolt_checkout('bix_' || lpad(g::text, 4, '0')))
	FROM generate_series($1, $2) g
$$;

CREATE SCHEMA bix VERSIONED;
CREATE TABLE bix.t (id int PRIMARY KEY, v int);
INSERT INTO bix.t SELECT g, g FROM generate_series(1, 200) g;
SELECT length(dolt_commit('-Am', 'base')::text) > 0 AS committed;
SELECT pg_temp.used(1, 2) > 0 AS made;
SELECT (dolt_checkout('main'))[1];

-- Each operation, with n branches: a branch's making; its first use at
-- main's head, made of the pages main keeps as of it; with no pages kept,
-- a first use at a root main's working set is at (the nearest branch); a
-- first use at a commit no branch is at (the estimator's candidates); and
-- a fast-forward merge, whose move shares pages where it can.
CREATE FUNCTION pg_temp.measure(n int) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	UPDATE bix.t SET v = v + 1 WHERE id = 1;
	PERFORM dolt_commit('-am', 'c1 ' || n);
	UPDATE bix.t SET v = v + 1 WHERE id = 2;
	PERFORM dolt_commit('-am', 'c2 ' || n);
END $$;
CREATE FUNCTION pg_temp.record(n int, what text) RETURNS void LANGUAGE sql AS $$
	INSERT INTO runs SELECT n, what, pg_temp.since('branches_listed')
$$;

\set n 2
-- (n branches)
:flush
SELECT pg_temp.since('branches_listed') IS NOT NULL AS counting;
SELECT dolt_branch('bix_new_' || :n) IS NOT NULL AS made;
:flush
SELECT pg_temp.record(:n, 'create');
SELECT (dolt_checkout('bix_new_' || :n))[1];
:flush
SELECT pg_temp.record(:n, 'first use kept');
SELECT (dolt_checkout('main'))[1];
SET debug_prolly_page_roots = off;
SELECT dolt_branch('bix_near_' || :n) IS NOT NULL AS made;
:flush
SELECT pg_temp.since('branches_listed') IS NOT NULL AS counting;
SELECT (dolt_checkout('bix_near_' || :n))[1];
:flush
SELECT pg_temp.record(:n, 'first use nearest');
SELECT (dolt_checkout('main'))[1];
SELECT pg_temp.measure(:n);
SELECT dolt_branch('bix_old_' || :n, 'HEAD~1') IS NOT NULL AS made;
:flush
SELECT pg_temp.since('branches_listed') IS NOT NULL AS counting;
SELECT (dolt_checkout('bix_old_' || :n))[1];
:flush
SELECT pg_temp.record(:n, 'first use estimated');
SELECT (dolt_checkout('main'))[1];
RESET debug_prolly_page_roots;
SELECT dolt_branch('bix_ff_' || :n) IS NOT NULL AS made;
SELECT (dolt_checkout('bix_ff_' || :n))[1];
UPDATE bix.t SET v = v + 1 WHERE id = 4;
SELECT length(dolt_commit('-am', 'ff ' || :n)::text) > 0 AS committed;
SELECT (dolt_checkout('main'))[1];
:flush
SELECT pg_temp.since('branches_listed') IS NOT NULL AS counting;
SELECT (dolt_merge('bix_ff_' || :n))[2] AS fast_forward;
:flush
SELECT pg_temp.record(:n, 'fast-forward merge');
SELECT pg_temp.used(3, 200) > 0 AS made;
SELECT (dolt_checkout('main'))[1];
\set n 200
-- (n branches)
:flush
SELECT pg_temp.since('branches_listed') IS NOT NULL AS counting;
SELECT dolt_branch('bix_new_' || :n) IS NOT NULL AS made;
:flush
SELECT pg_temp.record(:n, 'create');
SELECT (dolt_checkout('bix_new_' || :n))[1];
:flush
SELECT pg_temp.record(:n, 'first use kept');
SELECT (dolt_checkout('main'))[1];
SET debug_prolly_page_roots = off;
SELECT dolt_branch('bix_near_' || :n) IS NOT NULL AS made;
:flush
SELECT pg_temp.since('branches_listed') IS NOT NULL AS counting;
SELECT (dolt_checkout('bix_near_' || :n))[1];
:flush
SELECT pg_temp.record(:n, 'first use nearest');
SELECT (dolt_checkout('main'))[1];
SELECT pg_temp.measure(:n);
SELECT dolt_branch('bix_old_' || :n, 'HEAD~1') IS NOT NULL AS made;
:flush
SELECT pg_temp.since('branches_listed') IS NOT NULL AS counting;
SELECT (dolt_checkout('bix_old_' || :n))[1];
:flush
SELECT pg_temp.record(:n, 'first use estimated');
SELECT (dolt_checkout('main'))[1];
RESET debug_prolly_page_roots;
SELECT dolt_branch('bix_ff_' || :n) IS NOT NULL AS made;
SELECT (dolt_checkout('bix_ff_' || :n))[1];
UPDATE bix.t SET v = v + 1 WHERE id = 4;
SELECT length(dolt_commit('-am', 'ff ' || :n)::text) > 0 AS committed;
SELECT (dolt_checkout('main'))[1];
:flush
SELECT pg_temp.since('branches_listed') IS NOT NULL AS counting;
SELECT (dolt_merge('bix_ff_' || :n))[2] AS fast_forward;
:flush
SELECT pg_temp.record(:n, 'fast-forward merge');

SELECT what, a.listed AS with_2, b.listed AS with_200,
	CASE WHEN what = 'first use estimated' THEN b.listed <= 17 + 2
		 ELSE a.listed = b.listed END AS bounded
FROM runs a JOIN runs b USING (what)
WHERE a.branches = 2 AND b.branches = 200
ORDER BY what;

-- A name that differs from a branch's only by case is still refused.
SELECT dolt_branch('BIX_0002');
SELECT dolt_branch('-m', 'bix_0003', 'Bix_0002');

-- The indexes, as committed, against what they index, each way: each
-- branch with a head is under its head's root ('r' kind 'h') and its
-- baseline's fold's ('r' kind 'w'), with its name folded ('f'); each
-- live kept commit has its 'k' and 's' entries, and no others are left.
CREATE FUNCTION pg_temp.ix() RETURNS TABLE (tag text, branch text, keyed text,
	value text) LANGUAGE sql AS $$ SELECT * FROM test_refs_index() $$;
CREATE FUNCTION pg_temp.check_ix() RETURNS TABLE (problem text, n int8)
LANGUAGE sql AS $$
	WITH b AS (SELECT name FROM dolt_branches() WHERE hash IS NOT NULL),
	ix AS (SELECT * FROM test_refs_index()),
	w AS (SELECT b.name, encode(substring(v FROM 23 FOR 20), 'hex') AS root
		  FROM b, LATERAL test_refs_value('W' || b.name) v WHERE length(v) >= 42),
	live AS (SELECT b.name, kc.commit_id FROM b, LATERAL test_kept_commits(b.name) kc)
	SELECT 'head root not indexed', count(*) FROM b
	WHERE NOT EXISTS (SELECT FROM ix r WHERE r.tag = 'r' AND r.branch = b.name
		AND r.keyed = '14' || prolly_root_hash(b.name) || '68')
	UNION ALL
	SELECT 'root index without its head', count(*) FROM ix r
	WHERE r.tag = 'r' AND right(r.keyed, 2) = '68' AND NOT EXISTS (
		SELECT FROM b WHERE b.name = r.branch
		AND r.keyed = '14' || prolly_root_hash(b.name) || '68')
	UNION ALL
	SELECT 'baseline root not indexed', count(*) FROM w
	WHERE NOT EXISTS (SELECT FROM ix r WHERE r.tag = 'r' AND r.branch = w.name
		AND r.keyed = '14' || w.root || '77')
	UNION ALL
	SELECT 'root index without its baseline', count(*) FROM ix r
	WHERE r.tag = 'r' AND right(r.keyed, 2) = '77' AND NOT EXISTS (
		SELECT FROM w WHERE w.name = r.branch AND r.keyed = '14' || w.root || '77')
	UNION ALL
	SELECT 'name not folded', count(*) FROM b
	WHERE NOT EXISTS (SELECT FROM ix i WHERE i.tag = 'f' AND i.branch = b.name
		AND i.keyed = lower(b.name))
	UNION ALL
	SELECT 'index of no branch', count(*) FROM ix i
	WHERE NOT EXISTS (SELECT FROM b WHERE b.name = i.branch)
		AND i.branch NOT LIKE 'detached/%'
	UNION ALL
	SELECT 'kept commit not indexed', count(*) FROM live
	WHERE NOT EXISTS (SELECT FROM ix i WHERE i.tag = 'k' AND i.branch = live.name
		AND i.keyed = '14' || live.commit_id)
	UNION ALL
	SELECT 'kept index of no live kept commit', count(*) FROM ix i
	WHERE i.tag IN ('k', 's') AND NOT EXISTS (SELECT FROM live
		WHERE live.name = i.branch AND right(i.keyed, 40) = live.commit_id)
	ORDER BY 1
$$;
SELECT * FROM pg_temp.check_ix();
SELECT count(*) FILTER (WHERE tag = 'k') > 0 AS kept_indexed,
	count(*) FILTER (WHERE tag = 's') > 0 AS tables_indexed,
	count(*) FILTER (WHERE tag = 'r' AND right(keyed, 2) = '77') > 0 AS baselines_indexed
FROM pg_temp.ix();

-- bix_0001 committed on (pages kept), then renamed: its entries move to
-- the new name, as they were; renamed back in a transaction rolled back,
-- nothing changes.
SELECT (dolt_checkout('bix_0001'))[1];
UPDATE bix.t SET v = -1 WHERE id = 3;
SELECT length(dolt_commit('-am', 'on 0001')::text) > 0 AS committed;
SELECT (dolt_checkout('main'))[1];
CREATE TEMP TABLE before AS SELECT * FROM pg_temp.ix() WHERE branch = 'bix_0001';
SELECT count(DISTINCT tag) AS tags_of_0001 FROM before;
SELECT dolt_branch('-m', 'bix_0001', 'bix_moved') IS NOT NULL AS renamed;
SELECT count(*) AS left_under_old FROM pg_temp.ix() WHERE branch = 'bix_0001';
SELECT (SELECT count(*) FROM (
	SELECT tag, keyed, value FROM before
	EXCEPT SELECT tag, CASE WHEN tag = 'f' THEN 'bix_0001' ELSE keyed END, value
		FROM pg_temp.ix() WHERE branch = 'bix_moved') d) AS missing,
	(SELECT count(*) FROM pg_temp.ix() WHERE branch = 'bix_moved') =
	(SELECT count(*) FROM before) AS same_count,
	(SELECT keyed FROM pg_temp.ix() WHERE branch = 'bix_moved' AND tag = 'f') AS folded;
SELECT * FROM pg_temp.check_ix();
CREATE TEMP TABLE all_before AS SELECT * FROM pg_temp.ix();
BEGIN;
SELECT dolt_branch('-m', 'bix_moved', 'bix_0001') IS NOT NULL AS renamed;
SELECT dolt_branch('-D', 'bix_0002') IS NOT NULL AS deleted;
ROLLBACK;
SELECT (SELECT count(*) FROM (SELECT * FROM all_before EXCEPT SELECT * FROM pg_temp.ix()) a) +
	(SELECT count(*) FROM (SELECT * FROM pg_temp.ix() EXCEPT SELECT * FROM all_before) b)
	AS changed_by_rollback;

-- Deleted: its entries go with it, and a branch made again of the name
-- finds nothing of the old one's.
SELECT dolt_branch('-D', 'bix_moved') IS NOT NULL AS deleted;
SELECT count(*) AS left_of_deleted FROM pg_temp.ix() WHERE branch = 'bix_moved';
SELECT dolt_branch('bix_moved') IS NOT NULL AS made_again;
SELECT tag, keyed = lower(branch) AS folded FROM pg_temp.ix()
WHERE branch = 'bix_moved' AND tag IN ('f', 'k', 's') ORDER BY tag;
SELECT * FROM pg_temp.check_ix();

-- A kept commit read in a transaction that rolls back, then the branch
-- deleted: the delete reads the branch's kept commits again (what was
-- read is forgotten as the transaction ends), so none of their index
-- entries is left for a branch made again of the name.
SELECT dolt_branch('bix_kc') IS NOT NULL AS made;
SELECT (dolt_checkout('bix_kc'))[1];
UPDATE bix.t SET v = v + 1 WHERE id = 5;
SELECT length(dolt_commit('-am', 'kc')::text) > 0 AS committed;
SELECT (dolt_checkout('main'))[1];
BEGIN;
SELECT (dolt_checkout('bix_kc'))[1];
UPDATE bix.t SET v = v + 1 WHERE id = 6;
SELECT length(dolt_commit('-am', 'kc rolled back')::text) > 0 AS committed;
SELECT count(*) > 0 AS kept FROM test_kept_commits('bix_kc');
SELECT (dolt_checkout('main'))[1];
ROLLBACK;
SELECT count(*) > 0 AS kept_indexed FROM pg_temp.ix() WHERE branch = 'bix_kc' AND tag = 'k';
SELECT dolt_branch('-D', 'bix_kc') IS NOT NULL AS deleted;
SELECT count(*) AS left_of_deleted FROM pg_temp.ix() WHERE branch = 'bix_kc';
SELECT * FROM pg_temp.check_ix();

-- Many commits in one transaction: the kept commits each retires pile up
-- in the branch's 'K' entry until a later transaction lets go of them,
-- but only the live ones are indexed, so each commit's upkeep of the
-- index is a few entries however many came before it, and the refs nodes
-- a commit writes stay flat.
CREATE FUNCTION pg_temp.commits(n int) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	FOR i IN 1 .. n LOOP
		PERFORM dolt_commit('--allow-empty', '-m', 'empty ' || i);
	END LOOP;
END $$;
:flush
SELECT pg_temp.since('refs_nodes_written') IS NOT NULL AS counting;
SELECT pg_temp.commits(100);
:flush
SELECT pg_temp.since('refs_nodes_written') / 100.0 AS per_commit_100 \gset
SELECT count(*) AS main_kept_indexed FROM pg_temp.ix() WHERE tag = 'k' AND branch = 'main';
SELECT pg_temp.commits(400);
:flush
SELECT pg_temp.since('refs_nodes_written') / 400.0 AS per_commit_400 \gset
SELECT count(*) AS main_kept_indexed FROM pg_temp.ix() WHERE tag = 'k' AND branch = 'main';
SELECT :per_commit_400 <= 1.5 * :per_commit_100 AS written_flat;
SELECT * FROM pg_temp.check_ix();

-- The case check sees branches without a head too: in a database with
-- no commits, a branch is made with copies alone, and main has no head.
-- A rename moves the folded name, and a delete takes it out.
\c :home
DROP DATABASE branch_index_work;
CREATE DATABASE branch_index_case;
\c branch_index_case
SELECT dolt_branch('feat') IS NOT NULL AS made;
SELECT dolt_branch('FEAT');
SELECT dolt_branch('MAIN');
SELECT dolt_branch('-m', 'feat', 'Main');
SELECT dolt_branch('-m', 'feat', 'feat2') IS NOT NULL AS renamed;
SELECT dolt_branch('FEAT') IS NOT NULL AS made_after_rename;
SELECT dolt_branch('Feat2');
SELECT dolt_branch('-D', 'feat2') IS NOT NULL AS deleted;
SELECT dolt_branch('FEAT2') IS NOT NULL AS made_after_delete;
SELECT name FROM dolt_branches() ORDER BY name;
\c :home
DROP DATABASE branch_index_case;
