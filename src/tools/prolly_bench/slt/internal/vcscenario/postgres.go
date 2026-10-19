package vcscenario

import (
	"context"
	"crypto/rand"
	_ "embed"
	"errors"
	"fmt"
	"strings"
	"time"

	"github.com/jackc/pgx/v5/pgconn"
)

type Profile struct {
	Engine      string   `json:"engine"`
	Supported   []string `json:"supported"`
	Limitations string   `json:"limitations"`
}

func EngineProfile(am string) (Profile, error) {
	p := Profile{Engine: am}
	switch am {
	case "prolly":
		p.Supported = []string{"sql", "vc.core", "vc.merge", "vc.conflicts", "vc.violations", "vc.cherrypick", "vc.revert", "vc.diff", "vc.mergebase"}
		p.Limitations = "Versioned public schema. No staging area: commits take the whole working set and only hard resets exist. " +
			"Rows merge column by column, and a table's definition part by part (a column or constraint both sides changed differently is a schema conflict, an error: there is no dolt_schema_conflicts). " +
			"No claim about concurrent sessions beyond sequential interleavings."
	case "heap":
		p.Supported = []string{"sql"}
		p.Limitations = "Ordinary heap has no version-control capability."
	default:
		return p, fmt.Errorf("unknown engine %q", am)
	}
	return p, nil
}

type Postgres struct {
	profile  Profile
	dsn      string
	timeout  time.Duration
	admin    *pgconn.PgConn
	db       string
	sessions map[string]*pgconn.PgConn
}

func NewPostgres(dsn, am string, timeout time.Duration) (*Postgres, error) {
	p, err := EngineProfile(am)
	if err != nil {
		return nil, err
	}
	if timeout <= 0 {
		return nil, fmt.Errorf("timeout must be positive")
	}
	return &Postgres{profile: p, dsn: dsn, timeout: timeout}, nil
}
func (p *Postgres) Name() string           { return p.profile.Engine }
func (p *Postgres) Capabilities() []string { return p.profile.Supported }

// The built-in target probes are also reviewable scenarios, interpreted by
// the same executor as the corpus. Their databases are dropped before the
// actual case. The first proves commits, branches and resets reach the
// tables; the second, run when a case needs more, proves merges, diffs,
// picks, reverts, conflicts and violations do: a claimed capability whose
// calls don't work fails targeting, not the case's steps.
//
//go:embed cases/core-history.json
var targetScenario []byte

//go:embed cases/vc-probe.json
var capabilityScenario []byte

func (p *Postgres) Verify(ctx context.Context, requires []string) error {
	vc, beyond := false, false
	for _, r := range requires {
		if strings.HasPrefix(r, "vc.") {
			vc = true
			beyond = beyond || r != "vc.core"
		}
	}
	if err := p.canary(ctx, targetScenario, vc, "VC canary"); err != nil {
		return err
	}
	if beyond {
		return p.canary(ctx, capabilityScenario, true, "VC capability canary")
	}
	return nil
}

// canary opens a probe database (checking its target), and runs scenario b
// there if run.
func (p *Postgres) canary(ctx context.Context, b []byte, run bool, what string) (err error) {
	probe, _ := NewPostgres(p.dsn, p.Name(), p.timeout)
	defer func() { err = errors.Join(err, probe.Close(context.Background())) }()
	s, e := Parse(b)
	if e != nil {
		return e
	}
	if e = probe.Open(ctx, s.Sessions); e != nil {
		return e
	}
	if run {
		if _, detail := execute(ctx, s.Passes()[0], probe); detail != "" {
			return fmt.Errorf("%s: %s", what, detail)
		}
	}
	return nil
}

func (p *Postgres) connect(ctx context.Context, db string) (*pgconn.PgConn, error) {
	cfg, err := pgconn.ParseConfig(p.dsn)
	if err != nil {
		return nil, err
	}
	cfg.Database = db
	cfg.RuntimeParams["search_path"] = "public"
	cfg.RuntimeParams["timezone"] = "UTC"
	cfg.RuntimeParams["datestyle"] = "ISO, YMD"
	cfg.RuntimeParams["extra_float_digits"] = "3"
	cfg.RuntimeParams["statement_timeout"] = fmt.Sprint(p.timeout.Milliseconds())
	cfg.RuntimeParams["lock_timeout"] = fmt.Sprint(p.timeout.Milliseconds())
	ctx, cancel := context.WithTimeout(ctx, p.timeout)
	defer cancel()
	return pgconn.ConnectConfig(ctx, cfg)
}

func (p *Postgres) query(ctx context.Context, c *pgconn.PgConn, sql string, params []*string) (Outcome, error) {
	ctx, cancel := context.WithTimeout(ctx, p.timeout)
	defer cancel()
	args := make([][]byte, len(params))
	for i, v := range params {
		if v != nil {
			args[i] = []byte(*v)
		}
	}
	r := c.ExecParams(ctx, sql, args, nil, nil, nil).Read()
	o := Outcome{Tag: r.CommandTag.String(), Transaction: string(c.TxStatus()), Rows: [][]*string{}}
	if r.Err != nil {
		var pgerr *pgconn.PgError
		if !errors.As(r.Err, &pgerr) || c.IsClosed() || pgerr.Severity == "FATAL" || pgerr.Severity == "PANIC" || pgerr.Code == "57014" {
			return o, r.Err
		}
		o.SQLState = pgerr.Code
	}
	for _, f := range r.FieldDescriptions {
		o.Columns = append(o.Columns, f.Name)
	}
	for _, row := range r.Rows {
		cells := make([]*string, len(row))
		for i, v := range row {
			if v != nil {
				x := string(v)
				cells[i] = &x
			}
		}
		o.Rows = append(o.Rows, cells)
	}
	return o, nil
}

func (p *Postgres) must(ctx context.Context, c *pgconn.PgConn, sql string, params ...*string) (Outcome, error) {
	o, err := p.query(ctx, c, sql, params)
	if err == nil && o.SQLState != "" {
		err = fmt.Errorf("SQLSTATE %s executing %s", o.SQLState, sql)
	}
	return o, err
}

func (p *Postgres) Open(ctx context.Context, names []string) (err error) {
	if p.admin != nil {
		return fmt.Errorf("adapter already open")
	}
	p.admin, err = p.connect(ctx, "postgres")
	if err != nil {
		return err
	}
	var nonce [8]byte
	if _, err = rand.Read(nonce[:]); err != nil {
		return err
	}
	p.db = fmt.Sprintf("vc_case_%x", nonce)
	if _, err = p.must(ctx, p.admin, "CREATE DATABASE "+p.db+" TEMPLATE template0"); err != nil {
		return err
	}
	// The AM is from the closed EngineProfile whitelist; database names are random hex.
	if _, err = p.must(ctx, p.admin, "ALTER DATABASE "+p.db+" SET default_table_access_method = '"+p.Name()+"'"); err != nil {
		return err
	}
	p.sessions = map[string]*pgconn.PgConn{}
	for _, name := range names {
		c, e := p.connect(ctx, p.db)
		if e != nil {
			return e
		}
		p.sessions[name] = c
	}
	if len(names) == 0 {
		return fmt.Errorf("no sessions")
	}
	c := p.sessions[names[0]]
	if p.Name() == "prolly" {
		for _, q := range []string{"DROP SCHEMA public", "CREATE SCHEMA public VERSIONED"} {
			if _, err = p.must(ctx, c, q); err != nil {
				return err
			}
		}
	}
	if _, err = p.must(ctx, c, "CREATE TABLE __vc_target (id int PRIMARY KEY, payload text)"); err != nil {
		return err
	}
	if err = p.CheckTarget(ctx, names[0]); err != nil {
		return err
	}
	if p.Name() == "prolly" {
		_, err = p.must(ctx, c, `DO $$ BEGIN
IF (SELECT count(*) FROM pg_class WHERE
 (oid = '__vc_target'::regclass OR oid = '__vc_target_pkey'::regclass OR
  oid = (SELECT reltoastrelid FROM pg_class WHERE oid = '__vc_target'::regclass))
 AND (pg_stat_file(pg_relation_filepath(oid) || '_bmap', true)).size IS NOT NULL) <> 3
THEN RAISE EXCEPTION 'table, index or TOAST is not block-mapped'; END IF; END $$`)
		if err != nil {
			return err
		}
	}
	_, err = p.must(ctx, c, "DROP TABLE __vc_target")
	return err
}

func (p *Postgres) Execute(ctx context.Context, s Step, params, args []*string) (Outcome, error) {
	c, ok := p.sessions[s.Session]
	if !ok {
		return Outcome{}, fmt.Errorf("unknown session %s", s.Session)
	}
	if s.Op == "exec" || s.Op == "query" {
		return p.query(ctx, c, s.SQL, params)
	}
	if c.TxStatus() != 'I' {
		return Outcome{}, fmt.Errorf("VC operations require SQL-committed writes; session is in transaction state %c", c.TxStatus())
	}
	if s.Op == "connect" {
		return p.reconnect(ctx, s.Session, args)
	}
	var sql string
	switch s.Op {
	case "commit":
		sql = "SELECT (dolt_commit('-m', $1))[1]"
		if len(args) > 1 {
			sql = "SELECT (dolt_commit(" + placeholders(len(args))[len("$1::text, "):] + ", '-m', $1))[1]"
		}
	case "branch":
		sql = "SELECT dolt_branch($1)"
		if len(args) == 2 {
			sql = "SELECT dolt_branch($1, $2)"
		}
	case "checkout":
		sql = "SELECT dolt_checkout($1)"
	case "reset":
		sql = "SELECT dolt_reset('--hard', $1)"
	case "merge":
		// the commit made or fast-forwarded to; none if nothing was merged
		// (up to date, or the branch is ahead), or it stopped with conflicts
		sql = "SELECT CASE WHEN r[3] = '0' AND r[1] <> '' AND r[4] NOT IN " +
			"('Everything up-to-date', 'cannot fast forward from a to b. a is ahead of b already') " +
			"THEN r[1] END, r::text FROM dolt_merge(" + placeholders(len(args)) + ") r"
	case "cherrypick":
		sql = "SELECT NULLIF(r[1], ''), r::text FROM dolt_cherry_pick(" + placeholders(len(args)) + ") r"
	case "revert":
		// the last commit of its series; none if one stopped it
		sql = "SELECT NULLIF(r[1], ''), r::text FROM dolt_revert(" + placeholders(len(args)) + ") r"
	default:
		return Outcome{}, fmt.Errorf("unsupported operation %s", s.Op)
	}
	o, err := p.query(ctx, c, sql, args)
	if err != nil || o.SQLState != "" {
		return o, err
	}
	// The commit made, alone; the call's whole result in the tag, so a trace
	// shows why there was none.
	if s.Op == "merge" || s.Op == "cherrypick" || s.Op == "revert" {
		if len(o.Rows) != 1 || len(o.Rows[0]) != 2 {
			return o, fmt.Errorf("%s returned %d rows", ops[s.Op].function, len(o.Rows))
		}
		o.Tag = ops[s.Op].function + " returned " + *o.Rows[0][1]
		o.Rows, o.Columns = [][]*string{{o.Rows[0][0]}}, o.Columns[:1]
	}
	if s.Op == "checkout" {
		check, e := p.must(ctx, c, "SELECT active_branch()")
		if e != nil {
			return o, e
		}
		if len(check.Rows) != 1 || len(check.Rows[0]) != 1 || check.Rows[0][0] == nil || *check.Rows[0][0] != *args[0] {
			return o, fmt.Errorf("checkout did not select requested branch")
		}
	}
	return o, nil
}

// reconnect opens session's connection again, to the case's database or,
// given a branch, to "database/branch" (Dolt's USE mydb/branch). A
// connection refused for its branch is an outcome, its SQLSTATE, and
// leaves the session as it was, as a failed USE does; any other refusal is
// the target's failure. Otherwise the old connection is closed, and its
// backend gone, before the step ends: its lock on its branch with it. The
// new connection must be on the branch named, or the default one.
func (p *Postgres) reconnect(ctx context.Context, session string, args []*string) (Outcome, error) {
	old := p.sessions[session]
	db, branch, err := connectTarget(p.db, args)
	if err != nil {
		return Outcome{}, err
	}
	c, err := p.connect(ctx, db)
	if err != nil {
		if code := branchRefusal(err, len(args) == 1); code != "" {
			return Outcome{Rows: [][]*string{}, SQLState: code, Transaction: string(old.TxStatus())}, nil
		}
		return Outcome{}, err
	}
	pid := fmt.Sprint(old.PID())
	deadline, cancel := context.WithTimeout(ctx, p.timeout)
	defer cancel()
	if err = replace(deadline, p.sessions, session, c); err != nil {
		return Outcome{}, err
	}
	for {
		o, err := p.must(ctx, p.admin, "SELECT count(*) FROM pg_stat_activity WHERE pid = $1", &pid)
		if err != nil {
			return Outcome{}, err
		}
		if len(o.Rows) == 1 && len(o.Rows[0]) == 1 && o.Rows[0][0] != nil && *o.Rows[0][0] == "0" {
			break
		}
		select {
		case <-deadline.Done():
			return Outcome{}, fmt.Errorf("session %s's old backend didn't exit", session)
		case <-time.After(10 * time.Millisecond):
		}
	}
	check, err := p.must(ctx, c, "SELECT active_branch()")
	if err != nil {
		return Outcome{}, err
	}
	if len(check.Rows) != 1 || len(check.Rows[0]) != 1 || check.Rows[0][0] == nil || *check.Rows[0][0] != branch {
		return Outcome{}, fmt.Errorf("connection did not select branch %s", branch)
	}
	return Outcome{Rows: [][]*string{}, Tag: "connected to " + db, Transaction: string(c.TxStatus())}, nil
}

// The longest database name a startup packet keeps (NAMEDATALEN - 1): the
// server cuts a longer one short, silently, naming another database.
const maxDatabaseName = 63

// The branch a connection to the database alone starts on.
const defaultBranch = "main"

// connectTarget is the database a connect step names, the case's db or,
// given a branch, "db/branch", and the branch the connection must be on.
func connectTarget(db string, args []*string) (target, branch string, err error) {
	target, branch = db, defaultBranch
	if len(args) == 1 {
		branch = *args[0]
		target += "/" + branch
	}
	if len(target) > maxDatabaseName {
		return "", "", fmt.Errorf("database name %q is longer than %d bytes", target, maxDatabaseName)
	}
	return target, branch, nil
}

// branchRefusal is the SQLSTATE of a connection refused because the branch
// it named doesn't exist, or "" for any other failure (too many
// connections, authentication, the case's database gone): those are the
// target's, not the case's.
func branchRefusal(err error, named bool) string {
	var pgerr *pgconn.PgError
	if named && errors.As(err, &pgerr) && pgerr.Severity == "FATAL" && pgerr.Code == "42704" {
		return pgerr.Code
	}
	return ""
}

// replace makes c session's connection in place of the one it has, closing
// that. If closing it fails the session has neither: the old connection is
// closed regardless, and c is closed with it.
func replace[C interface{ Close(context.Context) error }](ctx context.Context, sessions map[string]C, session string, c C) error {
	if err := sessions[session].Close(ctx); err != nil {
		delete(sessions, session)
		return errors.Join(err, c.Close(ctx))
	}
	sessions[session] = c
	return nil
}

// "$1::text, ..., $n::text": Dolt's arguments, for a variadic text call.
func placeholders(n int) string {
	parts := make([]string, n)
	for i := range parts {
		parts[i] = fmt.Sprintf("$%d::text", i+1)
	}
	return strings.Join(parts, ", ")
}

func (p *Postgres) CheckTarget(ctx context.Context, session string) error {
	c := p.sessions[session]
	if c.IsClosed() {
		return fmt.Errorf("session connection closed")
	}
	// Catalog reads cannot run in an aborted transaction. Recheck on rollback.
	if c.TxStatus() == 'E' {
		return nil
	}
	am := p.Name()
	o, err := p.must(ctx, c, `SELECT count(*) FROM pg_class c JOIN pg_am a ON a.oid=c.relam
WHERE c.relnamespace='public'::regnamespace AND c.relkind IN ('r','m') AND a.amname<>$1`, &am)
	if err != nil {
		return err
	}
	if len(o.Rows) != 1 || len(o.Rows[0]) != 1 || o.Rows[0][0] == nil || *o.Rows[0][0] != "0" {
		return fmt.Errorf("public contains a table using the wrong AM (expected %s)", am)
	}
	return nil
}

func (p *Postgres) Close(ctx context.Context) error {
	var errs []error
	for _, c := range p.sessions {
		deadline, cancel := context.WithTimeout(ctx, p.timeout)
		errs = append(errs, c.Close(deadline))
		cancel()
	}
	p.sessions = nil
	if p.admin != nil {
		if p.db != "" {
			_, err := p.must(ctx, p.admin, "DROP DATABASE IF EXISTS "+p.db+" WITH (FORCE)")
			errs = append(errs, err)
		}
		deadline, cancel := context.WithTimeout(ctx, p.timeout)
		errs = append(errs, p.admin.Close(deadline))
		cancel()
		p.admin = nil
		p.db = ""
	}
	return errors.Join(errs...)
}
