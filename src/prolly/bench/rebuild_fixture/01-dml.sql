-- Row changes across every table: updates, deletes, inserts, rows moving
-- between partitions, large values replaced.
SET search_path = rt, public;
UPDATE kinds SET jb = jb || '{"upd": true}', t = upper(t),
	arr = CASE WHEN array_ndims(arr) > 1 THEN arr ELSE arr || 999 END WHERE a % 10 = 0;
UPDATE kinds SET big = repeat('changed ', 30000) WHERE big IS NOT NULL AND a % 2 = 0;
DELETE FROM kinds WHERE a % 97 = 0;
INSERT INTO kinds (a, b, t, vc, u, pt, bx, r, ip, dom, m, ts, i8)
SELECT 5000 + g, 'new', 'fresh ' || g, 'new' || g, md5('new' || g)::uuid,
	point(-g, -g), box(point(-g, -g), point(-g + 1, -g + 1)), int4range(-g, 0),
	('192.168.0.' || g)::inet, g, 'happy', timestamp '2030-01-01' + g * interval '1 day', -g
FROM generate_series(1, 200) g;
DELETE FROM nokey WHERE x = 3;
INSERT INTO nokey SELECT 99, 'dup' FROM generate_series(1, 5);
UPDATE acct SET score = 100 - score, region = 'moved' WHERE id % 7 = 0;
DELETE FROM acct WHERE id % 11 = 0;
INSERT INTO acct (email, region, score) SELECT 'late' || g || '@x', 'r9', 50 FROM generate_series(1, 50) g;
UPDATE ledger SET amount = amount * 2 WHERE seq = 2;
UPDATE ledger SET seq = seq + 100 WHERE seq = 1 AND acct_id % 2 = 0;
UPDATE node SET parent = 1 WHERE id % 13 = 0;
DELETE FROM booking WHERE room = 3;
UPDATE events SET at = at + 400 WHERE id % 5 = 0 AND at < '2024-06-01';
DELETE FROM events WHERE id % 9 = 0;
INSERT INTO tag VALUES ('Z', 'b');
UPDATE slot SET pos = pos + 1000 WHERE pos > 400;
INSERT INTO ticket (note) VALUES ('after');
SELECT nextval('seq_odd') FROM generate_series(1, 30);
REFRESH MATERIALIZED VIEW kinds_summary;
