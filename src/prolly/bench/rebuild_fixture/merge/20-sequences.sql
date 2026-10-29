-- ours
INSERT INTO rt.ticket (note) SELECT 'o' FROM generate_series(1, 5);
-- theirs
SELECT nextval('rt.ticket_id_seq') FROM generate_series(1, 10);
-- expect
clean
check SELECT last_value FROM rt.ticket_id_seq => 10
check SELECT count(*) FROM rt.ticket => 5
