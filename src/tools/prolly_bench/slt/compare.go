package main

import (
	"bufio"
	"compress/gzip"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"time"
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
var gateClasses = []string{clRegression, clHiddenRegression, clCrashed, clUnmatched,
	clOrderOnly, clNotImplemented, clEngineOnlyPass, clTimeout, clDidNotRun, clBaseDidNotRun}

// exception is a record expected to differ from the baseline.
type exception struct {
	File         string `json:"file"`
	Line         int    `json:"line"`
	Reason       string `json:"reason"`
	Owner        string `json:"owner"`
	Issue        string `json:"issue"`
	Expires      string `json:"expires"`
	Class        string `json:"class"`
	BaselineHash string `json:"baseline_hash"`
	EngineHash   string `json:"engine_hash"`
	CorpusHash   string `json:"corpus_hash"`
}

type comparison struct {
	Counts          map[string]int `json:"counts"`
	GateFailures    int            `json:"gate_failures"`
	StaleExceptions []exception    `json:"stale_exceptions"`
}

// diff is one line of diffs.jsonl.gz: a record that is neither in
// agreement nor postgres_semantics, with both sides.
type diff struct {
	File         string  `json:"file"`
	Line         int     `json:"line"`
	Class        string  `json:"class"`
	Reason       string  `json:"reason,omitempty"`
	Baseline     *record `json:"baseline,omitempty"`
	Engine       *record `json:"engine,omitempty"`
	BaselineHash string  `json:"baseline_hash"`
	EngineHash   string  `json:"engine_hash"`
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
	case b.Status == stSkipped && e.Status == stSkipped:
		return clAgree
	case e.Status == stCrashed:
		return clCrashed
	case e.Status == stTimeout:
		return clTimeout
	case e.Status == stDidNotRun:
		return clDidNotRun
	case b.Status == stTimeout || b.Status == stDidNotRun || b.Status == stCrashed:
		return clBaseDidNotRun
	case b.Status == stOk && e.Status == stOk:
		if sameAnswer(b, e) {
			return clAgree
		}
		if b.Sorted != "" && b.Sorted == e.Sorted && b.Rows == e.Rows {
			return clOrderOnly
		}
		return clRegression
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
		if b.SortMode != e.SortMode || b.Version != e.Version || b.Rows != e.Rows {
			return false
		}
		if b.SortMode == "nosort" || b.SortMode == "" {
			return b.Ordered != "" && b.Ordered == e.Ordered
		}
		return b.Sorted == e.Sorted
	}
	if b.SQLState != "" || e.SQLState != "" {
		return b.SQLState == e.SQLState
	}
	return false // missing evidence cannot establish agreement
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
		k := recKey{r.File, r.Line}
		if r.File == "" || r.Line <= 0 || out[k] != nil {
			return nil, fmt.Errorf("invalid or duplicate record %s:%d", r.File, r.Line)
		}
		if (r.Status == stOk || r.Status == stNotOk) && (r.Version != 2 ||
			(r.SQLState == "" && (r.Ordered == "" || r.Sorted == ""))) {
			return nil, fmt.Errorf("legacy or missing execution evidence %s:%d", r.File, r.Line)
		}
		out[k] = r
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
		x := &list[i]
		expires, err := time.Parse("2006-01-02", x.Expires)
		if err != nil || !expires.After(time.Now()) || x.Owner == "" || x.Issue == "" || x.Reason == "" ||
			len(x.BaselineHash) != 64 || len(x.EngineHash) != 64 || len(x.CorpusHash) != 64 ||
			(x.Class != clRegression && x.Class != clHiddenRegression && x.Class != clOrderOnly && x.Class != clEngineOnlyPass) {
			return nil, fmt.Errorf("exception %s:%d needs bounded signatures, owner, issue and future expiry", x.File, x.Line)
		}
		key := recKey{x.File, x.Line}
		if out[key] != nil {
			return nil, fmt.Errorf("duplicate exception %v", key)
		}
		out[key] = x
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
	base, err := openRecords(baseDir)
	if err != nil {
		return nil, err
	}
	defer base.close()
	eng, err := openRecords(engineDir)
	if err != nil {
		return nil, err
	}
	defer eng.close()
	corpus := map[string]string{}
	if len(exc) > 0 {
		for _, dir := range []string{baseDir, engineDir} {
			data, err := os.ReadFile(filepath.Join(dir, "corpus.json"))
			if err != nil {
				return nil, err
			}
			var current map[string]string
			if err := json.Unmarshal(data, &current); err != nil {
				return nil, err
			}
			for name, hash := range current {
				if old, found := corpus[name]; found && old != hash {
					return nil, fmt.Errorf("corpus mismatch %s", name)
				}
				corpus[name] = hash
			}
		}
	}

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
	br, er := base.next(), eng.next()
	compared := 0
	for br != nil || er != nil {
		var b, e *record
		if er == nil || br != nil && lessKey(recKey{br.File, br.Line}, recKey{er.File, er.Line}) {
			b, br = br, base.next()
		} else if br == nil || lessKey(recKey{er.File, er.Line}, recKey{br.File, br.Line}) {
			e, er = er, eng.next()
		} else {
			b, e = br, er
			br, er = base.next(), eng.next()
		}
		var k recKey
		if b != nil {
			k = recKey{b.File, b.Line}
		} else {
			k = recKey{e.File, e.Line}
		}
		compared++
		class := classify(b, e)
		reason := ""
		if x := exc[k]; x != nil {
			if class == x.Class && recordHash(b) == x.BaselineHash && recordHash(e) == x.EngineHash && corpus[k.file] == x.CorpusHash {
				used[k] = true
				class, reason = clException, x.Reason
			}
		}
		cmp.Counts[class]++
		if class == clAgree || class == clPostgresSemantics {
			continue
		}
		bb, _ := json.Marshal(diff{File: k.file, Line: k.line, Class: class,
			Reason: reason, Baseline: b, Engine: e, BaselineHash: recordHash(b), EngineHash: recordHash(e)})
		w.Write(append(bb, '\n'))
	}
	if base.err != nil {
		return nil, base.err
	}
	if eng.err != nil {
		return nil, eng.err
	}
	// An exception for a record that now agrees, or that isn't in the
	// corpus any more, is stale.
	for k, x := range exc {
		if !used[k] {
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
	cmp.GateFailures += len(cmp.StaleExceptions)
	if compared == 0 {
		cmp.GateFailures++
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

func recordHash(r *record) string {
	if r == nil {
		return ""
	}
	copy := *r
	copy.Ms, copy.Message = 0, ""
	b, _ := json.Marshal(copy)
	h := sha256.Sum256(b)
	return hex.EncodeToString(h[:])
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

type recordStream struct {
	f        *os.File
	gz       *gzip.Reader
	sc       *bufio.Scanner
	previous recKey
	seen     bool
	err      error
}

func lessKey(a, b recKey) bool { return a.file < b.file || a.file == b.file && a.line < b.line }
func openRecords(dir string) (*recordStream, error) {
	f, err := os.Open(filepath.Join(dir, "records.jsonl.gz"))
	if err != nil {
		return nil, err
	}
	gz, err := gzip.NewReader(f)
	if err != nil {
		f.Close()
		return nil, err
	}
	sc := bufio.NewScanner(gz)
	sc.Buffer(make([]byte, 1<<20), 1<<24)
	return &recordStream{f: f, gz: gz, sc: sc}, nil
}
func (s *recordStream) close() { s.gz.Close(); s.f.Close() }
func (s *recordStream) next() *record {
	if s.err != nil {
		return nil
	}
	if !s.sc.Scan() {
		s.err = s.sc.Err()
		return nil
	}
	r := &record{}
	if s.err = json.Unmarshal(s.sc.Bytes(), r); s.err != nil {
		return nil
	}
	k := recKey{r.File, r.Line}
	if r.File == "" || r.Line <= 0 || s.seen && !lessKey(s.previous, k) {
		s.err = fmt.Errorf("duplicate, unordered or invalid record %s:%d", r.File, r.Line)
		return nil
	}
	if (r.Status == stOk || r.Status == stNotOk) && (r.Version != 2 ||
		(r.SQLState == "" && (r.Ordered == "" || r.Sorted == ""))) {
		s.err = fmt.Errorf("legacy or missing execution evidence %s:%d", r.File, r.Line)
		return nil
	}
	s.previous, s.seen = k, true
	return r
}
