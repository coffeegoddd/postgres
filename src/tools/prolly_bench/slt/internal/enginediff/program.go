// Package enginediff compares reproducible transaction histories with heap.
package enginediff

import (
	"fmt"
	"math/rand"
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
}

func Generate(seed int64, steps int) Program {
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
		// Rotate the first full cycle so short runs vary without losing families.
		family := (i + int(uint64(seed)%uint64(len(families)))) % len(families)
		p.Groups = append(p.Groups, families[family])
	}
	return p
}

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
