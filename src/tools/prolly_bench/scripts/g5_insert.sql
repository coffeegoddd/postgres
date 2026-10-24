-- G5: a vehicle joins the fleet, with a new id.  Defines: extent, srid.
\set x random(0, :extent)
\set y random(0, :extent)
INSERT INTO gfleet VALUES (nextval('gfleet_new'), ST_SetSRID(ST_MakePoint(:x, :y), :srid));
