-- S7: lookup through a secondary index.
\set c random(0, 7918)
SELECT count(*) FROM s7 WHERE c = :c;
