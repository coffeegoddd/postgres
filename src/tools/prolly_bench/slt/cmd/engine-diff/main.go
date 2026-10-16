// engine-diff runs generated transaction histories against heap and a target AM.
package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"

	"prolly_bench/slt/internal/enginediff"
)

func writeJSON(path string, value any) error {
	b, err := json.MarshalIndent(value, "", "  ")
	if err != nil {
		return err
	}
	return os.WriteFile(path, append(b, '\n'), 0644)
}

func run() error {
	dsn := flag.String("dsn", "", "server connection string; creates fresh temporary databases")
	am := flag.String("am", "prolly", "target table AM")
	seed := flag.Int64("seed", 1, "first seed")
	seeds := flag.Int("seeds", 10, "number of consecutive seeds")
	steps := flag.Int("steps", 28, "transaction groups per seed")
	out := flag.String("out", "", "new artifact directory")
	replay := flag.String("replay", "", "replay a saved program.json instead of generating")
	reduce := flag.Int("reduce", 0, "maximum replay attempts to reduce a mismatch")
	timeout := flag.Duration("timeout", 30*time.Second, "timeout per statement")
	selfTest := flag.Bool("self-test", false, "prove the comparator rejects a known mismatch")
	flag.Parse()
	if *dsn == "" || *out == "" || *seeds < 1 || *steps < 1 || *timeout <= 0 || *reduce < 0 {
		return fmt.Errorf("requires -dsn, -out, positive -seeds/-steps/-timeout and nonnegative -reduce")
	}
	if err := os.Mkdir(*out, 0755); err != nil {
		return err
	}
	if *selfTest {
		p := enginediff.Generate(0, 1)
		p.Groups = []enginediff.Group{{Name: "known_mismatch", SQL: []string{
			"SELECT current_setting('default_table_access_method')",
		}}}
		target := *am
		if target == "heap" {
			target = "prolly"
		}
		f, err := enginediff.Compare(*dsn, target, p, *timeout)
		if err != nil {
			return fmt.Errorf("harness self-test: %w", err)
		}
		if f == nil || f.Group != "known_mismatch" {
			return fmt.Errorf("harness accepted a known result mismatch")
		}
		if err := writeJSON(filepath.Join(*out, "self-test.json"), f); err != nil {
			return err
		}
		fmt.Println("harness self-test: rejected known mismatch")
		p.Groups = []enginediff.Group{{Name: "unexpected_error", SQL: []string{"SELECT missing_column FROM t"}}}
		f, err = enginediff.Compare(*dsn, target, p, *timeout)
		if f != nil || err == nil || !strings.Contains(err.Error(), "unexpected baseline SQLSTATE") {
			return fmt.Errorf("harness did not reject matching unexpected errors: %v", err)
		}
		fmt.Println("harness self-test: rejected matching unexpected errors")
	}
	var programs []enginediff.Program
	if *replay != "" {
		b, err := os.ReadFile(*replay)
		if err != nil {
			return err
		}
		var p enginediff.Program
		if err := json.Unmarshal(b, &p); err != nil {
			return err
		}
		programs = append(programs, p)
	} else {
		for i := 0; i < *seeds; i++ {
			programs = append(programs, enginediff.Generate(*seed+int64(i), *steps))
		}
	}
	completed := 0
	// Persist every requested seed and family before the first execution.
	// A process crash must leave a denominator, including work not yet run.
	if err := writeJSON(filepath.Join(*out, "planned.json"), programs); err != nil {
		return err
	}
	for _, p := range programs {
		dir := filepath.Join(*out, fmt.Sprintf("seed-%d", p.Seed))
		if err := os.Mkdir(dir, 0755); err != nil {
			return err
		}
		if err := writeJSON(filepath.Join(dir, "program.json"), p); err != nil {
			return err
		}
		f, err := enginediff.Compare(*dsn, *am, p, *timeout)
		status := "passed"
		if f != nil {
			status = "mismatch"
		}
		if err != nil {
			status = "error"
		}
		detail := ""
		if err != nil {
			detail = err.Error()
		}
		if e := writeJSON(filepath.Join(dir, "result.json"), map[string]any{
			"status": status, "engine": *am, "seed": p.Seed, "failure": f, "error": detail,
		}); e != nil {
			return e
		}
		fmt.Printf("seed %d: %s\n", p.Seed, status)
		if f != nil || err != nil {
			if f != nil && *reduce > 0 {
				signature := f.Signature()
				p = enginediff.Reduce(p, *reduce, func(trial enginediff.Program) bool {
					got, e := enginediff.Compare(*dsn, *am, trial, *timeout)
					return e == nil && got != nil && got.Signature() == signature
				})
				if e := writeJSON(filepath.Join(dir, "reduced.json"), p); e != nil {
					return e
				}
			}
			if e := writeJSON(filepath.Join(*out, "summary.json"), map[string]any{
				"planned": len(programs), "passed": completed, "failed": 1, "did_not_run": len(programs) - completed - 1,
			}); e != nil {
				return e
			}
			return fmt.Errorf("%s: see %s/result.json", status, dir)
		}
		completed++
	}
	return writeJSON(filepath.Join(*out, "summary.json"), map[string]any{
		"planned": len(programs), "passed": completed, "failed": 0, "did_not_run": 0,
	})
}

func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, "engine-diff:", err)
		os.Exit(1)
	}
}
