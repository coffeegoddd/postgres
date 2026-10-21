package vcfuzz

import (
	"os"
	"testing"
	"time"
)

// A write waits for as many steps as the holder's transaction stays open,
// longer than one statement's timeout: it finishes once that transaction
// ends, not as the harness's error.  Needs a server: VCFUZZ_DSN names it.
func TestWaitOutlastsStatementTimeout(t *testing.T) {
	dsn := os.Getenv("VCFUZZ_DSN")
	if dsn == "" {
		t.Skip("VCFUZZ_DSN unset")
	}
	const timeout = 300 * time.Millisecond
	setup := emptyState()
	setup["t"][1] = Row{1, 1}
	h := History{Version: 1, Sessions: 2, Setup: setup}
	e, err := OpenPostgres(dsn, timeout)(h)
	if err != nil {
		t.Fatal(err)
	}
	defer e.Close()
	for _, step := range []struct {
		op    Op
		class string
	}{
		{Op{Session: 0, Kind: OpBegin}, ClassOK},
		{Op{Session: 0, Kind: OpUpdate, Table: "t", ID: 1, Val: 2}, ClassOK},
		{Op{Session: 1, Kind: OpUpdate, Table: "t", ID: 1, Col: 1, Val: 3, Wait: 1}, ClassWaiting},
	} {
		o, err := e.Apply(step.op)
		if err != nil || o.Class != step.class {
			t.Fatalf("%s: %v, %v", step.op, o, err)
		}
	}
	time.Sleep(3 * timeout)
	o, err := e.Apply(Op{Session: 0, Kind: OpCommitTxn})
	if err != nil || o.Class != ClassOK {
		t.Fatalf("commit: %v, %v", o, err)
	}
	// (the session's statements are timed again)
	rows, err := e.(*Postgres).must(e.(*Postgres).sessions[1], "SHOW lock_timeout")
	if err != nil || rows[0][0] != "300ms" {
		t.Fatalf("lock_timeout after the wait: %v, %v", rows, err)
	}
}
