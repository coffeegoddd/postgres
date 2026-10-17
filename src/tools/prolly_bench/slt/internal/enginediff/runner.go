package enginediff

import (
	"context"
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"reflect"
	"strings"
	"time"

	"github.com/jackc/pgx/v5/pgconn"
)

type Column struct {
	Name string `json:"name"`
	Type uint32 `json:"type"`
}

type Answer struct {
	Tag     string   `json:"tag"`
	Columns []Column `json:"columns,omitempty"`
	Rows    int      `json:"rows"`
	Digest  string   `json:"digest"`
}

type Outcome struct {
	Answers     []Answer `json:"answers,omitempty"`
	SQLState    string   `json:"sqlstate,omitempty"`
	Transaction string   `json:"transaction"`
}

type Failure struct {
	Kind      string  `json:"kind"`
	Group     string  `json:"group"`
	SQL       string  `json:"sql"`
	Baseline  Outcome `json:"baseline"`
	Candidate Outcome `json:"candidate"`
	Target    string  `json:"target"`
}

func (f *Failure) Signature() string {
	return f.Kind + ":" + f.Target + ":" + f.Group + ":" + f.SQL + ":" + f.Baseline.SQLState + ":" + f.Candidate.SQLState
}

type session struct {
	conn    *pgconn.PgConn
	timeout time.Duration
}

func (s *session) exec(sql string) (Outcome, error) {
	ctx, cancel := context.WithTimeout(context.Background(), s.timeout)
	defer cancel()
	results, err := s.conn.Exec(ctx, sql).ReadAll()
	o := Outcome{Transaction: string(s.conn.TxStatus())}
	if err != nil {
		var pgerr *pgconn.PgError
		if !errors.As(err, &pgerr) || s.conn.IsClosed() || pgerr.Severity == "FATAL" || pgerr.Code == "57014" {
			return o, fmt.Errorf("SQL %q: %w", sql, err)
		}
		o.SQLState = pgerr.Code
	}
	for _, result := range results {
		a := Answer{Tag: result.CommandTag.String(), Rows: len(result.Rows)}
		for _, f := range result.FieldDescriptions {
			a.Columns = append(a.Columns, Column{Name: f.Name, Type: f.DataTypeOID})
		}
		// JSON preserves NULL versus empty bytes and frames every field and row.
		data, err := json.Marshal(result.Rows)
		if err != nil {
			return o, err
		}
		h := sha256.Sum256(data)
		a.Digest = hex.EncodeToString(h[:])
		o.Answers = append(o.Answers, a)
	}
	return o, nil
}

func (s *session) must(sql string) error {
	o, err := s.exec(sql)
	if err != nil {
		return err
	}
	if o.SQLState != "" {
		return fmt.Errorf("SQLSTATE %s: %s", o.SQLState, sql)
	}
	return nil
}

func connect(dsn, db string, timeout time.Duration) (*session, error) {
	cfg, err := pgconn.ParseConfig(dsn)
	if err != nil {
		return nil, err
	}
	cfg.Database = db
	cfg.RuntimeParams["search_path"] = "public"
	cfg.RuntimeParams["timezone"] = "UTC"
	cfg.RuntimeParams["datestyle"] = "ISO, YMD"
	cfg.RuntimeParams["extra_float_digits"] = "3"
	cfg.RuntimeParams["statement_timeout"] = fmt.Sprint(timeout.Milliseconds())
	cfg.RuntimeParams["lock_timeout"] = fmt.Sprint(timeout.Milliseconds())
	ctx, cancel := context.WithTimeout(context.Background(), timeout)
	defer cancel()
	c, err := pgconn.ConnectConfig(ctx, cfg)
	if err != nil {
		return nil, err
	}
	return &session{c, timeout}, nil
}

func (s *session) close() {
	ctx, cancel := context.WithTimeout(context.Background(), s.timeout)
	defer cancel()
	s.conn.Close(ctx)
}

func (s *session) target(am string) error {
	if am == "prolly" {
		if err := s.must("DROP SCHEMA public; CREATE SCHEMA public VERSIONED"); err != nil {
			return err
		}
	}
	if err := s.must("SET default_table_access_method = '" + am + "'"); err != nil {
		return err
	}
	if err := s.must("CREATE TABLE target_canary (id int PRIMARY KEY, payload text)"); err != nil {
		return err
	}
	if err := s.checkAM(am); err != nil {
		return err
	}
	if am == "prolly" {
		if err := s.must(`DO $$ BEGIN
IF (SELECT count(*) FROM pg_class WHERE
  (oid = 'target_canary'::regclass OR oid = 'target_canary_pkey'::regclass OR
   oid = (SELECT reltoastrelid FROM pg_class WHERE oid = 'target_canary'::regclass))
  AND (pg_stat_file(pg_relation_filepath(oid) || '_bmap', true)).size IS NOT NULL) <> 3
THEN RAISE EXCEPTION 'table, index or TOAST is not block-mapped'; END IF; END $$`); err != nil {
			return err
		}
	}
	return s.must("DROP TABLE target_canary")
}

func (s *session) checkAM(am string) error {
	return s.must(`DO $$ BEGIN IF EXISTS (
SELECT FROM pg_class c JOIN pg_am a ON a.oid = c.relam
WHERE c.relnamespace = 'public'::regnamespace AND c.relkind IN ('r','m')
AND a.amname <> '` + am + `') THEN RAISE EXCEPTION 'wrong table AM'; END IF; END $$`)
}

func (s *session) requirePlan(sql, node string) error {
	ctx, cancel := context.WithTimeout(context.Background(), s.timeout)
	defer cancel()
	plan, err := s.conn.Exec(ctx, "EXPLAIN (FORMAT JSON, COSTS OFF) "+sql).ReadAll()
	if err != nil {
		return err
	}
	if len(plan) != 1 || len(plan[0].Rows) != 1 ||
		!strings.Contains(string(plan[0].Rows[0][0]), `"Node Type": "`+node+`"`) {
		return fmt.Errorf("probe did not use %s", node)
	}
	return nil
}

var snapshots = []string{
	"SELECT id, v, n, txt, b FROM t ORDER BY id",
	"SELECT id, parent FROM child ORDER BY id",
	"SELECT id, v FROM audit ORDER BY id, v",
}

// Compare uses fresh, randomly named databases for every replay. SQL errors are
// comparable; lost connections, timeouts and target failures always fail the run.
func Compare(dsn, am string, p Program, timeout time.Duration) (*Failure, error) {
	switch am {
	case "heap", "prolly":
	default:
		return nil, fmt.Errorf("unknown engine %q", am)
	}
	if p.Version != 1 || len(p.Setup) == 0 {
		return nil, fmt.Errorf("unsupported or empty replay program")
	}
	admin, err := connect(dsn, "postgres", timeout)
	if err != nil {
		return nil, err
	}
	defer admin.close()
	var nonce [8]byte
	if _, err := rand.Read(nonce[:]); err != nil {
		return nil, err
	}
	var sessions []*session
	for i, target := range []string{"heap", am} {
		db := fmt.Sprintf("engine_diff_%x_%d", nonce, i)
		if err := admin.must("CREATE DATABASE " + db + " TEMPLATE template0"); err != nil {
			return nil, err
		}
		defer admin.must("DROP DATABASE " + db + " WITH (FORCE)")
		s, err := connect(dsn, db, timeout)
		if err != nil {
			return nil, err
		}
		defer s.close()
		if err := s.target(target); err != nil {
			return nil, fmt.Errorf("target %s: %w", target, err)
		}
		sessions = append(sessions, s)
	}
	a, b := sessions[0], sessions[1]
	compare := func(group, sql string) (*Failure, error) {
		x, err := a.exec(sql)
		if err != nil {
			return nil, fmt.Errorf("heap/%s: %w", group, err)
		}
		y, err := b.exec(sql)
		if err != nil {
			return nil, fmt.Errorf("%s/%s: %w", am, group, err)
		}
		if !reflect.DeepEqual(x, y) {
			return &Failure{Kind: "mismatch", Group: group, SQL: sql, Baseline: x, Candidate: y, Target: am}, nil
		}
		if x.SQLState != p.Errors[sql] {
			return nil, fmt.Errorf("unexpected baseline SQLSTATE %q (expected %q): %s", x.SQLState, p.Errors[sql], sql)
		}
		return nil, nil
	}
	for _, sql := range p.Setup {
		if f, err := compare("setup", sql); f != nil || err != nil {
			return f, err
		}
	}
	groups := append([]Group{{Name: "initial"}}, p.Groups...)
	for _, g := range groups {
		for _, sql := range g.SQL {
			if f, err := compare(g.Name, sql); f != nil || err != nil {
				return f, err
			}
		}
		if a.conn.TxStatus() != 'I' || b.conn.TxStatus() != 'I' {
			return nil, fmt.Errorf("group %s leaves an open transaction", g.Name)
		}
		for i, s := range sessions {
			target := []string{"heap", am}[i]
			if err := s.checkAM(target); err != nil {
				return nil, err
			}
		}
		for _, sql := range snapshots {
			if f, err := compare(g.Name+"/state", sql); f != nil || err != nil {
				return f, err
			}
		}
		// Exercise the primary-key index separately and compare it with a scan
		// within each target as well as with heap. The index path is verified.
		query := "SELECT id, v, n, txt, b FROM t WHERE id BETWEEN 1 AND 250 ORDER BY id"
		for i, s := range sessions {
			if err := s.must("SET enable_indexscan = off; SET enable_bitmapscan = off"); err != nil {
				return nil, err
			}
			if err := s.requirePlan(query, "Seq Scan"); err != nil {
				return nil, err
			}
			scan, err := s.exec(query)
			if err != nil {
				return nil, err
			}
			if scan.SQLState != "" {
				return nil, fmt.Errorf("scan failed with SQLSTATE %s", scan.SQLState)
			}
			if err := s.must("SET enable_indexscan = on; SET enable_seqscan = off; SET enable_indexonlyscan = off"); err != nil {
				return nil, err
			}
			if err := s.requirePlan(query, "Index Scan"); err != nil {
				return nil, err
			}
			indexed, err := s.exec(query)
			if err != nil {
				return nil, err
			}
			if !reflect.DeepEqual(scan, indexed) {
				return &Failure{Kind: "index_scan_mismatch", Group: g.Name, SQL: query, Baseline: scan, Candidate: indexed, Target: []string{"heap", am}[i]}, nil
			}
			if err := s.must("RESET enable_indexscan; RESET enable_bitmapscan; RESET enable_seqscan; RESET enable_indexonlyscan"); err != nil {
				return nil, err
			}
		}
	}
	return nil, nil
}
