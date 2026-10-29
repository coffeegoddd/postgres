-- ours
UPDATE rt.acct SET name = 'o' WHERE id = 5;
-- theirs
UPDATE rt.acct SET score = 99 WHERE id = 5;
-- expect
clean
check SELECT name, score FROM rt.acct WHERE id = 5 => o|99
