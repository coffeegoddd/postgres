-- ours
INSERT INTO rt.acct VALUES (2000, 'new@x', 'new', 1);
-- theirs
ALTER TABLE rt.acct ADD COLUMN code text NOT NULL DEFAULT 'c';
-- expect
clean
check SELECT code FROM rt.acct WHERE id = 2000 => c
