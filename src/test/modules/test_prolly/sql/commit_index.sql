-- The commit index: every commit a branch head reached, by date and by id,
-- with its date, height and parents, found without reading the commit.
-- Each commit is indexed as a head moves to it (a commit, a merge, a
-- transaction's commit), with the transaction; a fetch's or a clone's new
-- head indexes what it brought.

\getenv abs_builddir PG_ABS_BUILDDIR
\set dir :abs_builddir '/tmp_check/commit_index'
SELECT 'file://' || :'dir' || '/' || txid_current() AS url \gset
SELECT current_database() AS home \gset
-- (a database of its own: its history is only this test's)
CREATE DATABASE regress_commit_index;
\c regress_commit_index
SELECT current_database() AS db \gset

CREATE SCHEMA ci VERSIONED;
CREATE TABLE ci.t (id int PRIMARY KEY, v int);
SELECT length(dolt_commit('-Am', 'ci 1')::text) AS committed;

-- indexed(): does the index list exactly the commits dolt_log() does, each
-- with its date, height and parents?
CREATE FUNCTION ci_indexed(ref text DEFAULT 'main') RETURNS text
LANGUAGE sql AS $$
	WITH log AS (SELECT commit_hash, date FROM dolt_log(ref)),
	idx AS (SELECT * FROM prolly_commit_index()),
	walked AS (SELECT l.commit_hash, l.date, i.committed_at, i.parents,
					  (SELECT array_agg(parent_hash ORDER BY parent_index)
					   FROM dolt_commit_ancestors a
					   WHERE a.commit_hash = l.commit_hash
						 AND a.parent_hash IS NOT NULL) AS known
			   FROM log l LEFT JOIN idx i USING (commit_hash))
	SELECT format('%s logged, %s unindexed, %s misdated, %s misparented',
				  count(*), count(*) FILTER (WHERE committed_at IS NULL),
				  count(*) FILTER (WHERE committed_at <> date),
				  count(*) FILTER (WHERE coalesce(parents, '{}') <>
										 coalesce(known, '{}')))
	FROM walked
$$;

SELECT ci_indexed();
SELECT height, cardinality(parents) AS parents FROM prolly_commit_indexed('main');

-- Commits on two branches and a merge: heights as dolt_log's, and the
-- index in date order.
SELECT dolt_branch('side');
INSERT INTO ci.t VALUES (1, 1);
SELECT length(dolt_commit('-am', 'ci 2')::text) AS committed;
SELECT dolt_checkout('side');
INSERT INTO ci.t VALUES (2, 2);
SELECT length(dolt_commit('-am', 'ci side')::text) AS committed;
SELECT dolt_checkout('main');
SELECT (dolt_merge('side', '-m', 'ci merge'))[4] AS merged;
SELECT ci_indexed();
SELECT ci_indexed('side');
SELECT l.message, i.height, cardinality(i.parents) AS parents
FROM prolly_commit_index() i JOIN dolt_log('main') l USING (commit_hash)
ORDER BY i.committed_at, i.commit_hash;

-- A range of dates, either way; an empty range.
SELECT committed_at AS second FROM prolly_commit_index() ORDER BY 1 OFFSET 1 LIMIT 1 \gset
SELECT committed_at AS fourth FROM prolly_commit_index() ORDER BY 1 OFFSET 3 LIMIT 1 \gset
SELECT count(*) AS forward,
	   (SELECT count(*) FROM prolly_commit_index(:'second', :'fourth', true)) AS backward,
	   (SELECT array_agg(committed_at ORDER BY committed_at DESC) =
			   array(SELECT committed_at FROM prolly_commit_index(:'second', :'fourth', true))
		FROM prolly_commit_index(:'second', :'fourth', false)) AS newest_first,
	   (SELECT count(*) FROM prolly_commit_index(:'fourth', :'second', false)) AS empty,
	   (SELECT count(*) FROM prolly_commit_index(NULL, :'second', true)) AS to_second
FROM prolly_commit_index(:'second', :'fourth', false);

-- A reset moves the head to a commit already indexed: nothing new.
SELECT count(*) AS entries FROM prolly_commit_index() \gset
SELECT dolt_reset('--hard', 'HEAD~1');
SELECT count(*) - :entries AS new_entries FROM prolly_commit_index();
SELECT ci_indexed();

-- A commit rolled back leaves no entry; nor does one in a subtransaction
-- rolled back.
BEGIN;
INSERT INTO ci.t VALUES (10, 10);
SELECT dolt_commit('-am', 'ci undone') AS undone \gset
SELECT count(*) AS in_xact FROM prolly_commit_indexed(trim(both '{}' from :'undone'));
ROLLBACK;
SELECT count(*) AS after_rollback FROM prolly_commit_index()
WHERE commit_hash = trim(both '{}' from :'undone');
BEGIN;
SAVEPOINT s;
INSERT INTO ci.t VALUES (11, 11);
SELECT dolt_commit('-am', 'ci sub') AS sub \gset
ROLLBACK TO s;
COMMIT;
SELECT count(*) AS after_savepoint FROM prolly_commit_index()
WHERE commit_hash = trim(both '{}' from :'sub');
SELECT ci_indexed();

-- dolt.transaction_commit: each transaction's commit is indexed with it.
SET dolt.transaction_commit = on;
INSERT INTO ci.t VALUES (20, 20);
INSERT INTO ci.t VALUES (21, 21);
BEGIN;
INSERT INTO ci.t VALUES (22, 22);
ROLLBACK;
RESET dolt.transaction_commit;
SELECT count(*) - :entries AS txn_entries FROM prolly_commit_index();
SELECT ci_indexed();

-- A clone indexes what it brought, as its heads are set.
SELECT dolt_remote('add', 'origin', :'url');
SELECT dolt_push('origin', 'main') AS pushed;
SELECT count(*) AS logged FROM dolt_log('main') \gset
CREATE DATABASE regress_commit_index_clone;
\c regress_commit_index_clone
SELECT dolt_clone(:'url');
SELECT count(*) = :logged AS all_indexed,
	   count(*) FILTER (WHERE l.date = i.committed_at) = :logged AS dated
FROM prolly_commit_index() i JOIN dolt_log('main') l USING (commit_hash);

-- A pull fast-forwarded to commits made elsewhere: the head's new commits
-- are indexed, all of them, as it moves.
INSERT INTO ci.t VALUES (30, 30);
SELECT length(dolt_commit('-am', 'ci elsewhere 1')::text) AS committed;
INSERT INTO ci.t VALUES (31, 31);
SELECT length(dolt_commit('-am', 'ci elsewhere 2')::text) AS committed;
SELECT dolt_push('origin', 'main') AS pushed;
\c :db
DROP DATABASE regress_commit_index_clone;
SELECT count(*) AS entries FROM prolly_commit_index() \gset
SELECT (dolt_pull('origin', 'main'))[2] AS fast_forward;
SELECT count(*) - :entries AS pulled_entries FROM prolly_commit_index();
SELECT ci_indexed();

\c :home
DROP DATABASE regress_commit_index;
