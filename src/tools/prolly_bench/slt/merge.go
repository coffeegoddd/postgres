package main

import (
	"bufio"
	"bytes"
	"compress/gzip"
	"encoding/json"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"runtime"
	"sort"
	"strconv"
	"strings"
	"sync"
)

// Record statuses.
const (
	stOk             = "ok"
	stNotOk          = "not_ok"
	stSkipped        = "skipped"
	stTimeout        = "timeout"
	stDidNotRun      = "did_not_run"
	stNotImplemented = "not_implemented"
	stCrashed        = "crashed"
)

// record is one line of records.jsonl.gz: a test record's status, with the
// actual result's digest when it ran.  Skipped records are only counted.
type record struct {
	File     string `json:"file"`
	Line     int    `json:"line"`
	Status   string `json:"status"`
	SQLState string `json:"sqlstate,omitempty"`
	Message  string `json:"msg,omitempty"`
	Rows     int    `json:"rows,omitempty"`
	Ordered  string `json:"o,omitempty"`
	Sorted   string `json:"s,omitempty"`
	Ms       int64  `json:"ms"`
}

type summary struct {
	AM               string         `json:"am"`
	Schema           string         `json:"schema,omitempty"`
	Files            int            `json:"files"`
	Total            int            `json:"total"`
	Counts           map[string]int `json:"counts"`
	PassPercent      float64        `json:"pass_percent"`
	WrongAMFiles     []string       `json:"wrong_am_files"`
	Reruns           []string       `json:"reruns"`
	UnmatchedDigests int            `json:"unmatched_digests"`
	UnparsedLines    int            `json:"unparsed_lines"`
}

// logEntry is one line of the reference runner's log.
type logEntry struct {
	File    string
	Line    int
	Ms      int64
	Result  string // a status, before digests refine it
	Message string
}

// Sample line:
// 2019-10-16T12:20:29.0594292-07:00 123 index/random/10/slt_good_0.test:535: SELECT ... not ok: Schemas differ
var logLineRE = regexp.MustCompile(`^(\d{4}-\S+) (\d+) ([^\s:]+):(\d+): (.*)$`)

func parseLogLine(line string) (logEntry, bool) {
	m := logLineRE.FindStringSubmatch(line)
	if m == nil {
		return logEntry{}, false
	}
	e := logEntry{File: m[3]}
	e.Ms, _ = strconv.ParseInt(m[2], 10, 64)
	e.Line, _ = strconv.Atoi(m[4])
	rest := m[5]
	if i := strings.Index(rest, "not ok: "); i >= 0 && (i == 0 || rest[i-1] == ' ') {
		e.Result, e.Message = stNotOk, rest[i+len("not ok: "):]
		return e, true
	}
	for _, s := range []struct{ suffix, status string }{
		{"did not run", stDidNotRun}, {"timeout", stTimeout},
		{"skipped", stSkipped}, {"ok", stOk},
	} {
		if strings.HasSuffix(rest, s.suffix) {
			e.Result = s.status
			return e, true
		}
	}
	return logEntry{}, false
}

// isNotImplemented tells an engine's "not implemented yet" error from
// others with the same SQLSTATE, which Postgres also raises for features
// it refuses outright.
func isNotImplemented(sqlstate, msg string) bool {
	return sqlstate == "0A000" &&
		(strings.Contains(msg, "does not implement") || strings.HasSuffix(msg, " yet"))
}

// resolve combines a log entry and its digest (nil if none) into a record.
func resolve(e logEntry, d *digest) record {
	r := record{File: e.File, Line: e.Line, Status: e.Result, Message: e.Message, Ms: e.Ms}
	if d == nil {
		return r
	}
	r.SQLState, r.Rows, r.Ordered, r.Sorted = d.SQLState, d.Rows, d.Ordered, d.Sorted
	if d.Message != "" {
		r.Message = d.Message
	}
	switch {
	case d.Event == evAfterCrash || d.Event == evInitFailed:
		r.Status = stDidNotRun
	case d.Event == evCrash || d.Event == evLost:
		r.Status = stCrashed
	case r.Status == stNotOk && isNotImplemented(d.SQLState, d.Message):
		r.Status = stNotImplemented
	}
	r.Message = truncate(r.Message, 300)
	return r
}

// The postmaster's line for a backend that crashed.
var crashRE = regexp.MustCompile(`server process \(PID (\d+)\) (was terminated by|exited with exit code [1-9])`)

func crashedPIDs(serverLog string) (map[uint32]bool, error) {
	out := map[uint32]bool{}
	if serverLog == "" {
		return out, nil
	}
	data, err := os.ReadFile(serverLog)
	if err != nil {
		return nil, err
	}
	for _, m := range crashRE.FindAllStringSubmatch(string(data), -1) {
		pid, _ := strconv.ParseUint(m[1], 10, 32)
		out[uint32(pid)] = true
	}
	return out, nil
}

// collateralFiles are the files whose session died in a crash of some
// other backend.  Without a server log, every crash counts as the file's
// own.
func collateralFiles(scratch string, units []string, crashed map[uint32]bool, haveLog bool) (map[string]bool, error) {
	out := map[string]bool{}
	if !haveLog {
		return out, nil
	}
	for _, u := range units {
		err := eachDigest(filepath.Join(scratch, u+".digests.jsonl"), func(d *digest) {
			if d.Event == evCrash && !crashed[d.PID] {
				out[d.File] = true
			}
		})
		if err != nil {
			return nil, err
		}
	}
	return out, nil
}

func eachDigest(path string, fn func(*digest)) error {
	f, err := os.Open(path)
	if err != nil {
		return err
	}
	defer f.Close()
	sc := bufio.NewScanner(f)
	sc.Buffer(make([]byte, 1<<20), 1<<24)
	for sc.Scan() {
		var d digest
		if err := json.Unmarshal(sc.Bytes(), &d); err != nil {
			return err
		}
		fn(&d)
	}
	return sc.Err()
}

// digestReader yields a unit's digests a file at a time.  The unit writes
// them in the order it runs records, the same order as its log; order
// gives each file's position in the unit's list.
type digestReader struct {
	sc    *bufio.Scanner
	order map[string]int
	peek  *digest
	skip  int
}

func newDigestReader(r io.Reader, order map[string]int) *digestReader {
	sc := bufio.NewScanner(r)
	sc.Buffer(make([]byte, 1<<20), 1<<24)
	dr := &digestReader{sc: sc, order: order}
	dr.advance()
	return dr
}

func (dr *digestReader) advance() {
	dr.peek = nil
	if dr.sc.Scan() {
		var d digest
		if json.Unmarshal(dr.sc.Bytes(), &d) == nil {
			dr.peek = &d
		}
	}
}

// forFile returns file's digests by line.  Digests left over from earlier
// files (records the log didn't name) are skipped and counted.
func (dr *digestReader) forFile(file string) map[int]*digest {
	for dr.peek != nil && dr.order[dr.peek.File] < dr.order[file] {
		dr.skip++
		dr.advance()
	}
	out := map[int]*digest{}
	for dr.peek != nil && dr.peek.File == file {
		out[dr.peek.Line] = dr.peek
		dr.advance()
	}
	return out
}

// merge joins every unit's log and digests into records.jsonl.gz,
// files.jsonl, summary.json and failures.log.gz (the reference runner's
// log lines for records that neither passed nor were skipped).  A file run again after a
// crash elsewhere is represented by its rerun unit only.
func merge(cfg *config, units []string, rerun map[string]bool) (*summary, error) {
	sum := &summary{AM: cfg.am, Schema: cfg.schema, Counts: map[string]int{},
		WrongAMFiles: []string{}, Reruns: []string{}}
	for f := range rerun {
		sum.Reruns = append(sum.Reruns, f)
	}
	sort.Strings(sum.Reruns)

	// Units merge in parallel, each into gzip members of its own, written
	// out in order: concatenated gzip members are one gzip stream.
	parts := make([]*unitMerge, len(units))
	errs := make([]error, len(units))
	var wg sync.WaitGroup
	sem := make(chan struct{}, runtime.NumCPU())
	for i, u := range units {
		wg.Add(1)
		sem <- struct{}{}
		go func(i int, u string) {
			defer wg.Done()
			defer func() { <-sem }()
			parts[i], errs[i] = mergeUnit(cfg, u)
		}(i, u)
	}
	wg.Wait()
	for _, err := range errs {
		if err != nil {
			return nil, err
		}
	}

	var recs, fails, files bytes.Buffer
	for _, p := range parts {
		recs.Write(p.records.Bytes())
		fails.Write(p.failures.Bytes())
		files.Write(p.files.Bytes())
		sum.Total += p.sum.Total
		sum.Files += p.sum.Files
		sum.UnmatchedDigests += p.sum.UnmatchedDigests
		sum.UnparsedLines += p.sum.UnparsedLines
		sum.WrongAMFiles = append(sum.WrongAMFiles, p.sum.WrongAMFiles...)
		for k, v := range p.sum.Counts {
			sum.Counts[k] += v
		}
	}
	for name, b := range map[string][]byte{"records.jsonl.gz": recs.Bytes(),
		"failures.log.gz": fails.Bytes(), "files.jsonl": files.Bytes()} {
		if err := os.WriteFile(filepath.Join(cfg.out, name), b, 0o644); err != nil {
			return nil, err
		}
	}
	if run := sum.Total - sum.Counts[stSkipped]; run > 0 {
		sum.PassPercent = 100 * float64(sum.Counts[stOk]) / float64(run)
	}
	sort.Strings(sum.WrongAMFiles)
	b, _ := json.MarshalIndent(sum, "", "  ")
	return sum, os.WriteFile(filepath.Join(cfg.out, "summary.json"), append(b, '\n'), 0o644)
}

// unitMerge is one unit's share of the outputs: its records and failure
// lines as gzip members, its file lines, and its counts.
type unitMerge struct {
	records, failures, files bytes.Buffer
	sum                      summary
}

func mergeUnit(cfg *config, u string) (*unitMerge, error) {
	m := &unitMerge{sum: summary{Counts: map[string]int{}}}
	sum := &m.sum
	err := eachFileResult(filepath.Join(cfg.scratch, u+".files.jsonl"), func(fr fileResult, raw []byte) {
		sum.Files++
		if len(fr.WrongAM) > 0 {
			sum.WrongAMFiles = append(sum.WrongAMFiles, fr.File)
		}
		m.files.Write(append(raw, '\n'))
	})
	if err != nil {
		return nil, err
	}
	recgz, _ := gzip.NewWriterLevel(&m.records, gzip.BestSpeed)
	out := bufio.NewWriterSize(recgz, 1<<20)
	logOut, _ := gzip.NewWriterLevel(&m.failures, gzip.BestSpeed)

	logf, err := os.Open(filepath.Join(cfg.scratch, u+".log"))
	if err != nil {
		return nil, err
	}
	defer logf.Close()
	digf, err := os.Open(filepath.Join(cfg.scratch, u+".digests.jsonl"))
	if err != nil {
		return nil, err
	}
	defer digf.Close()
	files, err := readLines(filepath.Join(cfg.scratch, u+".files"))
	if err != nil {
		return nil, err
	}
	order := map[string]int{}
	for i, f := range files {
		order[testFilePath(f)] = i
	}
	dr := newDigestReader(digf, order)

	sc := bufio.NewScanner(logf)
	sc.Buffer(make([]byte, 1<<20), 1<<26)
	file := ""
	var digests map[int]*digest
	for sc.Scan() {
		line := sc.Text()
		e, ok := parseLogLine(line)
		if !ok {
			if logLineRE.MatchString(line) {
				sum.UnparsedLines++
			}
			continue
		}
		if e.File != file {
			sum.UnmatchedDigests += len(digests)
			file = e.File
			digests = dr.forFile(file)
		}
		d := digests[e.Line]
		delete(digests, e.Line)
		r := resolve(e, d)
		sum.Total++
		sum.Counts[r.Status]++
		if r.Status == stSkipped {
			continue
		}
		if r.Status != stOk {
			io.WriteString(logOut, line+"\n")
		}
		b, _ := json.Marshal(r)
		out.Write(append(b, '\n'))
	}
	sum.UnmatchedDigests += len(digests) + dr.skip
	if err := sc.Err(); err != nil {
		return nil, err
	}
	if err := out.Flush(); err != nil {
		return nil, err
	}
	if err := recgz.Close(); err != nil {
		return nil, err
	}
	return m, logOut.Close()
}

func eachFileResult(path string, fn func(fileResult, []byte)) error {
	f, err := os.Open(path)
	if err != nil {
		return err
	}
	defer f.Close()
	sc := bufio.NewScanner(f)
	sc.Buffer(make([]byte, 1<<20), 1<<24)
	for sc.Scan() {
		var fr fileResult
		if err := json.Unmarshal(sc.Bytes(), &fr); err != nil {
			return err
		}
		fn(fr, append([]byte(nil), sc.Bytes()...))
	}
	return sc.Err()
}
