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
// history, its public schema versioned, a connection per session, and one
// more that only looks (at heads, root hashes, and who waits on whom).
type Postgres struct {
	admin    *pgconn.PgConn
	observer *pgconn.PgConn
	db       string
	sessions []*pgconn.PgConn
	timeout  time.Duration
	step     int
	// waits is the sessions whose writes wait for a row lock, by session
	waits map[int]*wait
}

// wait is a write waiting for the row lock session on's transaction holds.
type wait struct {
	on   int
	done chan error
}

// OpenPostgres returns an Opener making a fresh database in the server dsn
// names for each history.
func OpenPostgres(dsn string, timeout time.Duration) Opener {
	return func(h History) (Engine, error) {
		p := &Postgres{timeout: timeout, waits: map[int]*wait{}}
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
		if p.observer, err = p.connect(dsn, db); err != nil {
			p.Close()
			return nil, err
		}
		setup := []string{"DROP SCHEMA public", "CREATE SCHEMA public VERSIONED"}
		for _, t := range Tables {
			c := ""
			if h.Setup.HasC(t) {
				c = ", c int"
			}
			setup = append(setup, "CREATE TABLE "+t+" (id int PRIMARY KEY, a int, b int"+c+")")
			ids := make([]int, 0, len(h.Setup[t]))
			for id := range h.Setup[t] {
				ids = append(ids, id)
			}
			sort.Ints(ids)
			for _, id := range ids {
				r := h.Setup[t][id]
				if c != "" {
					setup = append(setup, fmt.Sprintf("INSERT INTO %s VALUES (%d, %d, %d, NULLIF(%d, 0))", t, id, r[0], r[1], r[2]))
				} else {
					setup = append(setup, fmt.Sprintf("INSERT INTO %s VALUES (%d, %d, %d)", t, id, r[0], r[1]))
				}
			}
		}
		setup = append(setup, "CREATE TABLE "+Keyless+" (a int, b int)")
		for id := 1; id <= keys; id++ {
			if n := h.Setup[Keyless][id][0]; n > 0 {
				a, b := content(id)
				setup = append(setup, fmt.Sprintf("INSERT INTO %s SELECT %d, %d FROM generate_series(1, %d)", Keyless, a, b, n))
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

// content is keyless content n's columns.
func content(n int) (a, b int) { return (n - 1) / 4, (n - 1) % 4 }

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
	return execIn(ctx, c, sql)
}

func execIn(ctx context.Context, c *pgconn.PgConn, sql string) ([][]string, error) {
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
// or revert refused outside a transaction block), nothing to do, a change
// a dropped column would lose, or an outcome of its own naming the error,
// which no model predicts.
func classify(pgerr *pgconn.PgError) Outcome {
	switch pgerr.Code {
	case "40001":
		return Outcome{Class: ClassConflict}
	case "55000":
		if strings.Contains(pgerr.Message, "nothing to commit") || strings.Contains(pgerr.Message, "changes nothing") {
			return Outcome{Class: ClassUnchanged}
		}
	case "0A000":
		if strings.Contains(pgerr.Message, "can't be merged across column") {
			return Outcome{Class: ClassLostChange}
		}
	}
	return Outcome{Class: "error " + pgerr.Code + ": " + pgerr.Message}
}

// rowSQL is a row write's statement.  A keyless write picks one copy of
// its content.
func rowSQL(op Op) string {
	if op.Table == Keyless {
		a, b := content(op.ID)
		copyOf := fmt.Sprintf("ctid = (SELECT ctid FROM %s WHERE a = %d AND b = %d LIMIT 1)", op.Table, a, b)
		switch op.Kind {
		case OpInsert:
			return fmt.Sprintf("INSERT INTO %s VALUES (%d, %d)", op.Table, a, b)
		case OpUpdate:
			a2, b2 := content(op.Val)
			return fmt.Sprintf("UPDATE %s SET a = %d, b = %d WHERE %s", op.Table, a2, b2, copyOf)
		}
		return fmt.Sprintf("DELETE FROM %s WHERE %s", op.Table, copyOf)
	}
	switch op.Kind {
	case OpInsert:
		if op.C {
			return fmt.Sprintf("INSERT INTO %s VALUES (%d, %d, %d, NULLIF(%d, 0)) ON CONFLICT (id) DO UPDATE SET a = excluded.a, b = excluded.b, c = excluded.c",
				op.Table, op.ID, op.Val, op.Val2, op.Val3)
		}
		return fmt.Sprintf("INSERT INTO %s VALUES (%d, %d, %d) ON CONFLICT (id) DO UPDATE SET a = excluded.a, b = excluded.b",
			op.Table, op.ID, op.Val, op.Val2)
	case OpUpdate:
		value := strconv.Itoa(op.Val)
		if op.Col == 2 {
			value = fmt.Sprintf("NULLIF(%d, 0)", op.Val)
		}
		return fmt.Sprintf("UPDATE %s SET %s = %s WHERE id = %d", op.Table, []string{"a", "b", "c"}[op.Col], value, op.ID)
	}
	return fmt.Sprintf("DELETE FROM %s WHERE id = %d", op.Table, op.ID)
}

func (p *Postgres) Apply(op Op) (Outcome, error) {
	p.step++
	c := p.sessions[op.Session]
	message := quote(fmt.Sprintf("step %d", p.step))
	var sql string
	switch op.Kind {
	case OpInsert, OpUpdate, OpDelete:
		sql = rowSQL(op)
		if op.Wait > 0 {
			return p.waitFor(op, sql)
		}
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
	case OpAddColumn:
		sql = "ALTER TABLE " + op.Table + " ADD COLUMN c int"
	case OpDropColumn:
		sql = "ALTER TABLE " + op.Table + " DROP COLUMN c"
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
	o := Outcome{Class: ClassOK}
	if pgerr != nil {
		o = classify(pgerr)
	}
	if op.Kind == OpCommitTxn || op.Kind == OpRollback {
		return p.release(op.Session, o)
	}
	return o, nil
}

// waitFor starts a write expected to wait for session op.Wait-1's row
// lock, and returns once the server shows it waiting on that session's
// transaction (or once it finished without waiting).
func (p *Postgres) waitFor(op Op, sql string) (Outcome, error) {
	c := p.sessions[op.Session]
	// (the write waits until the holder's transaction ends, steps later,
	// so --timeout, a statement's, doesn't bound it: no statement or lock
	// timeout and no deadline while it waits.  release bounds the wait
	// once that transaction has ended.  In a transaction already failed
	// the SET fails, as the write will.)
	if _, err := p.exec(c, "SET lock_timeout = 0; SET statement_timeout = 0"); err != nil {
		if _, err := sqlError(err); err != nil {
			return Outcome{}, err
		}
	}
	done := make(chan error, 1)
	go func() {
		_, err := execIn(context.Background(), c, sql)
		done <- err
	}()
	blocked := fmt.Sprintf("SELECT %d = ANY(pg_blocking_pids(%d))", p.sessions[op.Wait-1].PID(), c.PID())
	for deadline := time.Now().Add(p.timeout); time.Now().Before(deadline); time.Sleep(time.Millisecond) {
		select {
		case err := <-done:
			if err := p.timed(c); err != nil {
				return Outcome{}, err
			}
			pgerr, err := sqlError(err)
			if err != nil {
				return Outcome{}, fmt.Errorf("%s: %w", sql, err)
			}
			if pgerr != nil {
				return classify(pgerr), nil
			}
			return Outcome{Class: ClassOK}, nil
		default:
		}
		rows, err := p.must(p.observer, blocked)
		if err != nil {
			return Outcome{}, err
		}
		if rows[0][0] == "t" {
			p.waits[op.Session] = &wait{on: op.Wait - 1, done: done}
			return Outcome{Class: ClassWaiting}, nil
		}
	}
	return Outcome{}, fmt.Errorf("%s: neither waited on session %d nor finished", sql, op.Wait-1)
}

// timed gives session connection c back the statement and lock timeouts
// its waiting write went without.  After a write whose error failed its
// transaction the SET fails too, and the rollback to come undoes the
// untimed one.
func (p *Postgres) timed(c *pgconn.PgConn) error {
	ms := p.timeout.Milliseconds()
	_, err := p.exec(c, fmt.Sprintf("SET lock_timeout = %d; SET statement_timeout = %d", ms, ms))
	_, err = sqlError(err)
	return err
}

// release finishes the writes that waited on session h's transaction, now
// ended: its outcome o, or one naming the first of them that failed.
func (p *Postgres) release(h int, o Outcome) (Outcome, error) {
	waiters := make([]int, 0, len(p.waits))
	for s, w := range p.waits {
		if w.on == h {
			waiters = append(waiters, s)
		}
	}
	sort.Ints(waiters)
	for _, s := range waiters {
		select {
		case err := <-p.waits[s].done:
			delete(p.waits, s)
			if err := p.timed(p.sessions[s]); err != nil {
				return Outcome{}, err
			}
			pgerr, err := sqlError(err)
			if err != nil {
				return Outcome{}, fmt.Errorf("session %d's waiting write: %w", s, err)
			}
			if pgerr != nil && o.Class == ClassOK {
				o.Class = fmt.Sprintf("session %d's waiting write: %s", s, classify(pgerr).Class)
			}
		case <-time.After(p.timeout):
			return Outcome{}, fmt.Errorf("session %d still waits after session %d's transaction ended", s, h)
		}
	}
	return o, nil
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
	c := p.sessions[s]
	for _, t := range Tables {
		rows, err := p.must(c, "SELECT id, a, b, coalesce((to_jsonb(r) ->> 'c')::int, 0) FROM "+t+" r")
		if err != nil {
			return nil, err
		}
		for _, r := range rows {
			var v [4]int
			for i := range v {
				v[i], _ = strconv.Atoi(r[i])
			}
			st[t][v[0]] = Row{v[1], v[2], v[3]}
		}
	}
	rows, err := p.must(c, "SELECT a * 4 + b + 1, count(*) FROM "+Keyless+" GROUP BY 1")
	if err != nil {
		return nil, err
	}
	for _, r := range rows {
		id, _ := strconv.Atoi(r[0])
		n, _ := strconv.Atoi(r[1])
		st[Keyless][id] = Row{n}
	}
	rows, err = p.must(c, "SELECT r.relname FROM pg_attribute a JOIN pg_class r ON r.oid = a.attrelid "+
		"WHERE r.relnamespace = 'public'::regnamespace AND a.attname = 'c' AND NOT a.attisdropped")
	if err != nil {
		return nil, err
	}
	for _, r := range rows {
		if i := tableIndex(r[0]); i > 0 {
			st[Columns][i] = Row{1}
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
	rows, err := p.must(p.observer, "SELECT name, hash FROM dolt_branches")
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
	rows, err := p.must(p.observer, "SELECT prolly_root_hash("+quote(commit)+")")
	if err != nil {
		return "", err
	}
	return rows[0][0], nil
}

func (p *Postgres) Close() {
	ctx, cancel := context.WithTimeout(context.Background(), p.timeout)
	defer cancel()
	// (writes still waiting are cancelled, and finish, before their
	// connections close)
	for s, w := range p.waits {
		if p.observer != nil {
			_, _ = p.exec(p.observer, fmt.Sprintf("SELECT pg_cancel_backend(%d)", p.sessions[s].PID()))
		}
		select {
		case <-w.done:
		case <-time.After(p.timeout):
		}
	}
	for _, c := range p.sessions {
		c.Close(ctx)
	}
	if p.observer != nil {
		p.observer.Close(ctx)
	}
	if p.admin != nil {
		if p.db != "" {
			_, _ = p.exec(p.admin, "DROP DATABASE "+p.db+" WITH (FORCE)")
		}
		p.admin.Close(ctx)
	}
}
