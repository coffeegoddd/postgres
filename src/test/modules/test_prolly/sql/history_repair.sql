-- Repairing history severed at the horizon: a fragment's chunks brought
-- back from a remote (or a clone) that has them, each re-hashed as it is
-- received, then the fragment verified against its identity and digest
-- before the horizon lets go of it.  The history reads as it did, bit for
-- bit: the log, the hashes, the diffs, every commit's tables.

\getenv abs_builddir PG_ABS_BUILDDIR
\set dir :abs_builddir '/tmp_check/history_repair'
SELECT 'file://' || :'dir' || '/' || txid_current() AS url \gset
\set url_empty :url '_empty'
SELECT current_database() AS home \gset
-- (notices without their details, which name commits by hash)
\set VERBOSITY terse
CREATE DATABASE regress_history_repair;
\c regress_history_repair

CREATE FUNCTION hr_err(q text) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE q;
	RETURN 'no error';
EXCEPTION WHEN OTHERS THEN
	RETURN regexp_replace(regexp_replace(SQLERRM, '[0-9a-f]{40}', '<hash>', 'g'),
						  '"/[^"]*"', '"<dir>"', 'g');
END $$;

CREATE SCHEMA hr VERSIONED;
CREATE TABLE hr.t (id int PRIMARY KEY, v text);
CREATE FUNCTION hr_commit(msg text, day int) RETURNS text LANGUAGE sql AS $$
	SELECT trim(both '{}' from dolt_commit('-Am', msg, '--date',
		(timestamptz '2020-01-01 00:00+00' + make_interval(days => day))::text)::text)
$$;
INSERT INTO hr.t SELECT g, 'v' || g FROM generate_series(1, 300) g;
SELECT length(hr_commit('d1', 1)) AS committed;
UPDATE hr.t SET v = 'd2' WHERE id % 3 = 0;
SELECT length(hr_commit('d2', 2)) AS committed;
DELETE FROM hr.t WHERE id > 250;
SELECT length(hr_commit('d3', 3)) AS committed;
UPDATE hr.t SET v = 'd4' WHERE id % 5 = 0;
SELECT length(hr_commit('d4', 4)) AS committed;
INSERT INTO hr.t VALUES (1000, 'd5');
SELECT length(hr_commit('d5', 5)) AS committed;
UPDATE hr.t SET v = 'd6' WHERE id = 1;
SELECT length(hr_commit('d6', 6)) AS committed;
UPDATE hr.t SET v = 'd7' WHERE id = 2;
SELECT length(hr_commit('d7', 7)) AS committed;

-- history(): the history as read, every bit that should come back the same
CREATE TABLE hr_seen (what text, n int, got text);
CREATE FUNCTION hr_history() RETURNS TABLE (what text, n int, got text)
LANGUAGE plpgsql AS $$
DECLARE
	c record;
	prev text;
BEGIN
	FOR c IN SELECT commit_hash, message, date, commit_order FROM dolt_log('main')
			 ORDER BY commit_order LOOP
		what := 'log'; n := c.commit_order;
		got := c.commit_hash || ' ' || c.message || ' ' || c.date;
		RETURN NEXT;
		what := 'root'; got := prolly_root_hash(c.commit_hash); RETURN NEXT;
		what := 'rows';
		SELECT md5(string_agg(id || '=' || v, ',' ORDER BY id)) INTO got
		FROM dolt_at(c.commit_hash, NULL::hr.t);
		RETURN NEXT;
		IF prev IS NOT NULL THEN
			what := 'diff';
			SELECT md5(string_agg(diff_type || ':' || coalesce((to_row).id, (from_row).id),
								  ',' ORDER BY coalesce((to_row).id, (from_row).id)))
			INTO got FROM dolt_diff(prev, c.commit_hash, NULL::hr.t);
			RETURN NEXT;
		END IF;
		prev := c.commit_hash;
	END LOOP;
	what := 'history_rows';
	n := 0;
	SELECT count(*)::text INTO got FROM dolt_history_table(NULL::hr.t) h;
	RETURN NEXT;
END $$;
INSERT INTO hr_seen SELECT * FROM hr_history();
SELECT what, count(*) FROM hr_seen GROUP BY what ORDER BY what;

-- A remote has the history; then it is severed here and let go of.
SELECT dolt_remote('add', 'origin', :'url');
SELECT dolt_push('origin', 'main') AS pushed;
-- (and another has none of it: a database of its own pushed there)
CREATE DATABASE regress_history_repair_other;
\c regress_history_repair_other
CREATE SCHEMA hr VERSIONED;
CREATE TABLE hr.t (id int PRIMARY KEY, v text);
SELECT length(dolt_commit('-Am', 'unrelated')::text) AS committed;
SELECT dolt_remote('add', 'empty', :'url_empty');
SELECT dolt_push('empty', 'main') AS pushed_unrelated;
\c regress_history_repair
DROP DATABASE regress_history_repair_other;
SELECT dolt_remote('add', 'empty', :'url_empty');
-- (no collection before: one would settle the history in a file the
-- severed data is too little of to rewrite, and it would stay)
SELECT fragment, commits FROM dolt_history_drop('2020-01-04 00:00+00');
SELECT fragment, commits FROM dolt_history_drop('2020-01-05 12:00+00');
SELECT chunks_freed > 0 AS freed FROM prolly_gc(true);
SELECT fragment, state, commits FROM prolly_history_horizon();
SELECT count(*) AS logged FROM dolt_log('main');

-- A deleted fragment is repaired only from where its history is; one
-- that lacks it repairs nothing.
SELECT hr_err('SELECT dolt_history_repair()') AS no_source;
SELECT hr_err($$SELECT dolt_history_repair('empty')$$) AS lacking;
SELECT count(*) AS fragments FROM prolly_history_horizon();
-- Only the database's owner repairs.
CREATE ROLE regress_hr_other;
SET ROLE regress_hr_other;
SELECT hr_err($$SELECT dolt_history_repair('origin')$$) AS other;
RESET ROLE;

-- The newest fragment alone: the log reaches its commits, and stops at
-- the older one's.
-- The older fragment's data isn't received (the source has it), so a
-- collection after keeps the newest's and frees the older's; every
-- commit object is kept.
SELECT tops[1] AS older_top FROM prolly_history_horizon() ORDER BY fragment LIMIT 1 \gset
SELECT dolt_history_repair('origin', 1) AS repaired;
SELECT fragment, commits FROM prolly_history_horizon();
SELECT count(*) AS logged FROM dolt_log('main');
SELECT hr_err(format('SELECT count(*) FROM dolt_at(%L, NULL::hr.t)', :'older_top')) AS older_not_received;
SELECT chunks_freed >= 0 AS collected FROM prolly_gc(true);
SELECT count(*) AS logged_after_collection FROM dolt_log('main');

-- The rest, by URL: the history is as it was.
SELECT dolt_history_repair(:'url') AS repaired;
SELECT count(*) AS fragments FROM prolly_history_horizon();
SELECT h.what, h.n, h.got = s.got AS same
FROM hr_history() h FULL JOIN hr_seen s USING (what, n)
WHERE h.got IS DISTINCT FROM s.got;
SELECT count(*) AS compared FROM hr_history();

-- And a collection keeps it all, the horizon gone.
SELECT chunks_freed >= 0 AS collected FROM prolly_gc(true);
SELECT h.what, h.n FROM hr_history() h FULL JOIN hr_seen s USING (what, n)
WHERE h.got IS DISTINCT FROM s.got;
SELECT hr_err('SELECT dolt_history_repair()') AS nothing_left;
-- (nulls are none: every fragment, from each one's archive; a negative
-- count is refused)
SELECT dolt_history_repair(NULL, NULL) AS null_args;
SELECT hr_err($$SELECT dolt_history_repair(NULL, -1)$$) AS negative;

\c :home
DROP DATABASE regress_history_repair;
DROP ROLE regress_hr_other;
