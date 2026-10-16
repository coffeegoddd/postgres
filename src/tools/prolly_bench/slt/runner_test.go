package main

import (
	"compress/gzip"
	"encoding/json"
	"github.com/jackc/pgx/v5/pgconn"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestParseLogLine(t *testing.T) {
	cases := []struct {
		line, file, result, msg string
		lineNum                 int
	}{
		{"2026-10-15T12:00:00.1-07:00 12 select1.test:118: SELECT a FROM t1 ok",
			"select1.test", stOk, "", 118},
		{"2026-10-15T12:00:00.1-07:00 3 index/between/1/slt_good_0.test:535: SELECT * FROM tab0 not ok: Schemas differ. Expected III, got IIT",
			"index/between/1/slt_good_0.test", stNotOk, "Schemas differ. Expected III, got IIT", 535},
		{"2026-10-15T12:00:00.1-07:00 0 random/expr/slt_good_1.test:9: SELECT 1 skipped",
			"random/expr/slt_good_1.test", stSkipped, "", 9},
		{"2026-10-15T12:00:00.1-07:00 0 select2.test:40: SELECT 1 did not run",
			"select2.test", stDidNotRun, "", 40},
		{"2026-10-15T12:00:00.1-07:00 60000 select2.test:41: SELECT pg_sleep(100) timeout",
			"select2.test", stTimeout, "", 41},
		// A not-ok message that ends like another status.
		{"2026-10-15T12:00:00.1-07:00 5 select3.test:7: SELECT 1 not ok: Unexpected error timeout",
			"select3.test", stNotOk, "Unexpected error timeout", 7},
	}
	for _, c := range cases {
		e, ok := parseLogLine(c.line)
		if !ok || e.File != c.file || e.Line != c.lineNum || e.Result != c.result || e.Message != c.msg {
			t.Errorf("parseLogLine(%q) = %+v, %v", c.line, e, ok)
		}
	}
	for _, junk := range []string{"stacktrace from panic:", "goroutine 1 [running]:", ""} {
		if _, ok := parseLogLine(junk); ok {
			t.Errorf("parseLogLine(%q) parsed", junk)
		}
	}
}

func TestResolve(t *testing.T) {
	e := logEntry{File: "f.test", Line: 3, Result: stNotOk, Message: "Unexpected error x"}
	cases := []struct {
		d    *digest
		want string
	}{
		{nil, stDidNotRun},
		{&digest{SQLState: "0A000", Message: `table access method "prolly_d2" does not implement scan yet`}, stNotImplemented},
		{&digest{SQLState: "0A000", Message: "prolly indexes do not support INCLUDE columns yet"}, stNotImplemented},
		{&digest{SQLState: "0A000", Message: "cannot use subquery in DEFAULT expression"}, stNotOk},
		{&digest{Event: evCrash}, stCrashed},
		{&digest{Event: evLost}, stCrashed},
		{&digest{Event: evAfterCrash}, stDidNotRun},
	}
	for _, c := range cases {
		if got := resolve(e, c.d).Status; got != c.want {
			t.Errorf("resolve(%+v) = %s, want %s", c.d, got, c.want)
		}
	}
	// A statement expecting an error is "ok" in the log when the session
	// was gone; it didn't really run.
	ok := logEntry{File: "f.test", Line: 4, Result: stOk}
	if got := resolve(ok, &digest{Event: evAfterCrash}).Status; got != stDidNotRun {
		t.Errorf("after a crash: %s", got)
	}
}

func TestTestFilePath(t *testing.T) {
	cases := map[string]string{
		"/x/sqllogictest/test/select1.test":                            "select1.test",
		"/x/sqllogictest/test/random/expr/slt_good_0.test":             "random/expr/slt_good_0.test",
		"/x/sqllogictest/test/index/between/10/slt_good_0.test":        "index/between/10/slt_good_0.test",
		"/x/sqllogictest/test/index/orderby_nosort/10/slt_good_3.test": "index/orderby_nosort/10/slt_good_3.test",
	}
	for in, want := range cases {
		if got := testFilePath(in); got != want {
			t.Errorf("testFilePath(%q) = %q, want %q", in, got, want)
		}
	}
}

func TestDigestReader(t *testing.T) {
	// a.test has no digests; c.test's first digest is a stray.
	data := strings.Join([]string{
		`{"f":"b.test","l":1}`, `{"f":"b.test","l":5}`,
		`{"f":"c.test","l":0}`, `{"f":"c.test","l":2}`,
	}, "\n")
	order := map[string]int{"a.test": 0, "b.test": 1, "c.test": 2}
	dr := newDigestReader(strings.NewReader(data), order)
	if got := dr.forFile("a.test"); len(got) != 0 {
		t.Fatalf("a.test: %v", got)
	}
	if got := dr.forFile("b.test"); len(got) != 2 || got[5] == nil {
		t.Fatalf("b.test: %v", got)
	}
	if got := dr.forFile("c.test"); len(got) != 2 || got[2] == nil {
		t.Fatalf("c.test: %v", got)
	}
}

func TestFormatValue(t *testing.T) {
	cases := []struct {
		oid  uint32
		in   []byte
		want string
	}{
		{oidInt4, []byte("42"), "42"},
		{oidInt4, nil, "NULL"},
		{oidBool, []byte("t"), "1"},
		{oidBool, []byte("f"), "0"},
		{oidFloat8, []byte("0.1"), "0.100"},
		{oidNumeric, []byte("-12.3456"), "-12.346"},
		{25, []byte(""), ""},
	}
	for _, c := range cases {
		if got := formatValue(c.oid, c.in); got != c.want {
			t.Errorf("formatValue(%d, %q) = %q, want %q", c.oid, c.in, got, c.want)
		}
	}
}

func TestHashSortedRows(t *testing.T) {
	a := []string{"1", "x", "2", "y"}
	b := []string{"2", "y", "1", "x"}
	if hashValues(a) == hashValues(b) {
		t.Error("ordered hashes of different orders are equal")
	}
	if hashSortedRows(a, 2) != hashSortedRows(b, 2) {
		t.Error("sorted hashes of the same rows differ")
	}
	// Sorting is by row, not by value.
	c := []string{"1", "y", "2", "x"}
	if hashSortedRows(a, 2) == hashSortedRows(c, 2) {
		t.Error("different rows with the same values hash the same")
	}
}

func TestClassify(t *testing.T) {
	ok := func(s string) *record { return &record{Status: stOk, Ordered: s, Sorted: s, Rows: 1} }
	bad := func(s string) *record { return &record{Status: stNotOk, Ordered: s, Sorted: s, Rows: 1} }
	errRec := func(st, code string) *record { return &record{Status: st, SQLState: code} }
	cases := []struct {
		b, e *record
		want string
	}{
		{ok("a"), ok("a"), clAgree},
		{ok("a"), bad("a"), clOrderOnly},
		{ok("a"), bad("b"), clRegression},
		{ok("a"), errRec(stNotOk, "XX000"), clRegression},
		{ok("a"), errRec(stNotImplemented, "0A000"), clNotImplemented},
		{bad("a"), bad("a"), clPostgresSemantics},
		{bad("a"), bad("b"), clHiddenRegression},
		{errRec(stNotOk, "42601"), errRec(stNotOk, "42601"), clPostgresSemantics},
		{errRec(stNotOk, "42601"), errRec(stNotOk, "XX000"), clHiddenRegression},
		{&record{Status: stNotOk, Message: "Expected error but didn't get one"},
			&record{Status: stNotOk, Message: "Expected error but didn't get one"}, clHiddenRegression},
		{bad("a"), ok("a"), clEngineOnlyPass},
		{ok("a"), &record{Status: stCrashed}, clCrashed},
		{ok("a"), &record{Status: stTimeout}, clTimeout},
		{&record{Status: stTimeout}, ok("a"), clBaseDidNotRun},
		{nil, ok("a"), clUnmatched},
	}
	for i, c := range cases {
		if got := classify(c.b, c.e); got != c.want {
			t.Errorf("case %d: classify = %s, want %s", i, got, c.want)
		}
	}
}

func TestCompareDirs(t *testing.T) {
	dir := t.TempDir()
	write := func(name string, recs ...record) string {
		d := filepath.Join(dir, name)
		os.MkdirAll(d, 0o755)
		f, _ := os.Create(filepath.Join(d, "records.jsonl.gz"))
		gz := gzip.NewWriter(f)
		for _, r := range recs {
			r.Version, r.Ordered = 2, r.Sorted
			b, _ := json.Marshal(r)
			gz.Write(append(b, '\n'))
		}
		gz.Close()
		f.Close()
		return d
	}
	base := write("base",
		record{File: "a.test", Line: 1, Status: stOk, Sorted: "x", Rows: 1},
		record{File: "a.test", Line: 2, Status: stOk, Sorted: "y", Rows: 1},
		record{File: "a.test", Line: 3, Status: stOk, Sorted: "z", Rows: 1})
	eng := write("eng",
		record{File: "a.test", Line: 1, Status: stOk, Sorted: "x", Rows: 1},
		record{File: "a.test", Line: 2, Status: stNotOk, Sorted: "q", Rows: 2},
		record{File: "a.test", Line: 3, Status: stNotOk, Sorted: "w", Rows: 1})
	exc := filepath.Join(dir, "exc.json")
	br, _ := loadRecords(base)
	er, _ := loadRecords(eng)
	corpusHash := strings.Repeat("a", 64)
	for _, dir := range []string{base, eng} {
		data, _ := json.Marshal(map[string]string{"a.test": corpusHash})
		os.WriteFile(filepath.Join(dir, "corpus.json"), data, 0o644)
	}
	var exceptions []exception
	for _, line := range []int{3, 1} {
		k := recKey{"a.test", line}
		exceptions = append(exceptions, exception{File: k.file, Line: line, Reason: "known",
			Owner: "maintainer", Issue: "issue/1", Expires: "2999-01-01", Class: clRegression,
			BaselineHash: recordHash(br[k]), EngineHash: recordHash(er[k]), CorpusHash: corpusHash})
	}
	data, _ := json.Marshal(exceptions)
	os.WriteFile(exc, data, 0o644)
	cmp, err := compareDirs(base, eng, filepath.Join(dir, "out"), exc)
	if err != nil {
		t.Fatal(err)
	}
	if cmp.Counts[clAgree] != 1 || cmp.Counts[clRegression] != 1 || cmp.Counts[clException] != 1 {
		t.Errorf("counts %v", cmp.Counts)
	}
	if cmp.GateFailures != 2 {
		t.Errorf("gate failures %d", cmp.GateFailures)
	}
	if len(cmp.StaleExceptions) != 1 || cmp.StaleExceptions[0].Line != 1 {
		t.Errorf("stale %v", cmp.StaleExceptions)
	}
}

func TestExactEvidence(t *testing.T) {
	makeResult := func(oid uint32, rows ...[][]byte) *pgconn.Result {
		return &pgconn.Result{FieldDescriptions: []pgconn.FieldDescription{{DataTypeOID: oid}}, Rows: rows}
	}
	cases := [][2]*pgconn.Result{
		{makeResult(25, [][]byte{nil}), makeResult(25, [][]byte{[]byte("NULL")})},
		{makeResult(25, [][]byte{[]byte{}}), makeResult(25, [][]byte{nil})},
		{makeResult(1700, [][]byte{[]byte("1.0001")}), makeResult(1700, [][]byte{[]byte("1.0002")})},
		{makeResult(25, [][]byte{[]byte("1")}), makeResult(23, [][]byte{[]byte("1")})},
		{makeResult(25, [][]byte{[]byte("a\nb")}), makeResult(25, [][]byte{[]byte("a")}, [][]byte{[]byte("b")})},
	}
	for _, pair := range cases {
		a, as := exactResult(pair[0])
		b, bs := exactResult(pair[1])
		if a == b || as == bs {
			t.Fatal("distinct exact results collided")
		}
	}
	a := makeResult(25, [][]byte{[]byte("a")}, [][]byte{[]byte("b")})
	b := makeResult(25, [][]byte{[]byte("b")}, [][]byte{[]byte("a")})
	ao, as := exactResult(a)
	bo, bs := exactResult(b)
	if ao == bo || as != bs {
		t.Fatal("row-order contract violated")
	}
	base := &record{Version: 2, Status: stOk, Ordered: ao, Sorted: as, Rows: 2, SortMode: "nosort"}
	eng := &record{Version: 2, Status: stOk, Ordered: bo, Sorted: bs, Rows: 2, SortMode: "nosort"}
	if classify(base, eng) != clOrderOnly {
		t.Fatal("ordered query mismatch passed")
	}
	eng.Sorted = "different"
	if classify(base, eng) != clRegression {
		t.Fatal("both-ok mismatch passed")
	}
}

func TestCorruptDigestStream(t *testing.T) {
	for _, data := range []string{"not json\n", "{\"f\":\"a\",\"l\":1}\n{\"f\":\"a\",\"l\":1}\n"} {
		dr := newDigestReader(strings.NewReader(data), map[string]int{"a": 0})
		dr.forFile("a")
		if dr.err == nil {
			t.Fatal("corrupt or duplicate digest accepted")
		}
	}
}

func TestPlanConditionalHalt(t *testing.T) {
	dir := t.TempDir()
	file := filepath.Join(dir, "conditional.test")
	data := "onlyif mysql\nhalt\n\nquery I nosort\nSELECT 1\n----\n1\n\nonlyif postgresql\nhalt\n\nquery I nosort\nSELECT 2\n----\n2\n"
	if err := os.WriteFile(file, []byte(data), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := writePlan(&config{out: dir}, []string{file}); err != nil {
		t.Fatal(err)
	}
	f, err := os.Open(filepath.Join(dir, "planned.jsonl.gz"))
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	gz, err := gzip.NewReader(f)
	if err != nil {
		t.Fatal(err)
	}
	defer gz.Close()
	b, err := io.ReadAll(gz)
	if err != nil {
		t.Fatal(err)
	}
	if strings.Count(string(b), "\n") != 1 || !strings.Contains(string(b), `"line":5`) {
		t.Fatalf("conditional halt plan: %s", b)
	}
}
