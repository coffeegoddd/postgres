-- ours
ALTER TABLE rt.acct RENAME COLUMN name TO full_name;
UPDATE rt.acct SET full_name = 'o' WHERE id = 1000;
-- theirs
CREATE VIEW rt.top AS SELECT a.id, a.name, i.qty FROM rt.acct a JOIN rt.item i ON i.acct = a.id
	WHERE a.score > 995;
CREATE MATERIALIZED VIEW rt.top_names AS SELECT name FROM rt.acct WHERE score > 990;
-- expect
clean
check SELECT string_agg(DISTINCT full_name, ',') FROM rt.top => n996,n997,n998,n999,o
check SELECT count(*) FROM rt.top_names => 10
