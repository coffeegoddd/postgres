-- base
CREATE TABLE parent (pk int PRIMARY KEY);
CREATE TABLE child (pk int PRIMARY KEY, p_fk int);
INSERT INTO parent VALUES (1);
INSERT INTO child VALUES (1, 1);
-- ours
ALTER TABLE child ADD CONSTRAINT fk_parent FOREIGN KEY (p_fk) REFERENCES parent (pk);
ALTER TABLE child ADD COLUMN col1 int;
-- theirs
INSERT INTO child VALUES (2, 2);
UPDATE child SET p_fk = 3 WHERE pk = 1;
ALTER TABLE child ADD COLUMN col2 varchar(100);
UPDATE child SET col2 = pk || 'col2';
-- expect
violation foreign key child fk_parent 1
violation foreign key child fk_parent 2
