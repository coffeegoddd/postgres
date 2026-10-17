// The keyless benchmark's runner: it runs a script of timed SQL steps on one connection.
//
// Script: lines "@step NAME [REPS]" start a timed step; "@pre" starts
// SQL run untimed before each rep; "@setup" starts untimed SQL run once;
// other lines are SQL, statements ending with ";" at end of line.  "{i}"
// is replaced by the rep number (0-based).  Output: NAME<TAB>ms per rep.
package main

import (
	"bufio"
	"context"
	"database/sql"
	"fmt"
	"os"
	"sort"
	"strconv"
	"strings"
	"time"

	_ "github.com/go-sql-driver/mysql"
	_ "github.com/jackc/pgx/v5/stdlib"
)

type step struct {
	name  string
	reps  int
	pre   []string
	body  []string
	setup bool
}

// fail reports a malformed script and exits.
func fail(path string, line int, format string, args ...any) {
	fmt.Fprintf(os.Stderr, "%s:%d: %s\n", path, line, fmt.Sprintf(format, args...))
	os.Exit(2)
}

func parse(path string) []step {
	f, err := os.Open(path)
	if err != nil {
		panic(err)
	}
	defer f.Close()
	var steps []step
	var cur *step
	inPre := false
	var buf strings.Builder
	sc := bufio.NewScanner(f)
	sc.Buffer(make([]byte, 1<<20), 1<<24)
	n := 0
	flush := func(s string) {
		if cur == nil {
			fail(path, n, "SQL before the first @step or @setup")
		}
		if inPre {
			cur.pre = append(cur.pre, s)
		} else {
			cur.body = append(cur.body, s)
		}
	}
	for sc.Scan() {
		n++
		line := sc.Text()
		t := strings.TrimSpace(line)
		switch {
		case strings.HasPrefix(t, "@step "):
			f := strings.Fields(t)
			reps := 1
			if len(f) > 2 {
				var err error
				if reps, err = strconv.Atoi(f[2]); err != nil || reps < 1 {
					fail(path, n, "repetitions must be a whole number above 0: %q", f[2])
				}
			}
			steps = append(steps, step{name: f[1], reps: reps})
			cur, inPre = &steps[len(steps)-1], false
		case t == "@setup":
			steps = append(steps, step{name: "setup", reps: 1, setup: true})
			cur, inPre = &steps[len(steps)-1], false
		case t == "@pre":
			inPre = true
		case t == "@body":
			inPre = false
		case t == "" || strings.HasPrefix(t, "--"):
		default:
			buf.WriteString(line)
			buf.WriteString("\n")
			if strings.HasSuffix(t, ";") {
				flush(strings.TrimSuffix(strings.TrimSpace(buf.String()), ";"))
				buf.Reset()
			}
		}
	}
	if err := sc.Err(); err != nil {
		fail(path, n, "%v", err)
	}
	if strings.TrimSpace(buf.String()) != "" {
		fail(path, n, "a statement doesn't end with \";\" at the end of a line")
	}
	return steps
}

// median of sorted values: the middle one, or the mean of the middle two.
func median(sorted []float64) float64 {
	m := len(sorted) / 2
	if len(sorted)%2 == 0 {
		return (sorted[m-1] + sorted[m]) / 2
	}
	return sorted[m]
}

func run(ctx context.Context, c *sql.Conn, q string) error {
	rows, err := c.QueryContext(ctx, q)
	if err != nil {
		return err
	}
	for rows.Next() {
	}
	err = rows.Err()
	rows.Close()
	return err
}

func main() {
	driver, dsn, script := os.Args[1], os.Args[2], os.Args[3]
	steps := parse(script) // (all of it first: a bad script runs nothing)
	db, err := sql.Open(driver, dsn)
	if err != nil {
		panic(err)
	}
	ctx := context.Background()
	c, err := db.Conn(ctx)
	if err != nil {
		panic(err)
	}
	for _, s := range steps {
		var ms []float64
		for i := 0; i < s.reps; i++ {
			sub := func(q string) string { return strings.ReplaceAll(q, "{i}", strconv.Itoa(i)) }
			for _, q := range s.pre {
				if err := run(ctx, c, sub(q)); err != nil {
					fmt.Fprintf(os.Stderr, "%s pre: %v\n  %s\n", s.name, err, sub(q))
					os.Exit(1)
				}
			}
			t0 := time.Now()
			for _, q := range s.body {
				if err := run(ctx, c, sub(q)); err != nil {
					fmt.Fprintf(os.Stderr, "%s: %v\n  %s\n", s.name, err, sub(q))
					os.Exit(1)
				}
			}
			ms = append(ms, float64(time.Since(t0).Microseconds())/1000)
		}
		if s.setup {
			continue
		}
		sorted := append([]float64(nil), ms...)
		sort.Float64s(sorted)
		parts := make([]string, len(ms))
		for i, v := range ms {
			parts[i] = strconv.FormatFloat(v, 'f', 2, 64)
		}
		fmt.Printf("%s\t%.2f\t%s\n", s.name, median(sorted), strings.Join(parts, ","))
	}
}
