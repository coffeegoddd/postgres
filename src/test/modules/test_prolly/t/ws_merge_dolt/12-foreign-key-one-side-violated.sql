-- base
CREATE TABLE parent (pk int PRIMARY KEY);
CREATE TABLE child (pk int PRIMARY KEY, p_fk int);
INSERT INTO parent VALUES (1);
INSERT INTO child VALUES (1, 1);
-- ours
ALTER TABLE child ADD CONSTRAINT fk_parent FOREIGN KEY (p_fk) REFERENCES parent (pk);
-- theirs
INSERT INTO child VALUES (2, 2);
UPDATE child SET p_fk = 3 WHERE pk = 1;
-- expect
violation foreign key child fk_parent 1
violation foreign key child fk_parent 2
