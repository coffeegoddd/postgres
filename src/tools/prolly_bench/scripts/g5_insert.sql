-- G5: a vehicle joins the fleet (ids past the initial n).
-- Defines: n, extent, srid.
\set id random(:n + 1, 10 * :n)
\set x random(0, :extent)
\set y random(0, :extent)
INSERT INTO gfleet VALUES (:id, ST_SetSRID(ST_MakePoint(:x, :y), :srid)) ON CONFLICT (id) DO NOTHING;
