-- ours
UPDATE rt.acct SET name = 'o' WHERE id <= 10;
-- theirs
UPDATE rt.acct SET name = 't' WHERE id BETWEEN 11 AND 20;
INSERT INTO rt.item VALUES (5000, 3, 1, 'new');
-- expect
clean
check SELECT count(*) FROM rt.acct WHERE name IN ('o', 't') => 20
check SELECT note FROM rt.item WHERE id = 5000 => new
