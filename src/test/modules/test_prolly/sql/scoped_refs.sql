-- A transaction's refs changes are kept pending and applied in one edit
-- of the refs when the tree is next read whole, rebuilt or published.  A
-- subtransaction that applies them (a read of the refs in a savepoint, or
-- in a PL/pgSQL block with an exception handler) and is rolled back
-- leaves them as they were before it, so the transaction's commit
-- publishes; and pending changes applied, or saved for a subtransaction,
-- are let go of, so a long transaction's memory stays bounded.
CREATE DATABASE regress_scoped_refs;
\c regress_scoped_refs
CREATE EXTENSION test_prolly;
CREATE SCHEMA s VERSIONED;
CREATE TABLE s.t (id int PRIMARY KEY, v text);
INSERT INTO s.t VALUES (1, 'a');
SELECT length(dolt_commit('-Am', 'base')::text) AS committed;

-- a savepoint reading the refs, rolled back, after the transaction's commit
BEGIN;
UPDATE s.t SET v = 'b';
SELECT length(dolt_commit('-am', 'c1')::text) AS committed;
SAVEPOINT sp;
SELECT count(*) > 0 AS listed FROM dolt_branches;
ROLLBACK TO sp;
COMMIT;
SELECT message FROM dolt_log ORDER BY date DESC LIMIT 1;

-- the same in a PL/pgSQL block with an exception handler
DO $$
BEGIN
	UPDATE s.t SET v = 'c';
	PERFORM dolt_commit('-am', 'c2');
	BEGIN
		PERFORM count(*) FROM dolt_branches;
		RAISE EXCEPTION 'undone';
	EXCEPTION WHEN others THEN
		NULL;
	END;
END $$;
SELECT message FROM dolt_log ORDER BY date DESC LIMIT 1;
SELECT v FROM s.t;

-- 300 commits in one transaction, then each with a block rolled back
-- after it: the transaction's memory stays bounded (the pending changes,
-- a branch's kept commits among them, grow with each commit)
CREATE TEMP TABLE held (what text, bounded bool);
DO $$
BEGIN
	FOR i IN 1..300 LOOP
		PERFORM dolt_commit('--allow-empty', '-m', 'e' || i);
	END LOOP;
	INSERT INTO held VALUES ('commits', test_transaction_bytes() < 4000000);
END $$;
DO $$
BEGIN
	FOR i IN 1..300 LOOP
		PERFORM dolt_commit('--allow-empty', '-m', 'f' || i);
		BEGIN
			PERFORM count(*) FROM dolt_branches;
			RAISE EXCEPTION 'undone';
		EXCEPTION WHEN others THEN
			NULL;
		END;
	END LOOP;
	INSERT INTO held VALUES ('commits and rolled back blocks', test_transaction_bytes() < 4000000);
END $$;
SELECT * FROM held;
SELECT count(*) AS commits FROM dolt_log;

\c contrib_regression
DROP DATABASE regress_scoped_refs;
