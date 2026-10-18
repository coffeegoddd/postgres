package vcfuzz

import (
	"fmt"
	"math/rand"
	"sort"
)

// Op kinds.
const (
	OpInsert    = "insert"    // upsert of (ID, Val, Val2)
	OpUpdate    = "update"    // set column Col of row ID to Val
	OpDelete    = "delete"    // delete row ID
	OpBegin     = "begin"     // open a transaction
	OpCommitTxn = "commit"    // commit the open transaction
	OpRollback  = "rollback"  // roll it back
	OpCommit    = "vc_commit" // commit the branch's working set
	OpBranch    = "branch"    // make branch Ref at HEAD
	OpCheckout  = "checkout"  // switch to branch Ref
	OpReset     = "reset"     // hard reset to Ref
	OpMerge     = "merge"     // merge Ref (NoFF; Mode)
	OpPick      = "pick"      // cherry-pick Ref
	OpRevert    = "revert"    // revert Ref
)

// Merge modes: how a merge's conflicts are handled.
const (
	ModeAuto   = ""       // merge outside a transaction block: refused
	ModeOurs   = "ours"   // in one, resolved with ours, committed
	ModeTheirs = "theirs" // in one, resolved with theirs, committed
	ModeAbort  = "abort"  // in one, aborted
)

// Op is one step of a history, run by one session.
type Op struct {
	Session int    `json:"s"`
	Kind    string `json:"op"`
	Table   string `json:"table,omitempty"`
	ID      int    `json:"id,omitempty"`
	Col     int    `json:"col,omitempty"`
	Val     int    `json:"val,omitempty"`
	Val2    int    `json:"val2,omitempty"`
	Ref     string `json:"ref,omitempty"`
	NoFF    bool   `json:"no_ff,omitempty"`
	Mode    string `json:"mode,omitempty"`
}

func (op Op) String() string {
	switch op.Kind {
	case OpInsert:
		return fmt.Sprintf("s%d insert %s (%d, %d, %d)", op.Session, op.Table, op.ID, op.Val, op.Val2)
	case OpUpdate:
		return fmt.Sprintf("s%d update %s %d col %d = %d", op.Session, op.Table, op.ID, op.Col, op.Val)
	case OpDelete:
		return fmt.Sprintf("s%d delete %s %d", op.Session, op.Table, op.ID)
	case OpMerge:
		return fmt.Sprintf("s%d merge %s no_ff=%v mode=%q", op.Session, op.Ref, op.NoFF, op.Mode)
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
		switch k := r.Intn(100); {
		case k < 34:
			op.Table, op.ID = Tables[r.Intn(len(Tables))], 1+r.Intn(keys)
			switch r.Intn(3) {
			case 0:
				op.Kind, op.Val, op.Val2 = OpInsert, r.Intn(4), r.Intn(4)
			case 1:
				op.Kind, op.Col, op.Val = OpUpdate, r.Intn(2), r.Intn(4)
			default:
				op.Kind = OpDelete
			}
		case k < 40:
			op.Kind = []string{OpBegin, OpCommitTxn, OpRollback, OpCommitTxn}[r.Intn(4)]
		case k < 56:
			op.Kind = OpCommit
		case k < 61:
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
		if !m.Legal(op) || dull(m, op, r) {
			// a session that can't move its branch for uncommitted
			// changes commits them, now and then, so moves happen
			if (op.Kind == OpMerge || op.Kind == OpPick || op.Kind == OpRevert) && !m.Sessions[s].InTxn &&
				!m.anyTxn() && m.Dirty(m.Sessions[s].Branch) {
				op = Op{Session: s, Kind: OpCommit}
			} else {
				continue
			}
		}
		m.Apply(op)
		h.Ops = append(h.Ops, op)
	}
	// close transactions left open
	for s := range m.Sessions {
		if m.Sessions[s].InTxn {
			op := Op{Session: s, Kind: OpCommitTxn}
			m.Apply(op)
			h.Ops = append(h.Ops, op)
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
// filled in), rows of known tables with keys 1..keys and columns 0 or 1,
// and known ops.
func (h *History) Validate() error {
	if h.Version != 1 || h.Sessions < 1 {
		return fmt.Errorf("unsupported history: version %d, %d sessions", h.Version, h.Sessions)
	}
	if h.Setup == nil {
		h.Setup = State{}
	}
	known := map[string]bool{}
	for _, t := range Tables {
		known[t] = true
		if h.Setup[t] == nil {
			h.Setup[t] = map[int]Row{}
		}
	}
	for t, rows := range h.Setup {
		if !known[t] {
			return fmt.Errorf("setup of unknown table %q", t)
		}
		for id := range rows {
			if id < 1 || id > keys {
				return fmt.Errorf("setup row %d of %s out of range", id, t)
			}
		}
	}
	for i, op := range h.Ops {
		switch op.Kind {
		case OpInsert, OpUpdate, OpDelete:
			if !known[op.Table] || op.ID < 1 || op.ID > keys || op.Col < 0 || op.Col > 1 {
				return fmt.Errorf("op %d (%s) writes outside the tables", i+1, op)
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
