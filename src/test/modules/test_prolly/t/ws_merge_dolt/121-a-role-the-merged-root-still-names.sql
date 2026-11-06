-- base
CREATE TABLE a (id int);
-- ours
CREATE TABLE c (id int);
-- theirs
DROP TABLE a;
-- expect
clean
