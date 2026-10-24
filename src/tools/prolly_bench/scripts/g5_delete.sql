-- G5: a vehicle that joined leaves.  Defines: n.
\set id random(:n + 1, 10 * :n)
DELETE FROM gfleet WHERE id = :id;
