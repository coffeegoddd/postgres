-- S7: insert a new row, touching every index.  Defines: rows.
\set id random(:rows + 1, 1000000000000)
\set a :id % 1000
\set b :id % 100000
\set c :id % 7919
\set px :id % 1000
\set py :id % 997
INSERT INTO s7 VALUES (:id, :a, :b, :c, md5(:id::text), point(:px, :py)) ON CONFLICT (id) DO NOTHING;
