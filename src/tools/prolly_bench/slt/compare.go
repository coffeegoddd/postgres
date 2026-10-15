package main

import (
	"bufio"
	"compress/gzip"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"sort"
)

// Comparison classes of an engine's record against the baseline's (heap's).
const (
	clAgree             = "agree"
	clOrderOnly         = "order_only"         // same rows, another order
	clNotImplemented    = "not_implemented"    // the engine doesn't implement it yet
	clRegression        = "regression"         // heap passes, the engine doesn't
	clPostgresSemantics = "postgres_semantics" // both fail with the same answer
	clHiddenRegression  = "hidden_regression"  // both fail, with different answers
	clEngineOnlyPass    = "engine_only_pass"   // the engine passes where heap doesn't
	clCrashed           = "crashed"
	clTimeout           = "timeout"
	clDidNotRun         = "did_not_run"
	clBaseDidNotRun     = "baseline_did_not_run" // heap timed out or didn't run it
	clException         = "exception"            // listed as expected to differ
	clUnmatched         = "unmatched"            // in one result set only
)

// The classes that fail the gate.
var gateClasses = []string{clRegression, clHiddenRegression, clCrashed, clUnmatched}

// exception is a record expected to differ from the baseline.
type exception struct {
	File   string `json:"file"`
	Line   int    `json:"line"`
	Reason string `json:"reason"`
}

type comparison struct {
	Counts          map[string]int `json:"counts"`
	GateFailures    int            `json:"gate_failures"`
	StaleExceptions []exception    `json:"stale_exceptions"`
}

// diff is one line of diffs.jsonl.gz: a record that is neither in
// agreement nor postgres_semantics, with both sides.
type diff struct {
	File     string  `json:"file"`
	Line     int     `json:"line"`
	Class    string  `json:"class"`
	Reason   string  `json:"reason,omitempty"`
	Baseline *record `json:"baseline,omitempty"`
	Engine   *record `json:"engine,omitempty"`
}

type recKey struct {
	file string
	line int
}

// classify compares an engine's record with the baseline's.
func classify(b, e *record) string {
	switch {
	case b == nil || e == nil:
		return clUnmatched
	case e.Status == stCrashed:
		return clCrashed
	case e.Status == stTimeout:
		return clTimeout
	case e.Status == stDidNotRun:
		return clDidNotRun
	case b.Status == stTimeout || b.Status == stDidNotRun || b.Status == stCrashed:
		return clBaseDidNotRun
	case b.Status == stOk && e.Status == stOk:
		return clAgree
	case b.Status == stOk && e.Status == stNotImplemented:
		return clNotImplemented
	case b.Status == stOk:
		if e.Sorted != "" && e.Sorted == b.Sorted && e.Rows == b.Rows {
			return clOrderOnly
		}
		return clRegression
	case e.Status == stOk:
		return clEngineOnlyPass
	case sameAnswer(b, e):
		return clPostgresSemantics
	default:
		return clHiddenRegression
	}
}

// sameAnswer: two failing records gave the same result, or the same error.
// Order doesn't count: it isn't stable between runs even on heap.
func sameAnswer(b, e *record) bool {
	if b.Sorted != "" || e.Sorted != "" {
		return b.Sorted == e.Sorted && b.Rows == e.Rows
	}
	if b.SQLState != "" || e.SQLState != "" {
		return b.SQLState == e.SQLState
	}
	return b.Message == e.Message
}

func loadRecords(dir string) (map[recKey]*record, error) {
	f, err := os.Open(filepath.Join(dir, "records.jsonl.gz"))
	if err != nil {
		return nil, err
	}
	defer f.Close()
	gz, err := gzip.NewReader(f)
	if err != nil {
		return nil, err
	}
	sc := bufio.NewScanner(gz)
	sc.Buffer(make([]byte, 1<<20), 1<<24)
	out := map[recKey]*record{}
	for sc.Scan() {
		r := &record{}
		if err := json.Unmarshal(sc.Bytes(), r); err != nil {
			return nil, err
		}
		out[recKey{r.File, r.Line}] = r
	}
	return out, sc.Err()
}

func loadExceptions(path string) (map[recKey]*exception, error) {
	out := map[recKey]*exception{}
	if path == "" {
		return out, nil
	}
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	var list []exception
	if err := json.Unmarshal(data, &list); err != nil {
		return nil, err
	}
	for i := range list {
		out[recKey{list[i].File, list[i].Line}] = &list[i]
	}
	return out, nil
}

// compareDirs compares an engine's slt-runner output with the baseline's
// and writes compare.json and diffs.jsonl.gz to out.
func compareDirs(baseDir, engineDir, out, exceptionsPath string) (*comparison, error) {
	exc, err := loadExceptions(exceptionsPath)
	if err != nil {
		return nil, err
	}
	base, err := loadRecords(baseDir)
	if err != nil {
		return nil, err
	}
	eng, err := loadRecords(engineDir)
	if err != nil {
		return nil, err
	}
	keys := make([]recKey, 0, len(base))
	for k := range base {
		keys = append(keys, k)
	}
	for k := range eng {
		if base[k] == nil {
			keys = append(keys, k)
		}
	}
	sort.Slice(keys, func(i, j int) bool {
		if keys[i].file != keys[j].file {
			return keys[i].file < keys[j].file
		}
		return keys[i].line < keys[j].line
	})

	if err := os.MkdirAll(out, 0o755); err != nil {
		return nil, err
	}
	df, err := os.Create(filepath.Join(out, "diffs.jsonl.gz"))
	if err != nil {
		return nil, err
	}
	defer df.Close()
	gz, _ := gzip.NewWriterLevel(df, gzip.BestSpeed)
	w := bufio.NewWriterSize(gz, 1<<20)

	cmp := &comparison{Counts: map[string]int{}, StaleExceptions: []exception{}}
	used := map[recKey]bool{}
	for _, k := range keys {
		b, e := base[k], eng[k]
		class := classify(b, e)
		reason := ""
		if x := exc[k]; x != nil {
			used[k] = true
			if class != clAgree && class != clPostgresSemantics {
				class, reason = clException, x.Reason
			}
		}
		cmp.Counts[class]++
		if class == clAgree || class == clPostgresSemantics {
			continue
		}
		bb, _ := json.Marshal(diff{File: k.file, Line: k.line, Class: class,
			Reason: reason, Baseline: b, Engine: e})
		w.Write(append(bb, '\n'))
	}
	// An exception for a record that now agrees, or that isn't in the
	// corpus any more, is stale.
	for k, x := range exc {
		if !used[k] || cmp.isAgreeing(base[k], eng[k]) {
			cmp.StaleExceptions = append(cmp.StaleExceptions, *x)
		}
	}
	sort.Slice(cmp.StaleExceptions, func(i, j int) bool {
		a, b := cmp.StaleExceptions[i], cmp.StaleExceptions[j]
		return a.File < b.File || a.File == b.File && a.Line < b.Line
	})
	for _, c := range gateClasses {
		cmp.GateFailures += cmp.Counts[c]
	}
	if err := w.Flush(); err != nil {
		return nil, err
	}
	if err := gz.Close(); err != nil {
		return nil, err
	}
	b, _ := json.MarshalIndent(cmp, "", "  ")
	return cmp, os.WriteFile(filepath.Join(out, "compare.json"), append(b, '\n'), 0o644)
}

func (c *comparison) isAgreeing(b, e *record) bool {
	cl := classify(b, e)
	return cl == clAgree || cl == clPostgresSemantics
}

func runCompare(args []string) int {
	if len(args) < 3 || len(args) > 4 {
		fmt.Fprintln(os.Stderr, "usage: slt-runner compare BASELINE_DIR ENGINE_DIR OUT_DIR [EXCEPTIONS_JSON]")
		return 1
	}
	exc := ""
	if len(args) == 4 {
		exc = args[3]
	}
	cmp, err := compareDirs(args[0], args[1], args[2], exc)
	if err != nil {
		fatal(err)
	}
	fmt.Fprintf(os.Stderr, "slt-runner: %v; %d gate failures\n", cmp.Counts, cmp.GateFailures)
	return 0
}
