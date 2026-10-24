// Package enginediff compares reproducible transaction histories with heap.
package enginediff

import (
	"fmt"
	"math/rand"
	"strings"

	"prolly_bench/slt/internal/geomgen"
)

type Group struct {
	Name string   `json:"name"`
	SQL  []string `json:"sql"`
}

type Program struct {
	Version int               `json:"version"`
	Seed    int64             `json:"seed"`
	Setup   []string          `json:"setup"`
	Groups  []Group           `json:"groups"`
	Errors  map[string]string `json:"expected_errors,omitempty"`
	// PostGIS is whether the program has the spatial table geo (it creates
	// the postgis extension in its setup).
	PostGIS bool `json:"postgis,omitempty"`
}

// Options are what a generated program covers beyond the core families.
type Options struct {
	// PostGIS adds a geometry table with a GiST index, spatial writes and
	// spatial reads (window, intersection, distance and nearest-neighbour
	// queries), and now and then a multi-megabyte polygon.
	PostGIS bool
}

func Generate(seed int64, steps int) Program { return GenerateWith(seed, steps, Options{}) }

// GenerateWith is Generate with options.  Without any it makes the same
// program as Generate for the same seed.
func GenerateWith(seed int64, steps int, o Options) Program {
	r := rand.New(rand.NewSource(seed))
	p := Program{Version: 1, Seed: seed, Errors: map[string]string{
		"INSERT INTO t VALUES (-1, -100, NULL, NULL, NULL)": "23514",
		"INSERT INTO child VALUES (-1, -999)":               "23503",
		"SELECT count(*) FROM t":                            "25P02",
	}, Setup: []string{
		"CREATE TABLE t (id int PRIMARY KEY, v int CHECK (v >= -50), n numeric, txt text, b bytea)",
		fmt.Sprintf(`INSERT INTO t SELECT g, g %% 17, 9007199254740993::numeric + g,
CASE WHEN g %% 31 = 0 THEN (SELECT string_agg(md5(g::text || ':' || k::text), '') FROM generate_series(1, 100) k)
ELSE md5(g::text) END, decode(lpad(to_hex(g), 8, '0'), 'hex') FROM generate_series(1, %d) g`, 128+r.Intn(513)),
		"CREATE INDEX t_v ON t (v, id)",
		"CREATE TABLE child (id int PRIMARY KEY, parent int REFERENCES t ON DELETE CASCADE)",
		"INSERT INTO child SELECT g, g FROM generate_series(1, 10) g",
		"CREATE TABLE audit (id int, v int)",
	}}
	// (its own source, so the core families' seeds stay as they were)
	var geo *rand.Rand
	if o.PostGIS {
		geo = rand.New(rand.NewSource(seed ^ 0x67656f))
		p.PostGIS = true
		p.Setup = append(p.Setup, geoSetup(geo)...)
	}
	for i := 0; i < steps; i++ {
		id, v := 1+r.Intn(450), r.Intn(50)
		update := fmt.Sprintf("UPDATE t SET v = %d, txt = coalesce(txt, '') || 'x' WHERE id BETWEEN %d AND %d", v, id, id+7)
		insert := fmt.Sprintf("INSERT INTO t VALUES (%d, %d, 9223372036854775807, 'seed-%d', '\\x00ff') ON CONFLICT (id) DO UPDATE SET v = excluded.v", id, v, seed)
		families := []Group{
			{"upsert", []string{"WITH changed AS (" + insert + " RETURNING id, v) SELECT * FROM changed ORDER BY id"}},
			{"update", []string{"WITH changed AS (" + update + " RETURNING id, v) SELECT * FROM changed ORDER BY id"}},
			{"delete", []string{fmt.Sprintf("WITH changed AS (DELETE FROM t WHERE id BETWEEN %d AND %d RETURNING id) SELECT * FROM changed ORDER BY id", id, id+3)}},
			{"savepoints", []string{"BEGIN", insert, "SAVEPOINT outer_sp", update, "SAVEPOINT inner_sp",
				"INSERT INTO t VALUES (-1, -100, NULL, NULL, NULL)", "ROLLBACK TO inner_sp", "RELEASE inner_sp",
				"ROLLBACK TO outer_sp", "COMMIT"}},
			{"abort", []string{"BEGIN", update, "INSERT INTO child VALUES (-1, -999)", "SELECT count(*) FROM t", "ROLLBACK"}},
			{"ddl", []string{"BEGIN", "ALTER TABLE t ADD COLUMN extra int DEFAULT 17", "SELECT count(*), sum(extra) FROM t",
				"ALTER TABLE t DROP COLUMN extra", "COMMIT"}},
			{"index", []string{"CREATE INDEX t_expr ON t ((v + 1)) WHERE v >= 0", "SET enable_seqscan = off",
				"SELECT id, v FROM t WHERE v >= 0 AND v + 1 BETWEEN 3 AND 9 ORDER BY id", "RESET enable_seqscan", "DROP INDEX t_expr"}},
			{"rewrite", []string{"ALTER TABLE t ALTER COLUMN v TYPE bigint", update, "ALTER TABLE t ALTER COLUMN v TYPE int"}},
			{"truncate_rollback", []string{"BEGIN", "TRUNCATE t CASCADE", "SELECT count(*) FROM child", "ROLLBACK"}},
			{"maintenance", []string{"VACUUM (ANALYZE) t", "REINDEX TABLE t"}},
			{"cursor", []string{"BEGIN", "DECLARE c CURSOR FOR SELECT id, v FROM t ORDER BY id", "FETCH 7 FROM c",
				update, "FETCH 11 FROM c", "CLOSE c", "COMMIT"}},
			{"trigger", []string{`CREATE FUNCTION log_change() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN INSERT INTO audit VALUES (NEW.id, NEW.v); RETURN NEW; END $$`,
				"CREATE TRIGGER log_change AFTER UPDATE ON t FOR EACH ROW EXECUTE FUNCTION log_change()", update,
				"DROP TRIGGER log_change ON t", "DROP FUNCTION log_change()"}},
			{"queries", []string{"SELECT v, count(*), sum(n) FROM t GROUP BY v ORDER BY v",
				"SELECT id, v, row_number() OVER (PARTITION BY v ORDER BY id) FROM t WHERE id < 80 ORDER BY id",
				"SELECT c.id, t.v FROM child c LEFT JOIN t ON t.id = c.parent ORDER BY c.id"}},
			{"nulls", []string{fmt.Sprintf("UPDATE t SET n = NULL, txt = NULL, b = '\\x' WHERE id = %d", id),
				"SELECT id, n, txt, b FROM t WHERE n IS NULL OR txt IS NULL ORDER BY id"}},
			{"prepared_reuse", []string{"SET plan_cache_mode = force_generic_plan",
				"PREPARE pbench_q(int) AS SELECT id, txt FROM t WHERE id >= $1 ORDER BY id LIMIT 9",
				"EXECUTE pbench_q(10)", update, "CREATE INDEX pbench_txt ON t ((left(txt, 20)))",
				"EXECUTE pbench_q(10)", "DROP INDEX pbench_txt", "EXECUTE pbench_q(10)",
				"DEALLOCATE pbench_q", "RESET plan_cache_mode"}},
			{"keyless_duplicates", []string{"CREATE TABLE duplicates (v int, txt text)",
				"INSERT INTO duplicates SELECT g % 3, CASE WHEN g % 2=0 THEN NULL ELSE 'NULL' END FROM generate_series(1, 100) g",
				"SELECT v, txt, count(*) FROM duplicates GROUP BY v, txt ORDER BY v, txt NULLS FIRST",
				"BEGIN", "UPDATE duplicates SET txt=E'a\\nb\\tc' WHERE v=1",
				"SELECT v, txt FROM duplicates ORDER BY v, txt NULLS FIRST", "ROLLBACK",
				"DELETE FROM duplicates WHERE v=2", "SELECT v, txt FROM duplicates ORDER BY v, txt NULLS FIRST",
				"DROP TABLE duplicates"}},
			{"typed_values", []string{"CREATE TABLE typed_values (id int, n numeric, f float8, v text, b bytea, d date, a int[])",
				`INSERT INTO typed_values VALUES
(1, 9007199254740993.00001, 'NaN', NULL, '\x00ff', '2000-02-29', ARRAY[1,NULL,3]),
(2, 9007199254740993.00002, 'Infinity', 'NULL', '\x', 'infinity', ARRAY[]::int[]),
(3, NULL, '-Infinity', E'a\nb\tc', NULL, NULL, NULL)`,
				"SELECT * FROM typed_values ORDER BY id", "BEGIN", "UPDATE typed_values SET n=n+0.00001, v='' WHERE id=1",
				"SELECT * FROM typed_values ORDER BY id", "ROLLBACK", "SELECT * FROM typed_values ORDER BY id",
				"DROP TABLE typed_values"}},
			{"many_tables", []string{"CREATE TABLE sibling0 (id int PRIMARY KEY, v text)",
				"CREATE TABLE sibling1 (id int PRIMARY KEY, v text)", "CREATE TABLE sibling2 (id int PRIMARY KEY, v text)",
				"INSERT INTO sibling0 VALUES (1, 'zero')", "INSERT INTO sibling1 VALUES (1, 'one')",
				"INSERT INTO sibling2 VALUES (1, 'two')",
				"SELECT a.id, a.v, b.v, c.v FROM sibling0 a JOIN sibling1 b USING(id) JOIN sibling2 c USING(id) ORDER BY a.id",
				"TRUNCATE sibling1", "SELECT count(*) FROM sibling0", "SELECT count(*) FROM sibling2",
				"DROP TABLE sibling0, sibling1, sibling2"}},
		}
		if o.PostGIS {
			families = append(families, geoFamilies(geo)...)
		}
		// Rotate the first full cycle so short runs vary without losing families.
		family := (i + int(uint64(seed)%uint64(len(families)))) % len(families)
		p.Groups = append(p.Groups, families[family])
	}
	return p
}

// geoRows is the spatial table's initial rows; ids above it are inserted.
const geoRows = 200

// largeN is the fewest vertices of a large polygon (about 1.6 MB; up to
// twice that).
const largeN = 100000

// geoValues are rows of geo: id, kind, g (Random's, SRID 0), s (one of
// Random's in one of several SRIDs), gg (a geography) and r (a value
// PostGIS's receive refuses, one row in three).
func geoValues(r *rand.Rand, first, n, largeEvery int) string {
	rows := make([]string, n)
	for i := range rows {
		g := geomgen.Random(r, largeEvery, largeN)
		srid, _ := geomgen.WithSRID(r, geomgen.Random(r, 0, 0))
		refused := "NULL"
		if r.Intn(3) == 0 {
			refused = geomgen.Refused(r).SQL
		}
		rows[i] = fmt.Sprintf("(%d, '%s', %s, %s, %s, %s)", first+i, g.Kind, g.SQL, srid.SQL, geomgen.Geography(r), refused)
	}
	return strings.Join(rows, ", ")
}

func geoSetup(r *rand.Rand) []string {
	return []string{
		"CREATE EXTENSION postgis",
		"CREATE TABLE geo (id int PRIMARY KEY, kind text, g geometry, s geometry, gg geography, r geometry)",
		"INSERT INTO geo VALUES " + geoValues(r, 1, geoRows, 0),
		// one large polygon from the start, so every run reads one
		fmt.Sprintf("INSERT INTO geo (id, kind, g) VALUES (0, 'large', %s)", geomgen.Large(quarterOf(r), quarterOf(r), largeN).SQL),
		"CREATE INDEX geo_g ON geo USING gist (g)",
		"CREATE INDEX geo_s ON geo USING gist (s)",
		"CREATE INDEX geo_gg ON geo USING gist (gg)",
		"CREATE INDEX geo_r ON geo USING gist (r)",
		"ANALYZE geo",
	}
}

func quarterOf(r *rand.Rand) float64 { return float64(r.Intn(geomgen.Extent*4)) / 4 }

// geoFamilies are the spatial families: writes (with a large polygon one
// insert in eight), reads with the index and without, nearest neighbours,
// a transaction with savepoints, and a large polygon's corner moved.
// GEOS and distance functions read g, s in its own SRID, and gg; r's
// refused values are read only by && and functions of their points, which
// take them (GEOS and distances raise errors on them).
func geoFamilies(r *rand.Rand) []Group {
	id := 1 + r.Intn(geoRows)
	w := geomgen.Window(r)
	pt := geomgen.Point(quarterOf(r), quarterOf(r)).SQL
	window := w.SQL()
	changed := func(sql string) string {
		return "WITH changed AS (" + sql + " RETURNING id, kind, md5(ST_AsEWKB(g)) AS g, md5(ST_AsEWKB(s)) AS s, " +
			"md5(ST_AsEWKB(gg::geometry)) AS gg, md5(ST_AsEWKB(r)) AS r) SELECT * FROM changed ORDER BY id"
	}
	gpt := fmt.Sprintf("'SRID=4326;POINT(%s %s)'::geography", num(float64(r.Intn(1400))/4-175), num(float64(r.Intn(600))/4-75))
	reads := []string{
		"SELECT id, kind FROM geo WHERE g && " + window + " ORDER BY id",
		"SELECT id, kind FROM geo WHERE ST_Intersects(g, " + window + ") ORDER BY id",
		"SELECT id FROM geo WHERE ST_Contains(" + window + ", g) ORDER BY id",
		fmt.Sprintf("SELECT id, round(ST_Distance(g, %s)::numeric, 6) FROM geo WHERE ST_DWithin(g, %s, %d) ORDER BY id", pt, pt, 10+r.Intn(90)),
		// (numeric sums: the same whatever order the rows are scanned in)
		"SELECT kind, count(*), sum(round(ST_Area(g)::numeric, 6)), sum(round(ST_Length(g)::numeric, 6)), sum(ST_NPoints(g)) FROM geo GROUP BY kind ORDER BY kind",
		"SELECT ST_AsText(ST_Extent(g)) FROM geo",
		"SELECT id, ST_SRID(s) FROM geo WHERE s && " + window + " ORDER BY id",
		"SELECT id FROM geo WHERE ST_Intersects(s, ST_SetSRID(" + window + ", ST_SRID(s))) ORDER BY id",
		"SELECT ST_SRID(s), count(*), sum(ST_NPoints(s)) FROM geo GROUP BY 1 ORDER BY 1",
		fmt.Sprintf("SELECT id, round(ST_Distance(gg, %s)::numeric, 3) FROM geo WHERE ST_DWithin(gg, %s, %d) ORDER BY id", gpt, gpt, 100000+r.Intn(900000)),
		"SELECT count(*), sum(round(ST_Area(gg)::numeric, 1)), sum(round(ST_Length(gg)::numeric, 3)) FROM geo",
		"SELECT id, ST_NPoints(r), ST_AsText(r) FROM geo WHERE r && " + window + " ORDER BY id",
		"SELECT count(r), sum(ST_NPoints(r)), ST_AsText(ST_Extent(r)) FROM geo",
	}
	knn := fmt.Sprintf("SELECT id, round((g <-> %s)::numeric, 6) AS d FROM geo WHERE NOT ST_IsEmpty(g) ORDER BY g <-> %s, id LIMIT %d",
		pt, pt, []int{1, 10, 100}[r.Intn(3)])
	return []Group{
		{"geo_writes", []string{
			changed(fmt.Sprintf("INSERT INTO geo VALUES %s ON CONFLICT (id) DO UPDATE SET kind = excluded.kind, g = excluded.g, "+
				"s = excluded.s, gg = excluded.gg, r = excluded.r", geoValues(r, geoRows+1+r.Intn(geoRows), 1+r.Intn(4), 8))),
			changed(fmt.Sprintf("UPDATE geo SET g = ST_Translate(g, %[1]d, %[2]d), s = ST_Translate(s, %[1]d, %[2]d), "+
				"r = ST_Translate(r, %[1]d, %[2]d) WHERE id BETWEEN %[3]d AND %[4]d", r.Intn(21)-10, r.Intn(21)-10, id, id+5)),
			changed(fmt.Sprintf("UPDATE geo SET r = %s, gg = %s WHERE id = %d", geomgen.Refused(r).SQL, geomgen.Geography(r), id+6)),
			changed("DELETE FROM geo WHERE g && " + geomgen.Window(r).SQL() + " AND kind <> 'large'"),
		}},
		{"geo_reads", reads},
		{"geo_index_reads", append(append(append([]string{"SET enable_seqscan = off"}, reads[:4]...), reads[6], reads[7], reads[9], reads[11]), knn, "RESET enable_seqscan")},
		{"geo_knn", []string{knn}},
		{"geo_savepoints", []string{"BEGIN",
			changed(fmt.Sprintf("UPDATE geo SET g = ST_Scale(g, 2, 2) WHERE id BETWEEN %d AND %d", id, id+3)),
			"SAVEPOINT sp", changed(fmt.Sprintf("DELETE FROM geo WHERE id BETWEEN %d AND %d", id, id+10)),
			"SELECT id FROM geo WHERE g && " + window + " ORDER BY id",
			"ROLLBACK TO sp", "SELECT count(*) FROM geo", "COMMIT"}},
		{"geo_large", []string{
			// (the bottom right corner lowered: the polygon stays simple)
			changed(fmt.Sprintf("UPDATE geo SET g = ST_MakePolygon(ST_SetPoint(ST_ExteriorRing(g), 1, ST_MakePoint(ST_XMax(g), ST_YMin(g) - %d))) WHERE kind = 'large'", 1+r.Intn(4))),
			"SELECT id, ST_NPoints(g), md5(ST_AsEWKB(g)), ST_AsText(ST_Envelope(g)) FROM geo WHERE kind = 'large' ORDER BY id",
			"SELECT id, ST_NPoints(ST_Simplify(g, 0.5)) FROM geo WHERE kind = 'large' ORDER BY id",
		}},
	}
}

func num(f float64) string { return fmt.Sprintf("%g", f) }

// Reduce removes whole transaction groups, preserving setup and failure identity.
// It is bounded; the result is a smaller reproducer, not necessarily minimal.
func Reduce(p Program, budget int, fails func(Program) bool) Program {
	for width := (len(p.Groups) + 1) / 2; width > 0 && budget > 0; {
		changed := false
		for start := 0; start < len(p.Groups) && budget > 0; start += width {
			end := min(start+width, len(p.Groups))
			trial := p
			trial.Groups = append(append([]Group{}, p.Groups[:start]...), p.Groups[end:]...)
			budget--
			if fails(trial) {
				p, changed = trial, true
				break
			}
		}
		if !changed {
			width /= 2
		}
	}
	return p
}
