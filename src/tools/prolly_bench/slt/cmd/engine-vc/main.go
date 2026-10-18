// engine-vc executes strict version-control scenarios on PostgreSQL engines.
package main

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"

	"prolly_bench/slt/internal/vcscenario"
)

func writeJSON(path string, v any) error {
	b, e := json.MarshalIndent(v, "", "  ")
	if e != nil {
		return e
	}
	return os.WriteFile(path, append(b, '\n'), 0644)
}

type summary struct {
	Profiles       []vcscenario.Profile `json:"profiles"`
	ScenarioHashes map[string]string    `json:"scenario_sha256"`
	Counts         map[string]int       `json:"counts"`
	Results        []vcscenario.Result  `json:"results"`
}

func (s *summary) write(dir string) error {
	s.Counts = map[string]int{"planned": len(s.Results), "passed": 0, "failed": 0, "unsupported": 0, "not_run": 0}
	for _, r := range s.Results {
		s.Counts[r.Status]++
	}
	return writeJSON(filepath.Join(dir, "summary.json"), s)
}

func run() error {
	dsn := flag.String("dsn", "", "server connection string; only fresh randomly named databases are used")
	ams := flag.String("am", "prolly", "comma-separated engine names")
	cases := flag.String("cases", "", "scenario JSON file or directory")
	out := flag.String("out", "", "new artifact directory")
	timeout := flag.Duration("timeout", 30*time.Second, "timeout per SQL operation")
	selfTest := flag.Bool("self-test", false, "exercise native adapter negative controls")
	flag.Parse()
	if *dsn == "" || *cases == "" || *out == "" || *timeout <= 0 || flag.NArg() != 0 {
		return fmt.Errorf("requires -dsn, -cases, -out and positive -timeout")
	}
	info, err := os.Stat(*cases)
	if err != nil {
		return err
	}
	paths := []string{*cases}
	if info.IsDir() {
		paths, err = filepath.Glob(filepath.Join(*cases, "*.json"))
		if err != nil {
			return err
		}
	}
	if len(paths) == 0 {
		return fmt.Errorf("empty scenario corpus")
	}
	sum := summary{ScenarioHashes: map[string]string{}}
	var scenarios []vcscenario.Scenario
	var originals [][]byte
	for _, path := range paths {
		b, e := os.ReadFile(path)
		if e != nil {
			return e
		}
		s, e := vcscenario.Parse(b)
		if e != nil {
			return fmt.Errorf("%s: %w", path, e)
		}
		if _, ok := sum.ScenarioHashes[s.ID]; ok {
			return fmt.Errorf("duplicate scenario id %q", s.ID)
		}
		h := sha256.Sum256(b)
		sum.ScenarioHashes[s.ID] = hex.EncodeToString(h[:])
		scenarios = append(scenarios, s)
		originals = append(originals, b)
	}
	seen := map[string]bool{}
	for _, am := range strings.Split(*ams, ",") {
		if seen[am] {
			return fmt.Errorf("duplicate engine %q", am)
		}
		seen[am] = true
		p, e := vcscenario.EngineProfile(am)
		if e != nil {
			return e
		}
		sum.Profiles = append(sum.Profiles, p)
		for _, s := range scenarios {
			sum.Results = append(sum.Results, vcscenario.Result{ID: s.ID, Engine: am, Status: "not_run", Requires: s.Requires, PlannedSteps: s.PlannedSteps()})
		}
	}
	if err = os.Mkdir(*out, 0755); err != nil {
		return err
	}
	if err = os.Mkdir(filepath.Join(*out, "scenarios"), 0755); err != nil {
		return err
	}
	for i, s := range scenarios {
		if err = os.WriteFile(filepath.Join(*out, "scenarios", s.ID+".json"), originals[i], 0644); err != nil {
			return err
		}
	}
	if err = sum.write(*out); err != nil {
		return err
	}
	if *selfTest {
		var results []vcscenario.Result
		var e error
		for _, am := range strings.Split(*ams, ",") {
			var r []vcscenario.Result
			r, e = vcscenario.SelfTest(context.Background(), *dsn, am, *timeout)
			results = append(results, r...)
			if e != nil {
				break
			}
		}
		if err = writeJSON(filepath.Join(*out, "self-test.json"), map[string]any{"passed": e == nil, "controls": results}); err != nil {
			return err
		}
		if e != nil {
			return e
		}
		fmt.Printf("adapter self-test: all %d negative controls verified on %s (not corpus passes)\n", len(results), *ams)
	}
	n := 0
	for _, p := range sum.Profiles {
		for _, s := range scenarios {
			a, e := vcscenario.NewPostgres(*dsn, p.Engine, *timeout)
			if e != nil {
				return e
			}
			r := vcscenario.Run(context.Background(), s, a)
			sum.Results[n] = r
			n++
			fmt.Printf("%s/%s: %s", p.Engine, s.ID, r.Status)
			if len(r.Missing) != 0 {
				fmt.Printf(" (missing %s)", strings.Join(r.Missing, ","))
			}
			if r.Error != "" {
				fmt.Printf(" (%s: %s)", r.Phase, r.Error)
			}
			fmt.Println()
			if err = sum.write(*out); err != nil {
				return err
			}
		}
	}
	if sum.Counts["passed"] != sum.Counts["planned"] {
		return fmt.Errorf("incomplete coverage: %d passed, %d failed, %d unsupported, %d not run", sum.Counts["passed"], sum.Counts["failed"], sum.Counts["unsupported"], sum.Counts["not_run"])
	}
	return nil
}

func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, "engine-vc:", err)
		os.Exit(1)
	}
}
