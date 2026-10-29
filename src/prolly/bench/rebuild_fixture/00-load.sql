-- The first commit: one schema holding every kind of object Postgres
-- rebuilds from rows plus definitions.
CREATE SCHEMA rt;
SET search_path = rt, public;

CREATE TYPE mood AS ENUM ('sad', 'ok', 'happy');
CREATE TYPE addr AS (street text, zip int4);
CREATE DOMAIN posint AS int4 CHECK (VALUE > 0);
CREATE COLLATION ci (provider = icu, locale = 'und-u-ks-level2', deterministic = false);
CREATE FUNCTION norm(text) RETURNS text LANGUAGE sql IMMUTABLE PARALLEL SAFE
	RETURN lower(btrim($1));

-- Every column type, a composite text/int key, NULLs, edge values, and
-- values large enough to be TOASTed.
CREATE TABLE kinds (
	a int4, b text COLLATE "C",
	i2 int2, i8 int8, n numeric(20,6), nn numeric, f4 float4, f8 float8,
	bo bool, t text, vc varchar(40), ch char(5), by bytea,
	d date, tm time, tz timetz, ts timestamp, tstz timestamptz, iv interval,
	u uuid, ip inet, cid cidr, mac macaddr, mac8 macaddr8,
	j json, jb jsonb, arr int4[], tarr text[], m mood, ad addr, dom posint,
	r int4range, tsr tstzrange, mr int4multirange,
	pt point, bx box, pg polygon, ci circle, ls lseg, pth path, ln line,
	tv tsvector, tq tsquery, bt bit(8), vb varbit, mo money, o oid, lsn pg_lsn,
	hs hstore, cu cube, big text,
	PRIMARY KEY (a, b)
);
ALTER TABLE kinds ALTER COLUMN big SET STORAGE EXTERNAL;
ALTER TABLE kinds ALTER COLUMN t SET STATISTICS 500;

INSERT INTO kinds
SELECT g % 1000, 'b' || (g / 1000),
	CASE WHEN g % 17 = 0 THEN NULL ELSE (g % 30000 - 15000)::int2 END,
	g * 13,
	g * 1.000001,
	CASE g % 50 WHEN 0 THEN 'NaN' WHEN 1 THEN 'Infinity' WHEN 2 THEN '-Infinity'
		ELSE g::numeric / 7 END,
	CASE g % 40 WHEN 0 THEN 'NaN' WHEN 1 THEN '-0' WHEN 2 THEN 'Infinity'
		ELSE g / 3.0 END::float4,
	CASE g % 40 WHEN 0 THEN '-Infinity' WHEN 1 THEN '-0' ELSE g * 1e-300 END::float8,
	CASE g % 3 WHEN 0 THEN NULL ELSE g % 2 = 0 END,
	CASE g % 10 WHEN 0 THEN '' WHEN 1 THEN E'tab\there' WHEN 2 THEN E'new\nline'
		WHEN 3 THEN E'back\\slash' WHEN 4 THEN 'ünïcødé ✓ 日本 ' || g WHEN 5 THEN '\N'
		WHEN 6 THEN NULL ELSE '  Word ' || g END,
	'v' || md5(g::text),
	left(md5(g::text), 3),
	CASE g % 5 WHEN 0 THEN '\x'::bytea WHEN 1 THEN NULL
		ELSE decode(md5(g::text), 'hex') END,
	CASE g % 100 WHEN 0 THEN 'infinity' WHEN 1 THEN '-infinity'
		WHEN 2 THEN '0044-03-15 BC' ELSE date '2000-01-01' + g END,
	time '00:00' + g * interval '1.234567 s',
	timetz '10:00:00+05:30' + g * interval '1 s',
	CASE g % 100 WHEN 0 THEN 'infinity' ELSE timestamp '2020-01-01' + g * interval '1 hour 0.5 s' END,
	timestamptz '2020-01-01 00:00+00' + g * interval '37 min',
	CASE g % 101 WHEN 0 THEN 'infinity'::interval
		ELSE make_interval(months => g % 13, days => g % 31, secs => g * 1.5) END,
	md5(g::text)::uuid,
	CASE WHEN g % 7 = 0 THEN ('2001:db8::' || to_hex(g))::inet
		ELSE ('10.' || (g / 65536) % 256 || '.' || (g / 256) % 256 || '.' || g % 256
			  || CASE WHEN g % 3 = 0 THEN '/24' ELSE '' END)::inet END,
	NULL, NULL, NULL,
	format('{"g": %s,  "dup": 1, "dup": 2}', g)::json,
	jsonb_build_object('g', g, 'tags', jsonb_build_array('t' || g % 10, 't' || g % 7),
					   'n', CASE WHEN g % 9 = 0 THEN NULL END, 'f', g / 7.0),
	CASE g % 11 WHEN 0 THEN '{}' WHEN 1 THEN '{1,NULL,3}' WHEN 2 THEN '[0:2]={7,8,9}'
		WHEN 3 THEN '{{1,2},{3,4}}' ELSE ARRAY[g % 100, g % 37, g % 13]::text END::int4[],
	ARRAY['a' || g % 5, NULL, E'q"uo,te', '{brace}', '', E'x\\y'],
	(ARRAY['sad', 'ok', 'happy'])[1 + g % 3]::mood,
	ROW('street ' || g, g % 99999)::addr,
	1 + g % 500,
	CASE g % 20 WHEN 0 THEN 'empty'::int4range WHEN 1 THEN '(,)'::int4range
		ELSE int4range(g % 1000, g % 1000 + 1 + g % 50) END,
	tstzrange(timestamptz '2020-01-01 00:00+00' + g * interval '37 min',
			  timestamptz '2020-01-01 03:00+00' + g * interval '37 min'),
	int4multirange(int4range(g % 100, g % 100 + 5), int4range(g % 100 + 10, g % 100 + 20)),
	point(g % 1000, g / 1000.0),
	box(point(g % 1000, g % 500), point(g % 1000 + 3, g % 500 + 2)),
	polygon(6, circle(point(g % 100, g % 50), 2)),
	circle(point(g % 300, g % 200), 1 + g % 5),
	lseg(point(g, 0), point(0, g)),
	format('[(0,0),(1,%s),(2,1)]', g)::path,
	line(point(0, 0), point(1, g % 10 + 1)),
	to_tsvector('english', 'the quick brown fox '
				|| (ARRAY['jumps', 'sleeps', 'runs'])[1 + g % 3] || ' over ' || g),
	to_tsquery('english', 'fox & ' || (ARRAY['jump', 'sleep', 'run'])[1 + g % 3]),
	(g % 256)::bit(8),
	(g::bit(20))::varbit,
	(g * 1.23)::numeric::money,
	g::oid,
	('0/' || to_hex(g * 1000))::pg_lsn,
	hstore(ARRAY['k' || g % 5, 'v'], ARRAY[g::text, NULL]),
	cube(ARRAY[g % 100, g % 50]::float8[]),
	CASE g % 2000 WHEN 0 THEN repeat('compressible ', 20000)
		WHEN 1 THEN (SELECT string_agg(md5(g::text || s::text), '') FROM generate_series(1, 4000) s)
		END
FROM generate_series(1, 20000) g;
UPDATE kinds SET cid = network(set_masklen(ip, CASE family(ip) WHEN 4 THEN 16 ELSE 64 END)),
	mac = ('08:00:2b:' || lpad(to_hex((a / 256) % 256), 2, '0') || ':'
		   || lpad(to_hex(a % 256), 2, '0') || ':' || lpad(to_hex(length(b)), 2, '0'))::macaddr;
UPDATE kinds SET mac8 = mac::macaddr8;
-- A composite value whose fields are all NULL is not itself NULL.
INSERT INTO kinds (a, b, ad) VALUES (9001, 'nullrow', ROW(NULL, NULL));

-- B-tree: composite, ordered, covering, partial, expression, operator class.
CREATE INDEX kinds_tstz_desc ON kinds (tstz DESC NULLS FIRST, i8);
CREATE UNIQUE INDEX kinds_vc_incl ON kinds (vc) INCLUDE (n) WITH (fillfactor = 70);
CREATE INDEX kinds_norm_part ON kinds (norm(t)) WHERE t IS NOT NULL AND t <> '';
CREATE INDEX kinds_t_pattern ON kinds (t text_pattern_ops);
CREATE INDEX kinds_zip ON kinds (((ad).zip)) WITH (deduplicate_items = off);
CREATE INDEX kinds_mixed ON kinds (m, i2 DESC, b COLLATE "C");
-- Hash.
CREATE INDEX kinds_u_hash ON kinds USING hash (u);
CREATE INDEX kinds_ip_hash ON kinds USING hash (ip);
-- GIN.
CREATE INDEX kinds_jb_gin ON kinds USING gin (jb);
CREATE INDEX kinds_jb_path ON kinds USING gin (jb jsonb_path_ops);
CREATE INDEX kinds_arr_gin ON kinds USING gin (arr);
CREATE INDEX kinds_tv_gin ON kinds USING gin (tv);
CREATE INDEX kinds_t_trgm ON kinds USING gin (t gin_trgm_ops);
CREATE INDEX kinds_hs_gin ON kinds USING gin (hs);
CREATE INDEX kinds_btgin ON kinds USING gin (i8, m);
-- GiST.
CREATE INDEX kinds_pt_gist ON kinds USING gist (pt);
CREATE INDEX kinds_bx_gist ON kinds USING gist (bx);
CREATE INDEX kinds_pg_gist ON kinds USING gist (pg);
CREATE INDEX kinds_ci_gist ON kinds USING gist (ci);
CREATE INDEX kinds_r_gist ON kinds USING gist (r);
CREATE INDEX kinds_mr_gist ON kinds USING gist (mr);
CREATE INDEX kinds_tv_gist ON kinds USING gist (tv);
CREATE INDEX kinds_vc_trgm ON kinds USING gist (vc gist_trgm_ops);
CREATE INDEX kinds_cu_gist ON kinds USING gist (cu);
CREATE INDEX kinds_ip_gist ON kinds USING gist (ip inet_ops);
CREATE INDEX kinds_btgist ON kinds USING gist (i8, tsr);
-- SP-GiST.
CREATE INDEX kinds_pt_quad ON kinds USING spgist (pt);
CREATE INDEX kinds_pt_kd ON kinds USING spgist (pt kd_point_ops);
CREATE INDEX kinds_t_radix ON kinds USING spgist (t);
CREATE INDEX kinds_ip_spgist ON kinds USING spgist (ip);
CREATE INDEX kinds_r_spgist ON kinds USING spgist (r);
CREATE INDEX kinds_bx_spgist ON kinds USING spgist (bx);
-- BRIN: minmax, minmax-multi, bloom, inclusion.
CREATE INDEX kinds_ts_brin ON kinds USING brin (ts) WITH (pages_per_range = 4);
CREATE INDEX kinds_i8_multi ON kinds USING brin (i8 int8_minmax_multi_ops);
CREATE INDEX kinds_u_bloom ON kinds USING brin (u uuid_bloom_ops);
CREATE INDEX kinds_ip_incl ON kinds USING brin (ip inet_inclusion_ops, bx box_inclusion_ops);
-- The bloom access method.
CREATE INDEX kinds_bloom ON kinds USING bloom (a, t) WITH (length = 80, col1 = 2, col2 = 4);

-- No primary key, duplicate rows, rows of NULLs.
CREATE TABLE nokey (x int, y text);
INSERT INTO nokey SELECT g % 50, CASE WHEN g % 7 = 0 THEN NULL ELSE 'y' || g % 30 END
FROM generate_series(1, 3000) g;
INSERT INTO nokey VALUES (NULL, NULL), (NULL, NULL), (NULL, NULL);
CREATE INDEX nokey_x ON nokey (x NULLS FIRST);
ALTER TABLE nokey REPLICA IDENTITY FULL;

-- Identity, generated (stored and virtual), defaults, checks, FKs.
CREATE TABLE acct (
	id int8 GENERATED ALWAYS AS IDENTITY (START WITH 100 INCREMENT BY 3) PRIMARY KEY,
	email text NOT NULL,
	region text,
	created timestamptz NOT NULL DEFAULT timestamptz '2025-01-01 00:00+00',
	score int CHECK (score BETWEEN 0 AND 100),
	email_lower text GENERATED ALWAYS AS (lower(email)) STORED,
	label text GENERATED ALWAYS AS (coalesce(region, '?') || ':' || score) VIRTUAL
);
INSERT INTO acct (email, region, score)
SELECT 'User' || g || '@Example.com', CASE WHEN g % 4 = 0 THEN NULL ELSE 'r' || g % 6 END, g % 101
FROM generate_series(1, 2000) g;
CREATE UNIQUE INDEX acct_email_lower ON acct (lower(email));
CREATE INDEX acct_stored ON acct (email_lower) WHERE region IS NULL;
ALTER TABLE acct CLUSTER ON acct_pkey;
CREATE STATISTICS acct_stats (dependencies, ndistinct) ON region, score FROM acct;
COMMENT ON TABLE acct IS 'accounts';
COMMENT ON COLUMN acct.email IS 'login';
ALTER TABLE acct ENABLE ROW LEVEL SECURITY;
CREATE POLICY acct_own ON acct USING (region IS NOT NULL);

CREATE TABLE ledger (
	acct_id int8 REFERENCES acct ON DELETE CASCADE,
	seq int,
	amount numeric(12,2) NOT NULL,
	PRIMARY KEY (acct_id, seq)
);
INSERT INTO ledger SELECT 100 + 3 * (g % 2000), g / 2000, (g % 997) * 1.25
FROM generate_series(0, 9999) g;
CREATE TABLE ledger_note (
	acct_id int8, seq int, note text,
	FOREIGN KEY (acct_id, seq) REFERENCES ledger MATCH FULL ON UPDATE CASCADE ON DELETE CASCADE
);
INSERT INTO ledger_note SELECT acct_id, seq, 'n' || seq FROM ledger WHERE seq = 1;

-- Self reference, deferred.
CREATE TABLE node (
	id int PRIMARY KEY,
	parent int REFERENCES node DEFERRABLE INITIALLY DEFERRED,
	path text
);
INSERT INTO node SELECT g, CASE WHEN g = 1 THEN NULL ELSE g / 2 END, 'p' || g
FROM generate_series(1, 4000) g;

-- Exclusion constraints.
CREATE TABLE booking (
	room int, during tstzrange,
	EXCLUDE USING gist (room WITH =, during WITH &&)
);
INSERT INTO booking SELECT g % 20, tstzrange(timestamptz '2025-01-01 00:00+00' + (g / 20) * interval '1 hour',
	timestamptz '2025-01-01 00:00+00' + (g / 20 + 1) * interval '1 hour')
FROM generate_series(0, 1999) g;
CREATE TABLE zone (c circle, EXCLUDE USING gist (c WITH &&));
INSERT INTO zone SELECT circle(point(g * 10, 0), 4) FROM generate_series(1, 500) g;

-- Case-insensitive uniqueness, NULLS NOT DISTINCT, deferrable unique.
CREATE TABLE tag (name text COLLATE ci, scope text, UNIQUE NULLS NOT DISTINCT (name, scope));
INSERT INTO tag VALUES ('x', NULL), ('Y', 'a'), ('y', NULL);
CREATE TABLE slot (pos int UNIQUE DEFERRABLE INITIALLY DEFERRED, label text);
INSERT INTO slot SELECT g, 's' || g FROM generate_series(1, 500) g;

-- Declarative partitions, two levels, with a default partition.
CREATE TABLE events (id int8, at date, kind text, payload jsonb, PRIMARY KEY (id, at))
	PARTITION BY RANGE (at);
CREATE TABLE events_2024 PARTITION OF events FOR VALUES FROM ('2024-01-01') TO ('2025-01-01');
CREATE TABLE events_2025 PARTITION OF events FOR VALUES FROM ('2025-01-01') TO ('2026-01-01')
	PARTITION BY HASH (id);
CREATE TABLE events_2025_h0 PARTITION OF events_2025 FOR VALUES WITH (MODULUS 2, REMAINDER 0);
CREATE TABLE events_2025_h1 PARTITION OF events_2025 FOR VALUES WITH (MODULUS 2, REMAINDER 1);
CREATE TABLE events_other PARTITION OF events DEFAULT;
INSERT INTO events SELECT g, date '2023-07-01' + (g % 900), 'k' || g % 4,
	jsonb_build_object('g', g, 'odd', g % 2 = 1)
FROM generate_series(1, 9000) g;
CREATE INDEX events_kind_at ON events (kind, at);
CREATE INDEX events_payload ON events USING gin (payload);
CREATE INDEX events_at_brin ON events USING brin (at);

-- Inheritance.
CREATE TABLE base_item (id int, name text);
CREATE TABLE special_item (extra int) INHERITS (base_item);
INSERT INTO base_item SELECT g, 'base' || g FROM generate_series(1, 300) g;
INSERT INTO special_item SELECT g, 'sp' || g, g * 2 FROM generate_series(301, 600) g;
CREATE INDEX special_item_name ON special_item USING hash (name);

-- Sequences: serial, standalone cycling, never called.
CREATE TABLE ticket (id serial PRIMARY KEY, note text);
INSERT INTO ticket (note) SELECT 't' || g FROM generate_series(1, 77) g;
CREATE SEQUENCE seq_odd INCREMENT 2 START 1 MAXVALUE 99 CYCLE;
SELECT nextval('seq_odd') FROM generate_series(1, 60);
CREATE SEQUENCE seq_fresh START 5000;

CREATE UNLOGGED TABLE scratch (k int PRIMARY KEY, v text);
INSERT INTO scratch SELECT g, 'v' || g FROM generate_series(1, 100) g;

-- Derived objects: views, materialized views, a trigger.
CREATE VIEW acct_view AS SELECT id, email, region FROM acct WHERE score > 50;
CREATE MATERIALIZED VIEW kinds_summary AS
	SELECT m, count(*) AS n, sum(i8) AS s FROM kinds GROUP BY m;
CREATE UNIQUE INDEX kinds_summary_m ON kinds_summary (m);
CREATE MATERIALIZED VIEW empty_mv AS SELECT 1 AS one WITH NO DATA;
CREATE FUNCTION ledger_touch() RETURNS trigger LANGUAGE plpgsql AS
	$$BEGIN NEW.amount := round(NEW.amount, 2); RETURN NEW; END$$;
CREATE TRIGGER ledger_touch BEFORE INSERT OR UPDATE ON ledger
	FOR EACH ROW EXECUTE FUNCTION ledger_touch();
