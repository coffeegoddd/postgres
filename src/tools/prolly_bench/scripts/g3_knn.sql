-- G3: the k nearest points to a random point.  Defines: extent, k, srid.
\set x random(0, :extent)
\set y random(0, :extent)
SELECT id FROM gpts ORDER BY g <-> ST_SetSRID(ST_MakePoint(:x, :y), :srid) LIMIT :k;
