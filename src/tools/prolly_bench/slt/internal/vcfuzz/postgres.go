package vcfuzz

import (
	"context"
	"crypto/rand"
	"errors"
	"fmt"
	"sort"
	"strconv"
	"strings"
	"time"

	"github.com/jackc/pgx/v5/pgconn"
)

// Postgres is a prolly database in a Postgres server: a fresh database per
// history, its public schema versioned, a connection per session.
type Postgres struct {
	admin    *pgconn.PgConn
	db       string
	sessions []*pgconn.PgConn
	timeout  time.Duration
	step     int
}

// OpenPostgres returns an Opener making a fresh database in the server dsn
// names for each history.
func OpenPostgres(dsn string, timeout time.Duration) Opener {
	return func(h History) (Engine, error) {
		p := &Postgres{timeout: timeout}
		admin, err := p.connect(dsn, "postgres")
		if err != nil {
			return nil, err
		}
		p.admin = admin
		var nonce [8]byte
		if _, err := rand.Read(nonce[:]); err != nil {
			p.Close()
			return nil, err
		}
		db := fmt.Sprintf("vc_fuzz_%x", nonce)
		if _, err := p.exec(admin, "CREATE DATABASE "+db+" TEMPLATE template0"); err != nil {
			p.Close()
			return nil, err
		}
		p.db = db
		for i := 0; i < max(h.Sessions, 1); i++ {
			c, err := p.connect(dsn, db)
			if err != nil {
				p.Close()
				return nil, err
			}
			p.sessions = append(p.sessions, c)
		}
		setup := []string{"DROP SCHEMA public", "CREATE SCHEMA public VERSIONED"}
		for _, t := range Tables {
			setup = append(setup, "CREATE TABLE "+t+" (id int PRIMARY KEY, a int, b int)")
			ids := make([]int, 0, len(h.Setup[t]))
			for id := range h.Setup[t] {
				ids = append(ids, id)
			}
			sort.Ints(ids)
			for _, id := range ids {
				r := h.Setup[t][id]
				setup = append(setup, fmt.Sprintf("INSERT INTO %s VALUES (%d, %d, %d)", t, id, r[0], r[1]))
			}
		}
		setup = append(setup, "SELECT dolt_commit('-Am', 'setup')")
		for _, sql := range setup {
			if _, err := p.must(p.sessions[0], sql); err != nil {
				p.Close()
				return nil, err
			}
		}
		return p, nil
	}
}

func (p *Postgres) connect(dsn, db string) (*pgconn.PgConn, error) {
	cfg, err := pgconn.ParseConfig(dsn)
	if err != nil {
		return nil, err
	}
	cfg.Database = db
	cfg.RuntimeParams["statement_timeout"] = fmt.Sprint(p.timeout.Milliseconds())
	cfg.RuntimeParams["lock_timeout"] = fmt.Sprint(p.timeout.Milliseconds())
	ctx, cancel := context.WithTimeout(context.Background(), p.timeout)
	defer cancel()
	return pgconn.ConnectConfig(ctx, cfg)
}

// exec runs sql, returning its rows as text and any SQL error apart.
func (p *Postgres) exec(c *pgconn.PgConn, sql string) ([][]string, error) {
	ctx, cancel := context.WithTimeout(context.Background(), p.timeout)
	defer cancel()
	results, err := c.Exec(ctx, sql).ReadAll()
	var rows [][]string
	for _, r := range results {
		for _, row := range r.Rows {
			var cells []string
			for _, cell := range row {
				cells = append(cells, string(cell))
			}
			rows = append(rows, cells)
		}
	}
	return rows, err
}

// sqlError is the SQL error err holds, or nil; a lost connection, timeout
// or fatal error is the harness's.
func sqlError(err error) (*pgconn.PgError, error) {
	if err == nil {
		return nil, nil
	}
	var pgerr *pgconn.PgError
	if !errors.As(err, &pgerr) || pgerr.Severity == "FATAL" || pgerr.Code == "57014" || pgerr.Code == "55P03" {
		return nil, err
	}
	return pgerr, nil
}

func (p *Postgres) must(c *pgconn.PgConn, sql string) ([][]string, error) {
	rows, err := p.exec(c, sql)
	if err != nil {
		return nil, fmt.Errorf("%s: %w", sql, err)
	}
	return rows, nil
}

func quote(s string) string { return "'" + strings.ReplaceAll(s, "'", "''") + "'" }

// classify is the outcome of a failed VC call: a conflict (a merge, pick
// or revert refused outside a transaction block), nothing to do, or an
// outcome of its own naming the error, which no model predicts.
func classify(pgerr *pgconn.PgError) Outcome {
	switch pgerr.Code {
	case "40001":
		return Outcome{Class: ClassConflict}
	case "55000":
		if strings.Contains(pgerr.Message, "nothing to commit") || strings.Contains(pgerr.Message, "changes nothing") {
			return Outcome{Class: ClassUnchanged}
		}
	}
	return Outcome{Class: "error " + pgerr.Code + ": " + pgerr.Message}
}

func (p *Postgres) Apply(op Op) (Outcome, error) {
	p.step++
	c := p.sessions[op.Session]
	message := quote(fmt.Sprintf("step %d", p.step))
	var sql string
	switch op.Kind {
	case OpInsert:
		sql = fmt.Sprintf("INSERT INTO %s VALUES (%d, %d, %d) ON CONFLICT (id) DO UPDATE SET a = excluded.a, b = excluded.b",
			op.Table, op.ID, op.Val, op.Val2)
	case OpUpdate:
		sql = fmt.Sprintf("UPDATE %s SET %s = %d WHERE id = %d", op.Table, []string{"a", "b"}[op.Col], op.Val, op.ID)
	case OpDelete:
		sql = fmt.Sprintf("DELETE FROM %s WHERE id = %d", op.Table, op.ID)
	case OpBegin:
		sql = "BEGIN"
	case OpCommitTxn:
		sql = "COMMIT"
	case OpRollback:
		sql = "ROLLBACK"
	case OpCommit:
		sql = "SELECT dolt_commit('-Am', " + message + ")"
	case OpBranch:
		sql = "SELECT dolt_branch(" + quote(op.Ref) + ")"
	case OpCheckout:
		sql = "SELECT dolt_checkout(" + quote(op.Ref) + ")"
	case OpReset:
		sql = "SELECT dolt_reset('--hard', " + quote(op.Ref) + ")"
	case OpPick:
		sql = "SELECT dolt_cherry_pick(" + quote(op.Ref) + ")"
	case OpRevert:
		sql = "SELECT dolt_revert(" + quote(op.Ref) + ")"
	case OpMerge:
		return p.merge(c, op, message)
	default:
		return Outcome{}, fmt.Errorf("unknown op %q", op.Kind)
	}
	_, err := p.exec(c, sql)
	pgerr, err := sqlError(err)
	if err != nil {
		return Outcome{}, fmt.Errorf("%s: %w", sql, err)
	}
	if pgerr != nil {
		return classify(pgerr), nil
	}
	return Outcome{Class: ClassOK}, nil
}

// mergeOutcome reads dolt_merge's result (commit id, fast-forward,
// conflicts, message): its class, or one naming a result of another shape.
func mergeOutcome(rows [][]string) Outcome {
	if len(rows) != 1 || len(rows[0]) != 1 {
		return Outcome{Class: fmt.Sprintf("unexpected merge result: %v", rows)}
	}
	result := strings.Trim(rows[0][0], "{}")
	fields := strings.SplitN(result, ",", 4)
	switch {
	case len(fields) != 4:
		return Outcome{Class: "unexpected merge result: " + rows[0][0]}
	case strings.Contains(fields[3], "up-to-date") || strings.Contains(fields[3], "is ahead of"):
		return Outcome{Class: ClassUpToDate}
	case fields[1] == "1":
		return Outcome{Class: ClassFastForward}
	case fields[2] != "0":
		return Outcome{Class: ClassConflict}
	}
	return Outcome{Class: ClassOK}
}

// merge merges outside a transaction block, or in one, resolving or
// aborting the conflicts it finds.
func (p *Postgres) merge(c *pgconn.PgConn, op Op, message string) (Outcome, error) {
	args := []string{"'-m'", message}
	if op.NoFF {
		args = append(args, "'--no-ff'")
	}
	call := "SELECT dolt_merge(" + strings.Join(append(args, quote(op.Ref)), ", ") + ")"
	block := op.Mode != ModeAuto
	if block {
		if _, err := p.must(c, "BEGIN"); err != nil {
			return Outcome{}, err
		}
	}
	rows, err := p.exec(c, call)
	pgerr, err := sqlError(err)
	if err != nil {
		return Outcome{}, fmt.Errorf("%s: %w", call, err)
	}
	if pgerr != nil {
		if block {
			if _, err := p.must(c, "ROLLBACK"); err != nil {
				return Outcome{}, err
			}
		}
		return classify(pgerr), nil
	}
	o := mergeOutcome(rows)
	switch o.Class {
	case ClassOK, ClassUpToDate, ClassFastForward, ClassConflict:
	default:
		// (a result no model predicts: reported, the block ended)
		if block {
			if _, err := p.must(c, "ROLLBACK"); err != nil {
				return Outcome{}, err
			}
		}
		return o, nil
	}
	if o.Class == ClassConflict {
		n, err := p.must(c, "SELECT coalesce(sum(num_conflicts), 0) FROM dolt_conflicts")
		if err != nil {
			return Outcome{}, err
		}
		o.Conflicts, _ = strconv.Atoi(n[0][0])
		var resolve []string
		switch op.Mode {
		case ModeOurs, ModeTheirs:
			resolve = []string{"SELECT dolt_conflicts_resolve('--" + op.Mode + "', table_name) FROM dolt_conflicts",
				"SELECT dolt_commit('-am', " + message + ")"}
		case ModeAbort:
			resolve = []string{"SELECT dolt_merge('--abort')"}
		}
		for _, sql := range resolve {
			_, err := p.exec(c, sql)
			pgerr, err := sqlError(err)
			if err != nil {
				return Outcome{}, fmt.Errorf("%s: %w", sql, err)
			}
			if pgerr != nil {
				_, _ = p.exec(c, "ROLLBACK")
				o = classify(pgerr)
				o.Class = "after conflicts, " + o.Class
				return o, nil
			}
		}
	}
	if block {
		_, err := p.exec(c, "COMMIT")
		pgerr, err := sqlError(err)
		if err != nil {
			return Outcome{}, err
		}
		if pgerr != nil {
			o = classify(pgerr)
			o.Class = "at COMMIT, " + o.Class
		}
	}
	return o, nil
}

func (p *Postgres) Visible(s int) (State, error) {
	st := emptyState()
	for _, t := range Tables {
		rows, err := p.must(p.sessions[s], "SELECT id, a, b FROM "+t)
		if err != nil {
			return nil, err
		}
		for _, r := range rows {
			id, _ := strconv.Atoi(r[0])
			a, _ := strconv.Atoi(r[1])
			b, _ := strconv.Atoi(r[2])
			st[t][id] = Row{a, b}
		}
	}
	return st, nil
}

func (p *Postgres) Branch(s int) (string, error) {
	rows, err := p.must(p.sessions[s], "SELECT active_branch()")
	if err != nil {
		return "", err
	}
	return rows[0][0], nil
}

func (p *Postgres) Dirty(s int) (bool, error) {
	rows, err := p.must(p.sessions[s], "SELECT EXISTS (SELECT FROM dolt_status)")
	if err != nil {
		return false, err
	}
	return rows[0][0] == "t", nil
}

func (p *Postgres) Heads() (map[string]string, error) {
	rows, err := p.must(p.sessions[0], "SELECT name, hash FROM dolt_branches")
	if err != nil {
		return nil, err
	}
	heads := map[string]string{}
	for _, r := range rows {
		heads[r[0]] = r[1]
	}
	return heads, nil
}

func (p *Postgres) RootHash(commit string) (string, error) {
	rows, err := p.must(p.sessions[0], "SELECT prolly_root_hash("+quote(commit)+")")
	if err != nil {
		return "", err
	}
	return rows[0][0], nil
}

func (p *Postgres) Close() {
	ctx, cancel := context.WithTimeout(context.Background(), p.timeout)
	defer cancel()
	for _, c := range p.sessions {
		c.Close(ctx)
	}
	if p.admin != nil {
		if p.db != "" {
			_, _ = p.exec(p.admin, "DROP DATABASE "+p.db+" WITH (FORCE)")
		}
		p.admin.Close(ctx)
	}
}
