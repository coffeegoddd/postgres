-- ours
ALTER TABLE rt.acct RENAME COLUMN id TO acct_id;
ALTER TABLE rt.item RENAME COLUMN acct TO owner;
-- theirs
CREATE TABLE rt.note (id int PRIMARY KEY, acct int REFERENCES rt.acct (id), body text);
INSERT INTO rt.note VALUES (1, 5, 'hi');
-- expect
clean
check SELECT pg_get_constraintdef(oid) FROM pg_constraint WHERE conname = 'note_acct_fkey' => FOREIGN KEY (acct) REFERENCES rt.acct(acct_id)
check SELECT pg_get_constraintdef(oid) FROM pg_constraint WHERE conname = 'item_acct_fkey' => FOREIGN KEY (owner) REFERENCES rt.acct(acct_id)
