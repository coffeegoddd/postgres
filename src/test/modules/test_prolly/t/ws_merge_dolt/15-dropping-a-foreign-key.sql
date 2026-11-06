-- base
CREATE TABLE parent (pk int PRIMARY KEY);
CREATE TABLE child (pk int PRIMARY KEY, p_fk int, CONSTRAINT parent_fk FOREIGN KEY (p_fk) REFERENCES parent (pk));
INSERT INTO parent VALUES (1);
INSERT INTO child VALUES (1, 1);
-- ours
ALTER TABLE child DROP CONSTRAINT parent_fk;
DELETE FROM parent;
-- theirs
INSERT INTO child VALUES (2, 1);
-- expect
clean
check SELECT (SELECT count(*) FROM rt.parent), count(*) FROM rt.child => 0|2
