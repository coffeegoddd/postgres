-- S7: update indexed columns of one row.  Defines: rows.
\set id random(1, :rows)
UPDATE s7 SET a = a + 1, b = (b + 7) % 100000, p = point(random() * 1000, random() * 1000) WHERE id = :id;
