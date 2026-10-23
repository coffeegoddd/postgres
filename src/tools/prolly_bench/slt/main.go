// slt-runner runs sqllogictest files against a Postgres server, with every
// table the corpus creates on one table access method, and writes a record
// per test with what the server actually returned.
//
//	slt-runner -dsn 'host=/tmp port=5432 user=postgres' \
//	    -am prolly -schema slt -workers 8 -out DIR  FILE_OR_DIR...
//
// Each file runs in a child process of its own, in a fresh database, over
// one session; up to -workers at a time, largest files first, each worker
// taking the next file when it finishes one.  Processes rather than
// goroutines, because the reference runner keeps the current file and
// record in globals.  Output in DIR:
//
//	records.jsonl.gz  one line per record (see merge.go)
//	files.jsonl       one line per file: tables left, any on the wrong AM
//	summary.json      counts by status
//	failures.log.gz   the reference runner's log, for records that didn't pass
//
// The exit status is 0 when every file ran with its tables on the AM under
// test, 3 when a table was on another AM, and 1 on any other failure.
//
//	slt-runner compare BASELINE_DIR ENGINE_DIR OUT_DIR [EXCEPTIONS_JSON]
//
// compares two runs' records (compare.go).
package main

import (
	"bufio"
	"context"
	"flag"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"time"

	"github.com/dolthub/sqllogictest/go/logictest"
)

type config struct {
	dsn    string // the server, without a database
	am     string
	schema string // a versioned schema to create the tables in, or ""
	// Schemas searched after the test schema (an extension's, say), and a
	// file of SQL run in each new database before its tests: spatial
	// files need PostGIS created first, and its schema on the path.
	searchPathExtra string
	setupSQL        string
	workers         int
	timeout         int64
	out             string
	scratch         string
	serverLog       string
	dbPrefix        string
}

// maintDSN connects to database postgres, to create and drop the others.
func (c *config) maintDSN() string { return c.dbDSN("postgres") }

func (c *config) dbDSN(name string) string {
	return c.dsn + " dbname=" + name
}

// sessionParams are the settings every test session starts with.
func (c *config) sessionParams() map[string]string {
	return map[string]string{"default_table_access_method": c.am}
}

// searchPath is every session's search_path: the test schema, then the
// extra schemas.
func (c *config) searchPath() string {
	path := []string{c.testSchema()}
	for _, s := range strings.Split(c.searchPathExtra, ",") {
		if s = strings.TrimSpace(s); s != "" {
			path = append(path, s)
		}
	}
	return strings.Join(path, ", ")
}

// testSchema is where the corpus's tables land.
func (c *config) testSchema() string {
	if c.schema != "" {
		return c.schema
	}
	return "public"
}

func main() {
	if len(os.Args) > 1 && os.Args[1] == "compare" {
		os.Exit(runCompare(os.Args[2:]))
	}
	cfg := &config{}
	flag.StringVar(&cfg.dsn, "dsn", "", "libpq connection string (keyword form) of the server, without dbname")
	flag.StringVar(&cfg.am, "am", "heap", "table access method under test")
	flag.StringVar(&cfg.schema, "schema", "", "create a versioned schema of this name and run the tests in it")
	flag.StringVar(&cfg.searchPathExtra, "search-path-extra", "",
		"comma-separated schemas to search after the test schema")
	flag.StringVar(&cfg.setupSQL, "setup-sql", "",
		"file of SQL to run in each new database before its tests (on the search path)")
	flag.IntVar(&cfg.workers, "workers", 1, "worker processes")
	flag.Int64Var(&cfg.timeout, "timeout", 30, "per-record timeout, seconds; a timeout ends its file")
	flag.StringVar(&cfg.out, "out", "", "output directory")
	flag.StringVar(&cfg.scratch, "scratch", "", "directory for worker output (default OUT/scratch)")
	flag.StringVar(&cfg.serverLog, "server-log", "", "the server's log, to tell which backend crashed")
	flag.StringVar(&cfg.dbPrefix, "db-prefix", "slt", "prefix of the databases the workers create")
	keepScratch := flag.Bool("keep-scratch", false, "keep the per-file output the summary is merged from")
	child := flag.String("child", "", "internal: run as unit NAME")
	dbName := flag.String("db", "", "internal: the unit's database")
	fileList := flag.String("files", "", "internal: the unit's file list")
	flag.Parse()

	if cfg.dsn == "" || cfg.out == "" {
		fmt.Fprintln(os.Stderr, "usage: slt-runner -dsn DSN -out DIR [flags] FILE_OR_DIR...")
		os.Exit(1)
	}
	if strings.Contains(cfg.dsn, "dbname=") {
		fatal(fmt.Errorf("-dsn must not name a database"))
	}
	if cfg.scratch == "" {
		cfg.scratch = filepath.Join(cfg.out, "scratch")
	}
	if cfg.setupSQL != "" {
		abs, err := filepath.Abs(cfg.setupSQL)
		if err != nil {
			fatal(err)
		}
		cfg.setupSQL = abs
	}

	if *child != "" {
		if err := runChild(cfg, *child, *dbName, *fileList); err != nil {
			fatal(err)
		}
		return
	}
	code := runParent(cfg, flag.Args())
	if !*keepScratch && code == 0 {
		os.RemoveAll(cfg.scratch)
	}
	os.Exit(code)
}

func fatal(err error) {
	fmt.Fprintln(os.Stderr, "slt-runner:", err)
	os.Exit(1)
}

func runChild(cfg *config, name, dbName, fileList string) error {
	files, err := readLines(fileList)
	if err != nil {
		return err
	}
	h, err := newHarness(cfg, dbName, files,
		filepath.Join(cfg.scratch, name+".digests.jsonl"),
		filepath.Join(cfg.scratch, name+".files.jsonl"))
	if err != nil {
		return err
	}
	logictest.RunTestFiles(h, files...)
	return h.Close()
}

func runParent(cfg *config, paths []string) int {
	for _, d := range []string{cfg.out, cfg.scratch} {
		if err := os.MkdirAll(d, 0o755); err != nil {
			fatal(err)
		}
	}
	files, err := collectFiles(paths)
	if err != nil {
		fatal(err)
	}
	if len(files) == 0 {
		fatal(fmt.Errorf("no .test files in %v", paths))
	}
	if err := writePlan(cfg, files); err != nil {
		fatal(err)
	}
	if err := canary(cfg); err != nil {
		fmt.Fprintln(os.Stderr, "slt-runner: canary:", err)
		return 3
	}

	// Largest first, so the run doesn't end waiting on one big file.
	sortBySize(files)
	units := make([]string, len(files))
	errs := make([]error, len(files))
	slots := make(chan int, max(cfg.workers, 1))
	for i := 0; i < cap(slots); i++ {
		slots <- i
	}
	var wg sync.WaitGroup
	var mu sync.Mutex
	done := 0
	for i, f := range files {
		units[i] = fmt.Sprintf("f%04d", i)
		slot := <-slots
		wg.Add(1)
		go func(i, slot int, f string) {
			defer wg.Done()
			defer func() { slots <- slot }()
			errs[i] = spawn(cfg, units[i], fmt.Sprintf("%s_%d", cfg.dbPrefix, slot), []string{f})
			mu.Lock()
			done++
			if done%25 == 0 || done == len(files) {
				fmt.Fprintf(os.Stderr, "slt-runner: %d/%d files\n", done, len(files))
			}
			mu.Unlock()
		}(i, slot, f)
	}
	wg.Wait()
	for _, err := range errs {
		if err != nil {
			fatal(err)
		}
	}

	// Files whose session died because another backend crashed are run
	// again, once, on their own.
	crashed, err := crashedPIDs(cfg.serverLog)
	if err != nil {
		fatal(err)
	}
	rerun, err := collateralFiles(cfg.scratch, units, crashed, cfg.serverLog != "")
	if err != nil {
		fatal(err)
	}
	if len(rerun) > 0 {
		fmt.Fprintf(os.Stderr, "slt-runner: running %d files again after a crash elsewhere\n", len(rerun))
		for i, f := range files {
			if rerun[testFilePath(f)] {
				units[i] += "-rerun"
				if err := spawn(cfg, units[i], cfg.dbPrefix+"_rerun", []string{f}); err != nil {
					fatal(err)
				}
			}
		}
	}

	t0 := time.Now()
	sum, err := merge(cfg, units, rerun)
	if err != nil {
		fatal(err)
	}
	fmt.Fprintf(os.Stderr, "slt-runner: merged in %.1fs\n", time.Since(t0).Seconds())
	fmt.Fprintf(os.Stderr, "slt-runner: %d records, %d ok (%.3f%%), %d files with tables on the wrong AM\n",
		sum.Total, sum.Counts[stOk], sum.PassPercent, len(sum.WrongAMFiles))
	if len(sum.WrongAMFiles) > 0 {
		return 3
	}
	return 0
}

// spawn runs a unit's files in a child process and waits for it.
func spawn(cfg *config, name, dbName string, files []string) error {
	list := filepath.Join(cfg.scratch, name+".files")
	if err := os.WriteFile(list, []byte(strings.Join(files, "\n")+"\n"), 0o644); err != nil {
		return err
	}
	logf, err := os.Create(filepath.Join(cfg.scratch, name+".log"))
	if err != nil {
		return err
	}
	defer logf.Close()
	args := []string{"-child", name, "-db", dbName, "-files", list,
		"-dsn", cfg.dsn,
		"-am", cfg.am, "-schema", cfg.schema, "-timeout", fmt.Sprint(cfg.timeout),
		"-search-path-extra", cfg.searchPathExtra, "-setup-sql", cfg.setupSQL,
		"-out", cfg.out, "-scratch", cfg.scratch, "-db-prefix", cfg.dbPrefix}
	cmd := exec.Command(os.Args[0], args...)
	cmd.Stdout = logf
	cmd.Stderr = os.Stderr
	if err := cmd.Run(); err != nil {
		return fmt.Errorf("%s (%s): %w", name, strings.Join(files, " "), err)
	}
	return nil
}

// canary creates a table the way the tests will and checks its AM, so a
// target that silently lands on another AM fails before any file runs.
func canary(cfg *config) error {
	h := &harness{cfg: cfg, dbName: cfg.dbPrefix + "_canary"}
	if err := h.setupDatabase(); err != nil {
		return err
	}
	ctx := context.Background()
	defer h.conn.Close(ctx)
	if _, err := h.conn.Exec(ctx, "CREATE TABLE canary (id int PRIMARY KEY, payload text)").ReadAll(); err != nil {
		return err
	}
	rows, err := query(ctx, h.conn, amCheckSQL(cfg.testSchema()))
	if err != nil {
		return err
	}
	if len(rows) != 1 || rows[0][1] != cfg.am {
		return fmt.Errorf("a table created in %s is on %v, not %s", cfg.testSchema(), rows, cfg.am)
	}
	if cfg.am == "prolly" {
		if _, err := h.conn.Exec(ctx, `DO $$ BEGIN
IF (SELECT count(*) FROM pg_class WHERE
 (oid = 'canary'::regclass OR oid = 'canary_pkey'::regclass OR
 oid = (SELECT reltoastrelid FROM pg_class WHERE oid = 'canary'::regclass))
 AND (pg_stat_file(pg_relation_filepath(oid) || '_bmap', true)).size IS NOT NULL) <> 3
THEN RAISE EXCEPTION 'table, index or TOAST is not block-mapped'; END IF; END $$`).ReadAll(); err != nil {
			return err
		}
	}
	h.conn.Close(ctx)
	m, err := connect(cfg.maintDSN(), nil)
	if err != nil {
		return err
	}
	defer m.Close(ctx)
	_, err = m.Exec(ctx, "DROP DATABASE "+h.dbName).ReadAll()
	return err
}

// collectFiles finds the .test files under paths, as absolute paths.
func collectFiles(paths []string) ([]string, error) {
	var out []string
	for _, p := range paths {
		err := filepath.Walk(p, func(path string, info os.FileInfo, err error) error {
			if err != nil {
				return err
			}
			if !info.IsDir() && strings.HasSuffix(path, ".test") {
				abs, err := filepath.Abs(path)
				if err != nil {
					return err
				}
				out = append(out, abs)
			}
			return nil
		})
		if err != nil {
			return nil, err
		}
	}
	sort.Strings(out)
	return out, nil
}

// sortBySize sorts files largest first.
func sortBySize(files []string) {
	size := map[string]int64{}
	for _, f := range files {
		if st, err := os.Stat(f); err == nil {
			size[f] = st.Size()
		}
	}
	sort.SliceStable(files, func(i, j int) bool { return size[files[i]] > size[files[j]] })
}

// testFilePath is a test file's name as the reference runner logs it: the
// path below the corpus's test/ directory (at most four elements).
func testFilePath(f string) string {
	var elems []string
	name := f
	for len(elems) < 4 && len(name) > 0 {
		dir, file := filepath.Split(name)
		if file == "test" {
			break
		}
		elems = append([]string{file}, elems...)
		name = filepath.Clean(dir)
		if name == "/" || name == "." {
			break
		}
	}
	return strings.ReplaceAll(filepath.Join(elems...), "\\", "/")
}

func readLines(path string) ([]string, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	var out []string
	sc := bufio.NewScanner(f)
	for sc.Scan() {
		if line := strings.TrimSpace(sc.Text()); line != "" {
			out = append(out, line)
		}
	}
	return out, sc.Err()
}
