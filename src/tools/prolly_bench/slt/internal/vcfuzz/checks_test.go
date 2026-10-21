package vcfuzz

import (
	"strings"
	"testing"
)

// A failure's signature leaves out what any removal of an op changes (rows,
// commit ids), so a reduced history can fail the same way.
func TestSignature(t *testing.T) {
	a := &Failure{Kind: "session 0 rows", Step: 9, Op: Op{Kind: OpMerge}, Expected: "t:1=1,1;|u:|", Got: "t:1=2,1;|u:|"}
	b := &Failure{Kind: "session 0 rows", Step: 3, Op: Op{Kind: OpMerge}, Expected: "t:|u:|", Got: "t:2=0,0;|u:|"}
	if a.Signature() != b.Signature() {
		t.Fatalf("%q and %q differ", a.Signature(), b.Signature())
	}
	o := &Failure{Kind: "outcome", Op: Op{Kind: OpPick}, Expected: Outcome{Class: ClassOK}, Got: Outcome{Class: ClassConflict}}
	p := &Failure{Kind: "outcome", Op: Op{Kind: OpPick}, Expected: Outcome{Class: ClassOK}, Got: Outcome{Class: ClassUnchanged}}
	if o.Signature() == p.Signature() {
		t.Fatalf("outcomes of different classes share %q", o.Signature())
	}
}

// Reduce keeps its place after a removal: two ops far apart, needed
// together, are found within a budget a restart from the first chunk
// after each removal exhausts.
func TestReduceKeepsItsPlace(t *testing.T) {
	h := History{Version: 1, Sessions: 1}
	for i := 0; i < 128; i++ {
		h.Ops = append(h.Ops, Op{Kind: OpInsert, Table: "t", ID: 1, Val: i})
	}
	need := func(trial History) bool {
		found := 0
		for _, op := range trial.Ops {
			if op.Val == 5 || op.Val == 120 {
				found++
			}
		}
		return found == 2
	}
	r := Reduce(h, 36, need)
	if len(r.Ops) != 2 {
		t.Fatalf("reduced to %d ops", len(r.Ops))
	}
}

// A hard reset drops uncommitted changes, so it runs on a dirty branch.
func TestModelResetDropsChanges(t *testing.T) {
	m := NewModel(1, rows(map[int]Row{1: {1, 1}}))
	op := Op{Kind: OpUpdate, Table: "t", ID: 1, Val: 9}
	m.Apply(op)
	reset := Op{Kind: OpReset, Ref: "HEAD"}
	if !m.Legal(reset) {
		t.Fatal("a reset over uncommitted changes is illegal")
	}
	m.Apply(reset)
	if m.Dirty("main") || m.Visible(0)["t"][1] != (Row{1, 1}) {
		t.Fatalf("reset kept %v", m.Visible(0)["t"])
	}
}

// A commit, or a branch made, doesn't wait on another session's open
// transaction writing on the branch (the multi-session specs show it), so
// histories hold them.
func TestModelCommitBesideTransaction(t *testing.T) {
	m := NewModel(2, rows(map[int]Row{1: {1, 1}}))
	m.Apply(Op{Session: 0, Kind: OpBegin})
	m.Apply(Op{Session: 0, Kind: OpUpdate, Table: "t", ID: 1, Val: 5})
	for _, op := range []Op{{Session: 1, Kind: OpCommit}, {Session: 1, Kind: OpBranch, Ref: "x"}} {
		if !m.Legal(op) {
			t.Fatalf("%s illegal beside an open transaction", op)
		}
	}
	if m.Legal(Op{Session: 1, Kind: OpReset, Ref: "HEAD"}) {
		t.Fatal("a reset beside an open transaction is legal")
	}
}

// Of two maximal common ancestors (a criss-cross) the model names none;
// of one, that one.
func TestModelMergeBase(t *testing.T) {
	m := &Model{Commits: []Commit{
		{},                     // 0
		{Parents: []int{0}},    // 1: x1
		{Parents: []int{0}},    // 2: y1
		{Parents: []int{1, 2}}, // 3: xy
		{Parents: []int{2, 1}}, // 4: yx
		{Parents: []int{1}},    // 5
	}}
	if _, ok := m.MergeBase(3, 4); ok {
		t.Fatal("a criss-cross has one base")
	}
	if b, ok := m.MergeBase(3, 5); !ok || b != 1 {
		t.Fatalf("base %d, %v", b, ok)
	}
}

// Histories read back are checked before they run.
func TestValidate(t *testing.T) {
	h := History{Version: 1, Sessions: 1, Setup: State{"t": {1: {0, 0}}},
		Ops: []Op{{Kind: OpUpdate, Table: "t", ID: 1, Col: 1}}}
	if err := h.Validate(); err != nil || h.Setup["u"] == nil {
		t.Fatalf("%v, setup %v", err, h.Setup)
	}
	for _, bad := range []Op{
		{Kind: OpUpdate, Table: "t", ID: 1, Col: 3},
		{Kind: OpUpdate, Table: Keyless, ID: 1, Val: 0},
		{Kind: OpInsert, Table: "t", ID: 1, Wait: 2},
		{Kind: OpAddColumn, Table: Keyless},
		{Kind: OpInsert, Table: "v", ID: 1},
		{Kind: OpDelete, Table: "t", ID: 0},
		{Kind: "drop"},
		{Kind: OpMerge, Mode: "both"},
		{Kind: OpCommit, Session: 1},
	} {
		h.Ops = []Op{bad}
		if err := h.Validate(); err == nil {
			t.Errorf("%s accepted", bad)
		}
	}
}

// A merge result of another shape is an outcome no model predicts, not a
// crash.
func TestMergeOutcome(t *testing.T) {
	for result, class := range map[string]string{
		`{abc,0,0,"merge successful"}`:     ClassOK,
		`{abc,1,0,"merge successful"}`:     ClassFastForward,
		`{"",0,1,"conflicts found"}`:       ClassConflict,
		`{"",0,0,"Everything up-to-date"}`: ClassUpToDate,
	} {
		if got := mergeOutcome([][]string{{result}}).Class; got != class {
			t.Errorf("%s: %s, want %s", result, got, class)
		}
	}
	for _, rows := range [][][]string{nil, {{}}, {{"{abc}"}}} {
		if got := mergeOutcome(rows).Class; !strings.HasPrefix(got, "unexpected merge result") {
			t.Errorf("%v: %s", rows, got)
		}
	}
}
