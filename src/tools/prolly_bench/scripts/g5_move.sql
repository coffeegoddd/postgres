-- G5: a random vehicle of the fleet moves.  Defines: n, extent, srid.
\set id random(1, :n)
\set dx random(-50, 50)
\set dy random(-50, 50)
UPDATE gfleet SET g = ST_SetSRID(ST_MakePoint(
    least(greatest(ST_X(g) + :dx, 0), :extent), least(greatest(ST_Y(g) + :dy, 0), :extent)), :srid)
WHERE id = :id;
