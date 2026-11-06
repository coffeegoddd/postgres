-- base
CREATE TABLE parent (pk int PRIMARY KEY);
CREATE TABLE child (fk int REFERENCES parent (pk));
INSERT INTO parent VALUES (1), (2);
INSERT INTO child VALUES (1), (2);
-- ours
DELETE FROM child WHERE fk = 1;
DELETE FROM parent WHERE pk = 1;
-- theirs
INSERT INTO child VALUES (1);
-- expect
violation foreign key child child_fk_fkey (1)
