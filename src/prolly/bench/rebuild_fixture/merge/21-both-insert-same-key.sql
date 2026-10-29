-- ours
INSERT INTO rt.ticket (note) VALUES ('o');
-- theirs
INSERT INTO rt.ticket (note) VALUES ('t');
-- expect
conflict row ticket 1
