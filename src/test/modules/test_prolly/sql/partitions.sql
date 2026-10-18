-- Partitioned tables in versioned schemas: a partitioned table held with
-- its key (no rows of its own), each partition as a table with its parent
-- and bound, its rows folded as any table's; a move makes a partitioned
-- table before its partitions, and what a partition has of its parent
-- (columns, keys, indexes, row triggers) comes with it.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;

CREATE SCHEMA pt VERSIONED;
CREATE TABLE pt.base (id int PRIMARY KEY);
SELECT length(dolt_commit('-Am', 'base')::text) AS committed;
SELECT dolt_branch('pt_reset');
SELECT dolt_branch('pt_merge');

CREATE TABLE pt.p (id int, region text, v text, PRIMARY KEY (id, region))
  PARTITION BY LIST (region);
CREATE TABLE pt.p_east PARTITION OF pt.p FOR VALUES IN ('east');
CREATE TABLE pt.p_west PARTITION OF pt.p FOR VALUES IN ('west')
  PARTITION BY RANGE (id);
CREATE TABLE pt.p_west_lo PARTITION OF pt.p_west FOR VALUES FROM (MINVALUE) TO (100);
CREATE TABLE pt.p_west_hi PARTITION OF pt.p_west FOR VALUES FROM (100) TO (MAXVALUE);
CREATE TABLE pt.p_rest PARTITION OF pt.p DEFAULT;
CREATE INDEX p_v ON pt.p (v);
CREATE FUNCTION pt.stamp() RETURNS trigger LANGUAGE plpgsql
  AS $$ BEGIN NEW.v := coalesce(NEW.v, 'stamped'); RETURN NEW; END $$;
CREATE TRIGGER p_stamp BEFORE INSERT ON pt.p FOR EACH ROW EXECUTE FUNCTION pt.stamp();
INSERT INTO pt.p VALUES (1, 'east', 'a'), (2, 'west', 'b'), (150, 'west', 'c'),
  (3, 'north', NULL);
SELECT length(dolt_commit('-Am', 'partitioned')::text) AS committed;

CREATE FUNCTION pg_temp.show() RETURNS SETOF text LANGUAGE sql AS $$
	SELECT 'part ' || c.relname || ' of ' || p.relname || ' ' ||
		   pg_get_expr(c.relpartbound, c.oid)
	  FROM pg_inherits i JOIN pg_class c ON c.oid = i.inhrelid
	  JOIN pg_class p ON p.oid = i.inhparent
	  WHERE c.relnamespace = 'pt'::regnamespace AND c.relkind IN ('r', 'p')
	UNION ALL
	SELECT 'key ' || relname || ' ' || pg_get_partkeydef(oid) FROM pg_class
	  WHERE relnamespace = 'pt'::regnamespace AND relkind = 'p'
	UNION ALL
	SELECT 'index ' || indexname || ' on ' || tablename FROM pg_indexes
	  WHERE schemaname = 'pt' AND tablename <> 'base'
	UNION ALL
	SELECT 'row ' || tableoid::regclass || ' ' || id || ' ' || region || ' ' || v FROM pt.p
	ORDER BY 1
$$;
SELECT * FROM pg_temp.show();

-- a reset from a branch without them makes them from the commit
SELECT dolt_checkout('pt_reset');
SELECT dolt_reset('--hard', 'main');
SELECT * FROM dolt_status;
SELECT * FROM pg_temp.show();
INSERT INTO pt.p (id, region) VALUES (4, 'east');	-- (the trigger, cloned)
SELECT v FROM pt.p WHERE id = 4;
SELECT dolt_reset('--hard', 'HEAD');

-- a row changed in a partition: listed by the partition, merged
SELECT dolt_checkout('pt_merge');
SELECT dolt_merge('main') IS NOT NULL AS merged;
UPDATE pt.p SET v = 'changed' WHERE id = 150;
SELECT * FROM dolt_status;
SELECT diff_type, (from_row).v, (to_row).v FROM dolt_diff('HEAD', 'WORKING', NULL::pt.p_west_hi);
SELECT length(dolt_commit('-am', 'changed')::text) AS committed;
SELECT dolt_checkout('main');
SELECT dolt_merge('pt_merge') IS NOT NULL AS merged;
SELECT v FROM pt.p WHERE id = 150;

-- a partition added, one dropped, one bound changed: each moved
CREATE TABLE pt.p_south PARTITION OF pt.p FOR VALUES IN ('south');
INSERT INTO pt.p VALUES (5, 'south', 'd');
DROP TABLE pt.p_rest;
ALTER TABLE pt.p DETACH PARTITION pt.p_east;
ALTER TABLE pt.p ATTACH PARTITION pt.p_east FOR VALUES IN ('east', 'far east');
INSERT INTO pt.p VALUES (6, 'far east', 'e');
SELECT * FROM dolt_status;
SELECT length(dolt_commit('-Am', 'repartitioned')::text) AS committed;
SELECT dolt_checkout('pt_reset');
SELECT dolt_reset('--hard', 'main');
SELECT * FROM dolt_status;
SELECT * FROM pg_temp.show();

-- and back
SELECT dolt_reset('--hard', 'HEAD~1');
SELECT * FROM dolt_status;
SELECT * FROM pg_temp.show();

SELECT dolt_checkout('main');
SELECT dolt_branch('-D', 'pt_reset');
SELECT dolt_branch('-D', 'pt_merge');
DROP SCHEMA pt CASCADE;
SELECT length(dolt_commit('-Am', 'pt gone')::text) AS committed;
