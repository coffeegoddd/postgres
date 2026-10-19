-- S7: update indexed columns of one row.  Defines: rows.
-- (the new point from pgbench's seeded random, not the server's, so a run
-- with a seed does the same work every time; in thousandths, as fine as
-- the server's random() * 1000 was for the GiST index)
\set id random(1, :rows)
\set px random(0, 999999) / 1000.0
\set py random(0, 999999) / 1000.0
UPDATE s7 SET a = a + 1, b = (b + 7) % 100000, p = point(:px, :py) WHERE id = :id;
