-- base
CREATE TABLE ta (pk varchar(255) PRIMARY KEY, col1 varchar(255) UNIQUE);
INSERT INTO ta VALUES ('B', '1'), ('C', '2'), ('Y', '100');
-- ours
UPDATE ta SET pk = 'A' WHERE pk = 'B';
UPDATE ta SET pk = 'Z' WHERE pk = 'Y';
-- theirs
UPDATE ta SET col1 = 'C' WHERE pk = 'C';
-- expect
clean
check SELECT string_agg(pk || ':' || col1, ',' ORDER BY pk) FROM rt.ta => A:1,C:C,Z:100
