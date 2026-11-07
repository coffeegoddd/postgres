-- Row changes after the schema changes, using the new column and value; a composite value whose
-- fields are all NULL, which is not a NULL; DDL and rows in one transaction; a transaction with DDL
-- that rolls back; TRUNCATE; a detached partition written as a table of its own, and attached again
-- with its rows.
SET search_path = rt;
UPDATE kinds SET m = 'meh', extra = a WHERE a % 3 = 0;
UPDATE kinds SET ad = ROW(NULL, NULL) WHERE a % 50 = 1;
INSERT INTO events SELECT 20000 + g, date '2026-01-01' + g, 'k9', '{"late": 1}'
FROM generate_series(1, 300) g;
INSERT INTO events_other VALUES (30000, date '2030-01-01', 'gone', '{}');
ALTER TABLE events ATTACH PARTITION events_other DEFAULT;
INSERT INTO events VALUES (30001, date '2031-01-01', 'back', '{}');
INSERT INTO acct (email, score) VALUES ('even@x', 2);
BEGIN;
UPDATE acct SET region = 'pre' WHERE id % 13 = 0 AND score % 2 = 0;
ALTER TABLE acct ADD COLUMN nick text;
UPDATE acct SET nick = 'n' || id WHERE id % 5 = 0 AND score % 2 = 0;
COMMIT;
BEGIN;
ALTER TABLE ledger ADD COLUMN gone int DEFAULT 1;
DELETE FROM ledger WHERE seq = 3;
ROLLBACK;
TRUNCATE ctas;
INSERT INTO ctas VALUES (1, 'back');
DELETE FROM acct WHERE id % 17 = 0;
