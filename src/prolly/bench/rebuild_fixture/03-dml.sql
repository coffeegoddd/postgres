-- Row changes after the schema changes, using the new column and value.
SET search_path = rt, public;
UPDATE kinds SET m = 'meh', extra = a WHERE a % 3 = 0;
INSERT INTO events SELECT 20000 + g, date '2026-01-01' + g, 'k9', '{"late": 1}'
FROM generate_series(1, 300) g;
INSERT INTO acct (email, score) VALUES ('even@x', 2);
DELETE FROM special_item WHERE id % 2 = 0;
REFRESH MATERIALIZED VIEW kinds_summary;
