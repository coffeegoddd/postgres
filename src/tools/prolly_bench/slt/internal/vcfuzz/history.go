package vcfuzz

import (
	"fmt"
	"math/rand"
	"sort"
)

// Op kinds.
const (
	OpInsert     = "insert"      // upsert of (ID, Val, Val2[, Val3]); a copy of keyless content ID
	OpUpdate     = "update"      // set column Col of row ID to Val; a copy of keyless content ID to content Val
	OpDelete     = "delete"      // delete row ID; a copy of keyless content ID
	OpBegin      = "begin"       // open a transaction
	OpCommitTxn  = "commit"      // commit the open transaction
	OpRollback   = "rollback"    // roll it back
	OpCommit     = "vc_commit"   // commit the branch's working set
	OpBranch     = "branch"      // make branch Ref at HEAD
	OpCheckout   = "checkout"    // switch to branch Ref
	OpReset      = "reset"       // hard reset to Ref
	OpMerge      = "merge"       // merge Ref (NoFF; Mode)
	OpPick       = "pick"        // cherry-pick Ref
	OpRevert     = "revert"      // revert Ref
	OpAddColumn  = "add_column"  // add column c to Table
	OpDropColumn = "drop_column" // drop column c of Table
)

// Merge modes: how a merge's conflicts are handled.
const (
	ModeAuto   = ""       // merge outside a transaction block: refused
	ModeOurs   = "ours"   // in one, resolved with ours, committed
	ModeTheirs = "theirs" // in one, resolved with theirs, committed
	ModeAbort  = "abort"  // in one, aborted
)

// Op is one step of a history, run by one session.  A write with Wait
// waits for the row lock session Wait-1's open transaction holds, and is
// made once that transaction ends.
type Op struct {
	Session int    `json:"s"`
	Kind    string `json:"op"`
	Table   string `json:"table,omitempty"`
	ID      int    `json:"id,omitempty"`
	Col     int    `json:"col,omitempty"`
	Val     int    `json:"val,omitempty"`
	Val2    int    `json:"val2,omitempty"`
	Val3    int    `json:"val3,omitempty"`
	C       bool   `json:"c,omitempty"` // an insert's table has column c
	Wait    int    `json:"wait,omitempty"`
	Ref     string `json:"ref,omitempty"`
	NoFF    bool   `json:"no_ff,omitempty"`
	Mode    string `json:"mode,omitempty"`
}

func (op Op) String() string {
	wait := ""
	if op.Wait > 0 {
		wait = fmt.Sprintf(" (waits on s%d)", op.Wait-1)
	}
	switch {
	case op.Table == Keyless && op.Kind == OpUpdate:
		return fmt.Sprintf("s%d update a copy of %s #%d to #%d%s", op.Session, op.Table, op.ID, op.Val, wait)
	case op.Table == Keyless && (op.Kind == OpInsert || op.Kind == OpDelete):
		return fmt.Sprintf("s%d %s a copy of %s #%d%s", op.Session, op.Kind, op.Table, op.ID, wait)
	}
	switch op.Kind {
	case OpInsert:
		if op.C {
			return fmt.Sprintf("s%d insert %s (%d, %d, %d, %d)%s", op.Session, op.Table, op.ID, op.Val, op.Val2, op.Val3, wait)
		}
		return fmt.Sprintf("s%d insert %s (%d, %d, %d)%s", op.Session, op.Table, op.ID, op.Val, op.Val2, wait)
	case OpUpdate:
		return fmt.Sprintf("s%d update %s %d col %d = %d%s", op.Session, op.Table, op.ID, op.Col, op.Val, wait)
	case OpDelete:
		return fmt.Sprintf("s%d delete %s %d%s", op.Session, op.Table, op.ID, wait)
	case OpMerge:
		return fmt.Sprintf("s%d merge %s no_ff=%v mode=%q", op.Session, op.Ref, op.NoFF, op.Mode)
	case OpAddColumn, OpDropColumn:
		return fmt.Sprintf("s%d %s %s.c", op.Session, op.Kind, op.Table)
	}
	if op.Ref != "" {
		return fmt.Sprintf("s%d %s %s", op.Session, op.Kind, op.Ref)
	}
	return fmt.Sprintf("s%d %s", op.Session, op.Kind)
}

// History is a reproducible history: the rows main starts with, committed,
// and the ops after.
type History struct {
	Version  int   `json:"version"`
	Seed     int64 `json:"seed"`
	Sessions int   `json:"sessions"`
	Setup    State `json:"setup"`
	Ops      []Op  `json:"ops"`
}

const keys = 8 // row keys 1..keys, values 0..3: small, so changes collide

// Generate makes a history of steps ops for the given number of sessions,
// each op legal where the model stands when it runs.
func Generate(seed int64, steps, sessions int) History {
	r := rand.New(rand.NewSource(seed))
	setup := emptyState()
	for _, t := range Tables {
		for id := 1; id <= keys; id++ {
			if r.Intn(3) > 0 {
				setup[t][id] = Row{r.Intn(4), r.Intn(4)}
			}
		}
	}
	for id := 1; id <= keys; id++ {
		if n := r.Intn(4); n > 0 && r.Intn(2) == 0 {
			setup[Keyless][id] = Row{n}
		}
	}
	h := History{Version: 1, Seed: seed, Sessions: sessions, Setup: setup}
	m := NewModel(sessions, setup)
	nbranches := 0
	for len(h.Ops) < steps {
		s := r.Intn(sessions)
		op := Op{Session: s}
		branches := make([]string, 0, len(m.Branches))
		for b := range m.Branches {
			branches = append(branches, b)
		}
		// (map order isn't the seed's)
		sort.Strings(branches)
		// another branch than the session's, mostly
		ref := func() string {
			b := branches[r.Intn(len(branches))]
			if b == m.Sessions[s].Branch && len(branches) > 1 && r.Intn(5) > 0 {
				return ref0(branches, b, r)
			}
			if r.Intn(4) == 0 {
				return fmt.Sprintf("%s~%d", b, 1+r.Intn(2))
			}
			return b
		}
		working := m.Working[m.Sessions[s].Branch]
		switch k := r.Intn(100); {
		case k < 34:
			op.Table, op.ID = []string{"t", "u", Keyless}[r.Intn(3)], 1+r.Intn(keys)
			keyless := op.Table == Keyless
			switch r.Intn(3) {
			case 0:
				op.Kind = OpInsert
				if !keyless {
					op.Val, op.Val2 = r.Intn(4), r.Intn(4)
					if working.HasC(op.Table) {
						op.C, op.Val3 = true, r.Intn(4)
					}
				}
			case 1:
				op.Kind = OpUpdate
				switch {
				case keyless:
					op.Val = 1 + r.Intn(keys)
				case working.HasC(op.Table):
					op.Col, op.Val = r.Intn(3), r.Intn(4)
				default:
					op.Col, op.Val = r.Intn(2), r.Intn(4)
				}
			default:
				op.Kind = OpDelete
			}
			// a row another open transaction wrote: waited on, mostly, where it may
			if h := m.holder(op); h >= 0 && r.Intn(4) > 0 && m.canWait(op, h) {
				op.Wait = h + 1
			}
		case k < 40:
			op.Kind = []string{OpBegin, OpCommitTxn, OpRollback, OpCommitTxn}[r.Intn(4)]
		case k < 43:
			op.Table, op.Kind = Tables[r.Intn(len(Tables))], OpAddColumn
			if working.HasC(op.Table) {
				op.Kind = OpDropColumn
			}
		case k < 58:
			op.Kind = OpCommit
		case k < 62:
			nbranches++
			op.Kind, op.Ref = OpBranch, fmt.Sprintf("b%d", nbranches)
		case k < 70:
			op.Kind, op.Ref = OpCheckout, branches[r.Intn(len(branches))]
		case k < 86:
			op.Kind, op.Ref, op.NoFF = OpMerge, ref(), r.Intn(4) == 0
			op.Mode = []string{ModeAuto, ModeOurs, ModeTheirs, ModeAbort}[r.Intn(4)]
		case k < 91:
			op.Kind, op.Ref = OpPick, ref()
		case k < 95:
			op.Kind, op.Ref = OpRevert, ref()
		default:
			op.Kind, op.Ref = OpReset, "HEAD"
			if r.Intn(2) == 0 {
				op.Ref = ref()
			}
		}
		// a session waiting is let go soon: the transaction it waits on ends
		for _, w := range m.Sessions {
			if w.Waiting != nil && r.Intn(3) == 0 {
				op = Op{Session: w.WaitsOn, Kind: []string{OpCommitTxn, OpCommitTxn, OpRollback}[r.Intn(3)]}
				break
			}
		}
		if !m.Legal(op) || dull(m, op, r) {
			// a session that can't move its branch for uncommitted
			// changes commits them, now and then, so moves happen
			if (op.Kind == OpMerge || op.Kind == OpPick || op.Kind == OpRevert) && m.Legal(Op{Session: s, Kind: OpCommit}) &&
				!m.anyTxn() && m.Dirty(m.Sessions[s].Branch) {
				op = Op{Session: s, Kind: OpCommit}
			} else {
				continue
			}
		}
		m.Apply(op)
		h.Ops = append(h.Ops, op)
	}
	// close transactions left open (those waited on first: their waiters'
	// writes are made as they end)
	for open := true; open; {
		open = false
		for s := range m.Sessions {
			if m.Sessions[s].InTxn && m.Sessions[s].Waiting == nil {
				op := Op{Session: s, Kind: OpCommitTxn}
				m.Apply(op)
				h.Ops = append(h.Ops, op)
				open = true
			}
		}
	}
	return h
}

func ref0(branches []string, not string, r *rand.Rand) string {
	for {
		if b := branches[r.Intn(len(branches))]; b != not {
			return b
		}
	}
}

// dull is whether to pass over a legal op that would most often do
// nothing: a commit of no changes, a merge of what's merged already.
func dull(m *Model, op Op, r *rand.Rand) bool {
	if r.Intn(5) == 0 {
		return false
	}
	switch op.Kind {
	case OpCommit:
		return !m.Dirty(m.Sessions[op.Session].Branch)
	case OpMerge:
		theirs, _ := m.Resolve(op.Session, op.Ref)
		base, _ := m.MergeBase(m.Branches[m.Sessions[op.Session].Branch], theirs)
		return base == theirs
	}
	return false
}

// Validate checks a history read back (a replayed or edited one) holds only
// what the model and engines take: every table's setup (an empty one
// filled in), rows of known tables with keys 1..keys and columns 0 to 2,
// keyless rows of at least one copy, and known ops.
func (h *History) Validate() error {
	if h.Version != 1 || h.Sessions < 1 {
		return fmt.Errorf("unsupported history: version %d, %d sessions", h.Version, h.Sessions)
	}
	if h.Setup == nil {
		h.Setup = State{}
	}
	known := map[string]bool{}
	for _, t := range stateTables {
		known[t] = true
		if h.Setup[t] == nil {
			h.Setup[t] = map[int]Row{}
		}
	}
	for t, rows := range h.Setup {
		if !known[t] {
			return fmt.Errorf("setup of unknown table %q", t)
		}
		for id, r := range rows {
			last := keys
			if t == Columns {
				last = len(Tables)
			}
			if id < 1 || id > last || (t == Keyless || t == Columns) && r[0] < 1 {
				return fmt.Errorf("setup row %d of %s out of range", id, t)
			}
			// (what the engines can build: a table's column c only as
			// Columns' {1}, a keyless content's copies alone, and c's
			// value only in a table that has c)
			switch {
			case t == Columns && r != Row{1}:
				return fmt.Errorf("setup row %d of %s isn't {1}", id, t)
			case t == Keyless && (r[1] != 0 || r[2] != 0):
				return fmt.Errorf("setup row %d of %s holds more than a count of copies", id, t)
			case t != Keyless && t != Columns && r[2] != 0 && !h.Setup.HasC(t):
				return fmt.Errorf("setup row %d of %s sets c, which %s doesn't have", id, t, t)
			}
		}
	}
	for i, op := range h.Ops {
		switch op.Kind {
		case OpInsert, OpUpdate, OpDelete:
			keyless := op.Table == Keyless
			if !(keyless || tableIndex(op.Table) > 0) || op.ID < 1 || op.ID > keys || op.Col < 0 || op.Col > 2 ||
				keyless && op.Kind == OpUpdate && (op.Val < 1 || op.Val > keys) || op.Wait < 0 || op.Wait > h.Sessions {
				return fmt.Errorf("op %d (%s) writes outside the tables", i+1, op)
			}
		case OpAddColumn, OpDropColumn:
			if tableIndex(op.Table) == 0 {
				return fmt.Errorf("op %d (%s) changes an unknown table", i+1, op)
			}
		case OpBegin, OpCommitTxn, OpRollback, OpCommit, OpBranch, OpCheckout, OpReset, OpPick, OpRevert:
		case OpMerge:
			switch op.Mode {
			case ModeAuto, ModeOurs, ModeTheirs, ModeAbort:
			default:
				return fmt.Errorf("op %d: unknown merge mode %q", i+1, op.Mode)
			}
		default:
			return fmt.Errorf("op %d: unknown op %q", i+1, op.Kind)
		}
		if op.Session < 0 || op.Session >= h.Sessions {
			return fmt.Errorf("op %d: no session %d", i+1, op.Session)
		}
	}
	return nil
}

// Reduce removes ops while the history still fails the same way, halving
// the width removed when no removal of that width does.  It is bounded by
// budget runs; the result is smaller, not necessarily minimal.  Ops a
// removal leaves illegal are skipped by every engine when it runs.
func Reduce(h History, budget int, fails func(History) bool) History {
	for width := (len(h.Ops) + 1) / 2; width > 0 && budget > 0; {
		changed := false
		// (after a removal the next chunk is at start: tried there)
		for start := 0; start < len(h.Ops) && budget > 0; {
			end := min(start+width, len(h.Ops))
			trial := h
			trial.Ops = append(append([]Op{}, h.Ops[:start]...), h.Ops[end:]...)
			budget--
			if fails(trial) {
				h, changed = trial, true
			} else {
				start += width
			}
		}
		if !changed {
			width /= 2
		}
	}
	return h
}
