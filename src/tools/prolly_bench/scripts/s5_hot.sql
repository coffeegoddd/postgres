-- S5: many clients updating a few hot rows.  Defines: hot_rows.
\set id random(1, :hot_rows)
UPDATE s5_hot SET v = v + 1 WHERE id = :id;
