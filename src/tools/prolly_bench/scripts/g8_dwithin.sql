-- G8: geography points within radius metres of a random point.
-- Defines: radius.
\set lon random(-179, 179)
\set lat random(-84, 84)
SELECT count(*) FROM ggeog WHERE ST_DWithin(g, ST_MakePoint(:lon, :lat)::geography, :radius);
