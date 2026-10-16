package main

import (
	"bufio"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/dolthub/sqllogictest/go/logictest"
	"github.com/dolthub/sqllogictest/go/logictest/parser"
	"github.com/jackc/pgx/v5/pgconn"
	"github.com/jackc/pgx/v5/pgconn/ctxwatch"
)

// The engine string the corpus's skipif/onlyif directives name.
const engineStr = "postgresql"

// How long to wait for the server to accept connections: briefly in
// general, longer after a crash, while crash recovery replays WAL.
const (
	connectTimeout   = 30 * time.Second
	reconnectTimeout = 5 * time.Minute
)

// Events recorded with a digest.
const (
	evCrash      = "crash"       // the server restarted while this session ran the record
	evLost       = "lost"        // the session died without a server restart
	evAfterCrash = "after_crash" // not run: the session was lost earlier in the file
	evInitFailed = "init_failed" // not run: the file's database couldn't be set up
)

// digest is what the harness saw for one executed record: the actual
// result, summarized, or the error's SQLSTATE.  Records are identified by
// file and line, the same for every engine.
type digest struct {
	Version  int    `json:"version"`
	SortMode string `json:"sort_mode,omitempty"`
	File     string `json:"f"`
	Line     int    `json:"l"`
	Rows     int    `json:"n,omitempty"`
	Ordered  string `json:"o,omitempty"` // hash of the values in returned order
	Sorted   string `json:"s,omitempty"` // hash of the rows, sorted
	SQLState string `json:"e,omitempty"`
	Message  string `json:"m,omitempty"`
	Event    string `json:"ev,omitempty"`
	PID      uint32 `json:"pid,omitempty"`
}

// fileResult is written once per test file, after it ran.
type fileResult struct {
	File    string   `json:"file"`
	Tables  int      `json:"tables"`
	WrongAM []string `json:"wrong_am,omitempty"` // "table:am" for each table not on the AM under test
	Event   string   `json:"event,omitempty"`
	Ms      int64    `json:"ms"`
}

// harness runs one worker's files, one at a time, each in a fresh
// database, over a single session.
type harness struct {
	cfg    *config
	dbName string
	abs    map[string]string // file as logged -> path

	mu      sync.Mutex
	conn    *pgconn.PgConn
	started string // postmaster start time, to tell a restart

	file      string
	fileStart time.Time
	records   []*parser.Record
	next      int
	event     string // set when the file's session was lost

	digests *bufio.Writer
	files   *bufio.Writer
	closers []*os.File
}

var _ logictest.Harness = (*harness)(nil)

func newHarness(cfg *config, dbName string, files []string, digestPath, filesPath string) (*harness, error) {
	h := &harness{cfg: cfg, dbName: dbName, abs: map[string]string{}}
	for _, f := range files {
		h.abs[testFilePath(f)] = f
	}
	for _, p := range []string{digestPath, filesPath} {
		f, err := os.Create(p)
		if err != nil {
			return nil, err
		}
		h.closers = append(h.closers, f)
	}
	h.digests = bufio.NewWriterSize(h.closers[0], 1<<20)
	h.files = bufio.NewWriter(h.closers[1])
	t, err := h.postmasterStart(connectTimeout)
	if err != nil {
		return nil, err
	}
	h.started = t
	return h, nil
}

func (h *harness) EngineStr() string { return engineStr }
func (h *harness) GetTimeout() int64 { return h.cfg.timeout }

// Init finishes the previous file and sets up the next: the runner names
// the file before calling it.
func (h *harness) Init() error {
	h.mu.Lock()
	defer h.mu.Unlock()
	h.finishFile()

	h.file = logictest.GetCurrentFileName()
	h.fileStart = time.Now()
	h.event = ""
	h.next = 0
	recs, err := parser.ParseTestFile(h.abs[h.file])
	if err != nil {
		return err
	}
	h.records = recs

	if err := h.setupDatabase(); err != nil {
		fmt.Fprintf(os.Stderr, "%s: setting up %s: %v\n", h.file, h.dbName, err)
		h.event = evInitFailed
	}
	return nil
}

// setupDatabase recreates the worker's database and opens the session on
// it, with the target's settings.
func (h *harness) setupDatabase() error {
	ctx := context.Background()
	if h.conn != nil {
		h.conn.Close(ctx)
		h.conn = nil
	}
	m, err := connectRetry(h.cfg.maintDSN(), nil, connectTimeout)
	if err != nil {
		return err
	}
	defer m.Close(ctx)
	for _, sql := range []string{
		fmt.Sprintf("DROP DATABASE IF EXISTS %s WITH (FORCE)", h.dbName),
		fmt.Sprintf("CREATE DATABASE %s", h.dbName),
	} {
		if _, err := m.Exec(ctx, sql).ReadAll(); err != nil {
			return err
		}
	}
	conn, err := connectRetry(h.cfg.dbDSN(h.dbName), h.cfg.sessionParams(), connectTimeout)
	if err != nil {
		return err
	}
	if h.cfg.schema != "" {
		sql := fmt.Sprintf("CREATE SCHEMA %s VERSIONED; SET search_path = %s",
			h.cfg.schema, h.cfg.schema)
		if _, err := conn.Exec(ctx, sql).ReadAll(); err != nil {
			conn.Close(ctx)
			return err
		}
	}
	h.conn = conn
	return nil
}

// finishFile checks the AM of every table the file left behind and
// records the file.
func (h *harness) finishFile() {
	if h.file == "" {
		return
	}
	res := fileResult{File: h.file, Event: h.event,
		Ms: time.Since(h.fileStart).Milliseconds()}
	if h.event != evInitFailed {
		tables, wrong, err := h.checkAM()
		if err != nil {
			fmt.Fprintf(os.Stderr, "%s: checking table AMs: %v\n", h.file, err)
			wrong = append(wrong, "target-check-failed:"+err.Error())
		}
		res.Tables, res.WrongAM = tables, wrong
	}
	b, _ := json.Marshal(res)
	h.files.Write(append(b, '\n'))
	h.file = ""
}

func (h *harness) checkAM() (int, []string, error) {
	ctx := context.Background()
	conn, err := connectRetry(h.cfg.dbDSN(h.dbName), nil, connectTimeout)
	if err != nil {
		return 0, nil, err
	}
	defer conn.Close(ctx)
	rows, err := query(ctx, conn, amCheckSQL(h.cfg.testSchema()))
	if err != nil {
		return 0, nil, err
	}
	var wrong []string
	for _, r := range rows {
		if r[1] != h.cfg.am {
			wrong = append(wrong, r[0]+":"+r[1])
		}
	}
	return len(rows), wrong, nil
}

// amCheckSQL lists the tables and materialized views in schema with their
// table AM.
func amCheckSQL(schema string) string {
	return fmt.Sprintf(`SELECT c.relname, coalesce(a.amname, '')
		FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace
		LEFT JOIN pg_am a ON a.oid = c.relam
		WHERE n.nspname = '%s' AND c.relkind IN ('r', 'm') ORDER BY 1`, schema)
}

func (h *harness) Close() error {
	h.mu.Lock()
	defer h.mu.Unlock()
	h.finishFile()
	if h.conn != nil {
		h.conn.Close(context.Background())
	}
	h.digests.Flush()
	h.files.Flush()
	for _, f := range h.closers {
		f.Close()
	}
	return nil
}

// -- execution ----------------------------------------------------------

func (h *harness) ExecuteStatement(statement string) error {
	return h.ExecuteStatementContext(context.Background(), statement)
}

func (h *harness) ExecuteQuery(statement string) (string, []string, error) {
	return h.ExecuteQueryContext(context.Background(), statement)
}

func (h *harness) ExecuteStatementContext(ctx context.Context, statement string) error {
	h.mu.Lock()
	defer h.mu.Unlock()
	line := h.match(parser.Statement, statement)
	if h.event != "" {
		return h.notRun(line)
	}
	results, err := h.conn.Exec(ctx, statement).ReadAll()
	if err != nil {
		h.recordError(ctx, line, err)
	} else if ctx.Err() == nil {
		// Statements need evidence too: absence of an error log is not proof
		// of execution. Include every command tag (including affected rows).
		var tags []string
		for _, r := range results {
			tags = append(tags, r.CommandTag.String())
		}
		h.writeDigest(digest{Version: 2, File: h.file, Line: line,
			Ordered: hashValues(tags), Sorted: hashValues(tags)})
	}
	return err
}

func (h *harness) ExecuteQueryContext(ctx context.Context, statement string) (string, []string, error) {
	h.mu.Lock()
	defer h.mu.Unlock()
	line := h.match(parser.Query, statement)
	if h.event != "" {
		return "", nil, h.notRun(line)
	}
	results, err := h.conn.Exec(ctx, statement).ReadAll()
	if err != nil {
		h.recordError(ctx, line, err)
		return "", nil, err
	}
	// The result with columns; a statement has none.
	var res *pgconn.Result
	for _, r := range results {
		if len(r.FieldDescriptions) > 0 {
			res = r
		}
	}
	if res == nil && len(results) > 0 {
		res = results[len(results)-1]
	}
	if res == nil {
		res = &pgconn.Result{}
	}
	schema, values := formatResult(res)
	if ctx.Err() == nil {
		ordered, sorted := exactResult(res)
		mode := h.records[h.next-1].SortString()
		h.writeDigest(digest{Version: 2, File: h.file, Line: line, Rows: len(res.Rows),
			Ordered: ordered, Sorted: sorted, SortMode: mode})
	}
	return schema, values, nil
}

// match finds the record being executed: the next one of its type that
// runs on Postgres and has this text.
func (h *harness) match(typ parser.RecordType, sql string) int {
	for h.next < len(h.records) {
		r := h.records[h.next]
		h.next++
		if r.Type() == typ && r.ShouldExecuteForEngine(engineStr) && r.Query() == sql {
			return r.LineNum()
		}
	}
	return 0
}

func (h *harness) notRun(line int) error {
	ev := evAfterCrash
	if h.event == evInitFailed {
		ev = evInitFailed
	}
	h.writeDigest(digest{Version: 2, File: h.file, Line: line, Event: ev})
	return fmt.Errorf("not run: %s earlier in this file", h.event)
}

func (h *harness) recordError(ctx context.Context, line int, err error) {
	if ctx.Err() != nil {
		return // a timeout; the runner logs it
	}
	d := digest{Version: 2, File: h.file, Line: line}
	var pgErr *pgconn.PgError
	if errors.As(err, &pgErr) {
		d.SQLState = pgErr.Code
		d.Message = truncate(pgErr.Message, 300)
	} else {
		d.Message = truncate(err.Error(), 300)
	}
	if h.sessionLost(pgErr) {
		d.PID = h.conn.PID()
		d.Event = h.diagnoseLoss()
		h.event = d.Event
	}
	h.writeDigest(d)
}

// sessionLost reports whether the session can't go on: the connection
// closed, or the server ended it (SQLSTATE class 57P).
func (h *harness) sessionLost(pgErr *pgconn.PgError) bool {
	if h.conn.IsClosed() {
		return true
	}
	return pgErr != nil && strings.HasPrefix(pgErr.Code, "57P")
}

// diagnoseLoss waits for the server and tells a restart (a crash) from a
// session that died alone.
func (h *harness) diagnoseLoss() string {
	t, err := h.postmasterStart(reconnectTimeout)
	if err != nil || t != h.started {
		if err == nil {
			h.started = t
		}
		return evCrash
	}
	return evLost
}

func (h *harness) writeDigest(d digest) {
	b, _ := json.Marshal(d)
	h.digests.Write(append(b, '\n'))
}

// -- connections --------------------------------------------------------

func (h *harness) postmasterStart(wait time.Duration) (string, error) {
	ctx := context.Background()
	conn, err := connectRetry(h.cfg.maintDSN(), nil, wait)
	if err != nil {
		return "", err
	}
	defer conn.Close(ctx)
	rows, err := query(ctx, conn, "SELECT pg_postmaster_start_time()::text")
	if err != nil {
		return "", err
	}
	return rows[0][0], nil
}

// connectRetry connects, waiting up to wait while the server is starting
// or in recovery.
func connectRetry(dsn string, params map[string]string, wait time.Duration) (*pgconn.PgConn, error) {
	deadline := time.Now().Add(wait)
	for {
		conn, err := connect(dsn, params)
		if err == nil || time.Now().After(deadline) {
			return conn, err
		}
		time.Sleep(200 * time.Millisecond)
	}
}

func connect(dsn string, params map[string]string) (*pgconn.PgConn, error) {
	cfg, err := pgconn.ParseConfig(dsn)
	if err != nil {
		return nil, err
	}
	for k, v := range params {
		cfg.RuntimeParams[k] = v
	}
	// On a timeout, cancel the query on the server, and give up on the
	// connection only if that doesn't end it.
	cfg.BuildContextWatcherHandler = func(c *pgconn.PgConn) ctxwatch.Handler {
		return &pgconn.CancelRequestContextWatcherHandler{Conn: c, DeadlineDelay: 10 * time.Second}
	}
	return pgconn.ConnectConfig(context.Background(), cfg)
}

// query runs sql and returns its rows as text.
func query(ctx context.Context, conn *pgconn.PgConn, sql string) ([][]string, error) {
	results, err := conn.Exec(ctx, sql).ReadAll()
	if err != nil {
		return nil, err
	}
	var out [][]string
	for _, r := range results {
		for _, row := range r.Rows {
			vals := make([]string, len(row))
			for i, v := range row {
				vals[i] = string(v)
			}
			out = append(out, vals)
		}
	}
	return out, nil
}

// -- result formatting --------------------------------------------------

// Type OIDs (pg_type.dat).
const (
	oidBool    = 16
	oidInt8    = 20
	oidInt2    = 21
	oidInt4    = 23
	oidOid     = 26
	oidFloat4  = 700
	oidFloat8  = 701
	oidUnknown = 705
	oidNumeric = 1700
)

// formatResult renders a result the way the corpus expects: one schema
// letter per column (I integer, R floating point, T text) and one value
// per column of each row; integers as %d, floating point as %.3f, NULL as
// "NULL", booleans as 1 or 0.  Types the corpus doesn't use are text.
func formatResult(res *pgconn.Result) (string, []string) {
	var schema strings.Builder
	for _, fd := range res.FieldDescriptions {
		switch fd.DataTypeOID {
		case oidBool, oidInt2, oidInt4, oidInt8, oidOid, oidUnknown:
			schema.WriteByte('I')
		case oidFloat4, oidFloat8, oidNumeric:
			schema.WriteByte('R')
		default:
			schema.WriteByte('T')
		}
	}
	s := schema.String()
	values := make([]string, 0, len(res.Rows)*len(s))
	for _, row := range res.Rows {
		for i, v := range row {
			values = append(values, formatValue(res.FieldDescriptions[i].DataTypeOID, v))
		}
	}
	return s, values
}

func formatValue(oid uint32, v []byte) string {
	if v == nil {
		return "NULL"
	}
	switch oid {
	case oidBool:
		if string(v) == "t" {
			return "1"
		}
		return "0"
	case oidFloat4, oidFloat8, oidNumeric:
		f, err := strconv.ParseFloat(string(v), 64)
		if err != nil {
			return string(v)
		}
		return fmt.Sprintf("%.3f", f)
	}
	return string(v)
}

func hashValues(values []string) string {
	b, _ := json.Marshal(values)
	sum := sha256.Sum256(b)
	return hex.EncodeToString(sum[:])
}

// exactResult is deliberately separate from corpus formatting. JSON frames
// preserve NULL versus empty/text, cell/row boundaries, all bytes and types.
// Sorting whole rows preserves duplicates and column associations.
func exactResult(res *pgconn.Result) (string, string) {
	types := make([]uint32, len(res.FieldDescriptions))
	for i, fd := range res.FieldDescriptions {
		types[i] = fd.DataTypeOID
	}
	t, _ := json.Marshal(types)
	rows := make([]string, 0, len(res.Rows))
	for _, row := range res.Rows {
		b, _ := json.Marshal(row)
		rows = append(rows, string(b))
	}
	ordered := hashValues(append([]string{string(t)}, rows...))
	sort.Strings(rows)
	return ordered, hashValues(append([]string{string(t)}, rows...))
}

// hashSortedRows hashes the rows in sorted order, so two results with the
// same rows in different orders hash the same.
func hashSortedRows(values []string, cols int) string {
	if cols == 0 {
		return hashValues(nil)
	}
	rows := make([]string, 0, len(values)/cols)
	for i := 0; i+cols <= len(values); i += cols {
		b, _ := json.Marshal(values[i : i+cols])
		rows = append(rows, string(b))
	}
	sort.Strings(rows)
	return hashValues(rows)
}

func truncate(s string, n int) string {
	if len(s) <= n {
		return s
	}
	return s[:n]
}
