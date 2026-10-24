-- G5: a vehicle leaves the fleet: the first at or past a random id of the
-- initial fleet's that no other client is taking (there is always one:
-- inserts add as many new ids as deletes take).  Defines: n.
\set id random(1, :n)
DELETE FROM gfleet WHERE id = (SELECT id FROM gfleet WHERE id >= :id ORDER BY id LIMIT 1
                               FOR UPDATE SKIP LOCKED);
