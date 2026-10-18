package vcfuzz

import (
	"encoding/json"
	"reflect"
	"testing"
)

func rows(t map[int]Row) State {
	s := emptyState()
	s["t"] = t
	return s
}

// The model's merge is the three-way row merge the corpus's truth tables
// hold engines to.
func TestMerge3(t *testing.T) {
	base := rows(map[int]Row{1: {1, 1}, 2: {2, 2}, 3: {3, 3}, 4: {4, 4}, 7: {7, 7}})
	ours := rows(map[int]Row{1: {10, 1}, 3: {30, 3}, 4: {4, 4}, 5: {5, 5}, 6: {6, 6}, 7: {70, 7}})
	theirs := rows(map[int]Row{1: {11, 21}, 2: {22, 2}, 4: {4, 40}, 5: {5, 5}, 6: {6, 7}, 7: {7, 77}})
	merged, conflicts := Merge3(base, ours, theirs)
	want := rows(map[int]Row{
		1: {10, 1}, // a changed both ways: conflict, ours kept
		3: {30, 3}, // changed here, deleted there: conflict
		4: {4, 40}, // changed there only
		5: {5, 5},  // added the same both ways
		6: {6, 6},  // added differently: conflict
		7: {70, 77},
	})
	if !merged.equal(want) {
		t.Fatalf("merged %s, want %s", merged.key(), want.key())
	}
	// 2: deleted here, changed there
	if !reflect.DeepEqual(conflicts, Conflicts{"t": {1, 2, 3, 6}}) {
		t.Fatalf("conflicts %v", conflicts)
	}
}

func TestModelMerges(t *testing.T) {
	m := NewModel(1, rows(map[int]Row{1: {1, 1}}))
	apply := func(op Op, class string) {
		t.Helper()
		if !m.Legal(op) {
			t.Fatalf("%s illegal", op)
		}
		if o := m.Apply(op); o.Class != class {
			t.Fatalf("%s: %s, want %s", op, o.Class, class)
		}
	}
	apply(Op{Kind: OpBranch, Ref: "x"}, ClassOK)
	apply(Op{Kind: OpUpdate, Table: "t", ID: 1, Col: 0, Val: 5}, ClassOK)
	if m.Legal(Op{Kind: OpMerge, Ref: "x"}) {
		t.Fatal("a merge over uncommitted changes is legal")
	}
	apply(Op{Kind: OpCommit}, ClassOK)
	apply(Op{Kind: OpCommit}, ClassUnchanged)
	apply(Op{Kind: OpMerge, Ref: "x"}, ClassUpToDate)
	apply(Op{Kind: OpCheckout, Ref: "x"}, ClassOK)
	apply(Op{Kind: OpMerge, Ref: "main"}, ClassFastForward)
	apply(Op{Kind: OpUpdate, Table: "t", ID: 1, Col: 0, Val: 6}, ClassOK)
	apply(Op{Kind: OpCommit}, ClassOK)
	apply(Op{Kind: OpCheckout, Ref: "main"}, ClassOK)
	apply(Op{Kind: OpUpdate, Table: "t", ID: 1, Col: 0, Val: 7}, ClassOK)
	apply(Op{Kind: OpCommit}, ClassOK)
	apply(Op{Kind: OpMerge, Ref: "x"}, ClassConflict)
	apply(Op{Kind: OpMerge, Ref: "x", Mode: ModeTheirs}, ClassConflict)
	if got := m.Visible(0)["t"][1]; got != (Row{6, 1}) {
		t.Fatalf("resolved with theirs: %v", got)
	}
	head := m.head("main")
	if len(head.Parents) != 2 {
		t.Fatalf("resolved merge's parents %v", head.Parents)
	}
	// a revert of x's change conflicts with nothing now
	apply(Op{Kind: OpRevert, Ref: "x"}, ClassOK)
	if got := m.Visible(0)["t"][1]; got != (Row{5, 1}) {
		t.Fatalf("reverted: %v", got)
	}
	apply(Op{Kind: OpPick, Ref: "x"}, ClassOK)
	apply(Op{Kind: OpPick, Ref: "x"}, ClassUnchanged)
}

// A transaction's writes are its session's alone until it commits; other
// sessions' commits don't take them, and others can't write its rows.
func TestModelTransactions(t *testing.T) {
	m := NewModel(2, rows(map[int]Row{1: {1, 1}}))
	m.Apply(Op{Session: 0, Kind: OpBegin})
	m.Apply(Op{Session: 0, Kind: OpInsert, Table: "t", ID: 2, Val: 2, Val2: 2})
	if _, ok := m.Visible(1)["t"][2]; ok {
		t.Fatal("another session sees an open transaction's row")
	}
	if m.Legal(Op{Session: 1, Kind: OpDelete, Table: "t", ID: 2}) {
		t.Fatal("a write of a row an open transaction wrote is legal")
	}
	if m.Legal(Op{Session: 1, Kind: OpMerge, Ref: "main"}) {
		t.Fatal("a merge while a transaction is open is legal")
	}
	if o := m.Apply(Op{Session: 1, Kind: OpCommit}); o.Class != ClassUnchanged {
		t.Fatalf("a commit took an open transaction's row: %v", o)
	}
	m.Apply(Op{Session: 0, Kind: OpCommitTxn})
	if !m.Dirty("main") {
		t.Fatal("a committed transaction's row isn't an uncommitted change")
	}
}

// Generated histories are reproducible, legal throughout, and leave no
// transaction open; they survive a round trip through JSON.
func TestGenerate(t *testing.T) {
	kinds := map[string]int{}
	for seed := int64(1); seed <= 20; seed++ {
		h := Generate(seed, 80, 1+int(seed%3))
		if !reflect.DeepEqual(h, Generate(seed, 80, 1+int(seed%3))) {
			t.Fatalf("seed %d isn't reproducible", seed)
		}
		m := NewModel(h.Sessions, h.Setup)
		for _, op := range h.Ops {
			if !m.Legal(op) {
				t.Fatalf("seed %d: %s illegal", seed, op)
			}
			m.Apply(op)
			kinds[op.Kind]++
		}
		for _, s := range m.Sessions {
			if s.InTxn {
				t.Fatalf("seed %d leaves a transaction open", seed)
			}
		}
		b, err := json.Marshal(h)
		if err != nil {
			t.Fatal(err)
		}
		var back History
		if err := json.Unmarshal(b, &back); err != nil {
			t.Fatal(err)
		}
		if !reflect.DeepEqual(back, h) {
			t.Fatalf("seed %d changes through JSON", seed)
		}
	}
	// every kind of op, and moves of branches often
	for _, k := range []string{OpInsert, OpUpdate, OpDelete, OpBegin, OpCommitTxn, OpRollback, OpCommit,
		OpBranch, OpCheckout, OpReset, OpMerge, OpPick, OpRevert} {
		if kinds[k] == 0 {
			t.Errorf("no %s ops in %v", k, kinds)
		}
	}
	if kinds[OpMerge]+kinds[OpPick]+kinds[OpRevert] < 100 {
		t.Errorf("few merges, picks and reverts: %v", kinds)
	}
}

func TestReduce(t *testing.T) {
	h := Generate(3, 60, 2)
	target := h.Ops[41]
	fails := func(trial History) bool {
		for _, op := range trial.Ops {
			if op == target {
				return true
			}
		}
		return false
	}
	r := Reduce(h, 200, fails)
	if len(r.Ops) != 1 || r.Ops[0] != target {
		t.Fatalf("reduced to %v", r.Ops)
	}
}
