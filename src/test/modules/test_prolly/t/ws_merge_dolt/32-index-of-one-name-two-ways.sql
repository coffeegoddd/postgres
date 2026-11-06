-- base
CREATE TABLE t (pk int PRIMARY KEY, col1 int, col2 varchar(100));
-- ours
CREATE INDEX idx1 ON t (col2 varchar_pattern_ops);
-- theirs
CREATE INDEX idx1 ON t (col2);
-- expect
conflict table-ddl t
