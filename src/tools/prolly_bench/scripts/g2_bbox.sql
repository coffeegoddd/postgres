-- G2: a random window over the buildings, by bounding box (&&).
-- Defines: extent, w, srid.
\set x random(0, :extent - :w)
\set y random(0, :extent - :w)
\set x2 :x + :w
\set y2 :y + :w
SELECT count(*) FROM gbld WHERE g && ST_MakeEnvelope(:x, :y, :x2, :y2, :srid);
