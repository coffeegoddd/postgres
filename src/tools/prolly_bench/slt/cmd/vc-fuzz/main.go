// vc-fuzz runs seeded version-control histories against prolly and checks
// them against a reference model, reducing a failing history to a smaller
// one that fails the same way.
package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"time"

	"prolly_bench/slt/internal/vcfuzz"
)

func writeJSON(path string, value any) error {
	b, err := json.MarshalIndent(value, "", "  ")
	if err != nil {
		return err
	}
	return os.WriteFile(path, append(b, '\n'), 0644)
}

func readHistories(paths []string) ([]vcfuzz.History, error) {
	var hs []vcfuzz.History
	for _, p := range paths {
		b, err := os.ReadFile(p)
		if err != nil {
			return nil, err
		}
		var h vcfuzz.History
		if err := json.Unmarshal(b, &h); err != nil {
			return nil, fmt.Errorf("%s: %w", p, err)
		}
		if err := h.Validate(); err != nil {
			return nil, fmt.Errorf("%s: %w", p, err)
		}
		hs = append(hs, h)
	}
	return hs, nil
}

func run() error {
	dsn := flag.String("dsn", "", "server connection string; creates a fresh database per history")
	seed := flag.Int64("seed", 1, "first seed")
	seeds := flag.Int("seeds", 10, "number of consecutive seeds")
	steps := flag.Int("steps", 60, "ops per history")
	sessions := flag.Int("sessions", 0, "sessions per history; 0 varies them, 1 to 3, by seed")
	out := flag.String("out", "", "new artifact directory")
	replay := flag.String("replay", "", "replay saved histories (a glob) instead of generating")
	reduce := flag.Int("reduce", 0, "maximum runs to reduce a failing history")
	timeout := flag.Duration("timeout", 30*time.Second, "timeout per statement")
	self := flag.Bool("self-test", false, "first prove the checks reject engines wrong in known ways")
	flag.Parse()
	if *dsn == "" || *out == "" || *seeds < 1 || *steps < 1 || *timeout <= 0 || *reduce < 0 || *sessions < 0 {
		return fmt.Errorf("requires -dsn, -out, positive -seeds/-steps/-timeout and nonnegative -reduce/-sessions")
	}
	if err := os.Mkdir(*out, 0755); err != nil {
		return err
	}
	var histories []vcfuzz.History
	if *replay != "" {
		paths, err := filepath.Glob(*replay)
		if err != nil {
			return err
		}
		if len(paths) == 0 {
			return fmt.Errorf("no histories match %s", *replay)
		}
		if histories, err = readHistories(paths); err != nil {
			return err
		}
	} else {
		for i := 0; i < *seeds; i++ {
			s := *seed + int64(i)
			n := *sessions
			if n == 0 {
				n = 1 + int(uint64(s)%3)
			}
			histories = append(histories, vcfuzz.Generate(s, *steps, n))
		}
	}
	// Persist every planned history before the first runs, so a crash
	// leaves the denominator.
	if err := writeJSON(filepath.Join(*out, "planned.json"), histories); err != nil {
		return err
	}
	open := vcfuzz.OpenPostgres(*dsn, *timeout)
	if *self {
		if err := selfTest(open); err != nil {
			return fmt.Errorf("self-test: %w", err)
		}
	}
	passed := 0
	outcomes := map[string]int{}
	for i, h := range histories {
		dir := filepath.Join(*out, fmt.Sprintf("history-%d-seed-%d", i, h.Seed))
		if err := os.Mkdir(dir, 0755); err != nil {
			return err
		}
		if err := writeJSON(filepath.Join(dir, "history.json"), h); err != nil {
			return err
		}
		report, err := vcfuzz.Run(h, open)
		f := report.Failure
		for k, n := range report.Outcomes {
			outcomes[k] += n
		}
		status := "passed"
		detail := ""
		switch {
		case err != nil:
			status, detail = "error", err.Error()
		case f != nil:
			status, detail = "failed", f.Error()
		}
		if e := writeJSON(filepath.Join(dir, "result.json"), map[string]any{
			"status": status, "seed": h.Seed, "sessions": h.Sessions, "report": report, "error": detail,
		}); e != nil {
			return e
		}
		fmt.Printf("history %d (seed %d, %d sessions): %s %s\n", i, h.Seed, h.Sessions, status, detail)
		if status == "passed" {
			passed++
			continue
		}
		if f != nil && *reduce > 0 {
			signature := f.Signature()
			r := vcfuzz.Reduce(h, *reduce, func(trial vcfuzz.History) bool {
				got, e := vcfuzz.Run(trial, open)
				return e == nil && got.Failure != nil && got.Failure.Signature() == signature
			})
			if e := writeJSON(filepath.Join(dir, "reduced.json"), r); e != nil {
				return e
			}
			fmt.Printf("reduced to %d ops:\n", len(r.Ops))
			for _, op := range r.Ops {
				fmt.Println("  ", op)
			}
		}
	}
	if err := writeJSON(filepath.Join(*out, "summary.json"), map[string]any{
		"planned": len(histories), "passed": passed, "failed": len(histories) - passed,
		"outcomes": outcomes,
	}); err != nil {
		return err
	}
	if passed != len(histories) {
		return fmt.Errorf("%d of %d histories failed", len(histories)-passed, len(histories))
	}
	return nil
}

func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, "vc-fuzz:", err)
		os.Exit(1)
	}
}
