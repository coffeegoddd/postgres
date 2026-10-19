-- A float NaN has one canonical bit pattern, the positive quiet NaN,
-- whatever bits the value holds: 'NaN' and 'inf' - 'inf' (on most
-- platforms the negative quiet NaN) are the same value to every operator,
-- so they are the same bytes, and the same rows give the same trees.  So
-- in floats' send formats wherever they're encoded: plain values, arrays,
-- composites, ranges and multiranges over floats, and geometric types.
-- Signed zeros stay apart: -0 prints as "-0", and is read back so.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;

-- The bits each NaN holds, and its canonical bytes: always the positive
-- quiet NaN, as 'NaN' itself sends.
SELECT encode(float8send(f), 'hex') <> encode(float8send('NaN'), 'hex') AS f8_bits_differ,
	   encode(prolly_canonical_value(f), 'hex') AS f8_canon,
	   encode(prolly_canonical_value('NaN'::float8), 'hex') AS f8_nan_canon,
	   prolly_canonical_round_trip(f)::text AS f8_round_trip
FROM (SELECT 'inf'::float8 - 'inf'::float8 AS f) s;
SELECT encode(float4send(f), 'hex') <> encode(float4send('NaN'), 'hex') AS f4_bits_differ,
	   encode(prolly_canonical_value(f), 'hex') AS f4_canon,
	   encode(prolly_canonical_value('NaN'::float4), 'hex') AS f4_nan_canon,
	   prolly_canonical_round_trip(f)::text AS f4_round_trip
FROM (SELECT 'inf'::float4 - 'inf'::float4 AS f) s;

-- Every other float is its send format, signed zeros and infinities kept.
SELECT f, prolly_canonical_value(f) = float8send(f) AS f8_as_sent,
	   prolly_canonical_value(f::float4) = float4send(f::float4) AS f4_as_sent,
	   prolly_canonical_round_trip(f)::text AS round_trip
FROM (VALUES ('0'::float8), ('-0'), ('1.5'), ('-1e30'), ('Infinity'),
			 ('-Infinity'), ('NaN')) v(f);

-- The same for NaNs inside other values.
CREATE TYPE fnan AS (a float8, b float4);
CREATE TYPE fspan AS RANGE (subtype = float8);
CREATE TYPE f4span AS RANGE (subtype = float4);
CREATE DOMAIN dfloat AS float8;
CREATE TYPE dspan AS RANGE (subtype = dfloat);
\set nan8 '(\'inf\'::float8 - \'inf\'::float8)'
\set nan4 '(\'inf\'::float4 - \'inf\'::float4)'
SELECT prolly_canonical_value(ARRAY[:nan8, 1]) =
	   prolly_canonical_value(ARRAY['NaN'::float8, 1]) AS array_same,
	   prolly_canonical_value(ROW(:nan8, :nan4)::fnan) =
	   prolly_canonical_value(ROW('NaN', 'NaN')::fnan) AS composite_same,
	   prolly_canonical_value(:nan8::dfloat) =
	   prolly_canonical_value('NaN'::dfloat) AS domain_same,
	   prolly_canonical_value(fspan(1, :nan8)) =
	   prolly_canonical_value(fspan(1, 'NaN')) AS range_same,
	   prolly_canonical_value(f4span(:nan4, :nan4, '[]')) =
	   prolly_canonical_value(f4span('NaN', 'NaN', '[]')) AS f4range_same,
	   prolly_canonical_value(dspan(1, :nan8)) =
	   prolly_canonical_value(dspan(1, 'NaN')) AS domain_range_same,
	   prolly_canonical_value(fspan_multirange(fspan(0, 1), fspan(2, :nan8))) =
	   prolly_canonical_value(fspan_multirange(fspan(0, 1), fspan(2, 'NaN'))) AS multirange_same;
SELECT prolly_canonical_value(point(:nan8, 1)) =
	   prolly_canonical_value(point('NaN', 1)) AS point_same,
	   prolly_canonical_value(lseg(point(:nan8, 1), point(2, :nan8))) =
	   prolly_canonical_value(lseg(point('NaN', 1), point(2, 'NaN'))) AS lseg_same,
	   prolly_canonical_value(box(point(:nan8, 1), point(2, 3))) =
	   prolly_canonical_value(box(point('NaN', 1), point(2, 3))) AS box_same,
	   prolly_canonical_value(circle(point(1, :nan8), 2)) =
	   prolly_canonical_value(circle(point(1, 'NaN'), 2)) AS circle_same,
	   prolly_canonical_value(line(point(0, 0), point(1, :nan8))) =
	   prolly_canonical_value(line(point(0, 0), point(1, 'NaN'))) AS line_same,
	   prolly_canonical_value(path(polygon(box(point(:nan8, 1), point(2, 3))))) =
	   prolly_canonical_value(path(polygon(box(point('NaN', 1), point(2, 3))))) AS path_same,
	   prolly_canonical_value(polygon(box(point(:nan8, 1), point(2, 3)))) =
	   prolly_canonical_value(polygon(box(point('NaN', 1), point(2, 3)))) AS polygon_same;
-- ... and they read back as they were.
SELECT prolly_canonical_round_trip(fspan_multirange(fspan(0, 1), fspan(2, :nan8))) AS multirange,
	   prolly_canonical_round_trip(polygon(box(point(:nan8, 1), point(2, 3)))) AS polygon,
	   prolly_canonical_round_trip(fspan(:nan8, :nan8, '[]')) AS range;

-- Tables: branches that set a value to each NaN have the same root, and
-- merge cleanly, as keys and values, keyed and keyless.
CREATE SCHEMA fl VERSIONED;
CREATE TABLE fl.t (id int PRIMARY KEY, f float8, f4 float4, p point, s fspan);
CREATE TABLE fl.k (f float8 PRIMARY KEY, v int);
CREATE TABLE fl.kl (f float8, v int);
INSERT INTO fl.t VALUES (1, 0, 0, point(0, 0), fspan(0, 1));
INSERT INTO fl.k VALUES (0, 1);
INSERT INTO fl.kl VALUES (0, 1);
SELECT length(dolt_commit('-Am', 'base')::text) AS committed;
SELECT dolt_branch('fl_other');
UPDATE fl.t SET f = 'NaN', f4 = 'NaN', p = point('NaN', 0), s = fspan(0, 'NaN');
UPDATE fl.k SET f = 'NaN';
UPDATE fl.kl SET f = 'NaN';
SELECT length(dolt_commit('-Am', 'NaN')::text) AS committed;
SELECT dolt_checkout('fl_other');
UPDATE fl.t SET f = :nan8, f4 = :nan4, p = point(:nan8, 0), s = fspan(0, :nan8);
UPDATE fl.k SET f = :nan8;
UPDATE fl.kl SET f = :nan8;
SELECT length(dolt_commit('-Am', 'inf - inf')::text) AS committed;
SELECT prolly_root_hash('main') = prolly_root_hash('fl_other') AS same_root;
SELECT (dolt_merge('main'))[2:4];
SELECT * FROM dolt_status;
SELECT * FROM fl.t;
SELECT * FROM fl.k;
SELECT * FROM fl.kl;
SELECT dolt_checkout('main');
