-- base
CREATE TABLE test (pk int PRIMARY KEY, c0 int);
INSERT INTO test VALUES (1, 1), (3, 3);
-- ours
ALTER TABLE test ALTER COLUMN c0 SET NOT NULL;
INSERT INTO test VALUES (4, 4);
-- theirs
INSERT INTO test VALUES (2, NULL);
-- expect
violation not null test test_c0_not_null 2
