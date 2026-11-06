-- base
CREATE TABLE parent (pk int PRIMARY KEY, a int UNIQUE, b int UNIQUE);
CREATE TABLE child (pk int PRIMARY KEY, x int, y int, CONSTRAINT fa FOREIGN KEY (x) REFERENCES parent (a), CONSTRAINT fb FOREIGN KEY (y) REFERENCES parent (b));
INSERT INTO parent VALUES (1, 1, 1);
-- ours
DELETE FROM parent WHERE pk = 1;
-- theirs
INSERT INTO child VALUES (1, 1, 1);
-- expect
violation foreign key child fa 1
violation foreign key child fb 1
