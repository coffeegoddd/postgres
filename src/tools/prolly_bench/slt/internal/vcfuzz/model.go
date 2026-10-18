// Package vcfuzz runs seeded version-control histories (branches, commits,
// merges, cherry-picks, reverts, resets, several sessions) against engines
// and checks each against a reference model of branches and three-way row
// merges, and against invariants of the engine's own.
package vcfuzz

import (
	"fmt"
	"maps"
	"sort"
	"strconv"
	"strings"
)

// Tables are the versioned tables every history writes: (id int PRIMARY
// KEY, a int, b int).
var Tables = []string{"t", "u"}

// Row holds a row's non-key columns, a and b.
type Row [2]int

// State is the rows of every table, by table and key.
type State map[string]map[int]Row

func emptyState() State {
	s := State{}
	for _, t := range Tables {
		s[t] = map[int]Row{}
	}
	return s
}

func (s State) clone() State {
	c := State{}
	for t, rows := range s {
		c[t] = maps.Clone(rows)
	}
	return c
}

func (s State) equal(o State) bool {
	for _, t := range Tables {
		if !maps.Equal(s[t], o[t]) {
			return false
		}
	}
	return true
}

// key is a canonical encoding of the state, for grouping equal states.
func (s State) key() string {
	var b strings.Builder
	for _, t := range Tables {
		ids := make([]int, 0, len(s[t]))
		for id := range s[t] {
			ids = append(ids, id)
		}
		sort.Ints(ids)
		b.WriteString(t + ":")
		for _, id := range ids {
			r := s[t][id]
			fmt.Fprintf(&b, "%d=%d,%d;", id, r[0], r[1])
		}
		b.WriteString("|")
	}
	return b.String()
}

// Conflicts are the keys of each table's rows a merge couldn't merge.
type Conflicts map[string][]int

func (c Conflicts) count() int {
	n := 0
	for _, ids := range c {
		n += len(ids)
	}
	return n
}

// Merge3 merges ours and theirs from base, row by row and, for a row both
// changed, cell by cell.  A row both sides changed differently (a cell
// both changed to different values, a row one side deleted and the other
// changed, a row both added differently) is a conflict, and the result
// keeps ours for it.
func Merge3(base, ours, theirs State) (State, Conflicts) {
	out := emptyState()
	conflicts := Conflicts{}
	for _, t := range Tables {
		ids := map[int]bool{}
		for _, s := range []State{base, ours, theirs} {
			for id := range s[t] {
				ids[id] = true
			}
		}
		sorted := make([]int, 0, len(ids))
		for id := range ids {
			sorted = append(sorted, id)
		}
		sort.Ints(sorted)
		for _, id := range sorted {
			b, inB := base[t][id]
			o, inO := ours[t][id]
			th, inT := theirs[t][id]
			same := func(x Row, inX bool, y Row, inY bool) bool { return inX == inY && (!inX || x == y) }
			switch {
			case same(o, inO, th, inT), same(th, inT, b, inB):
				if inO {
					out[t][id] = o
				}
			case same(o, inO, b, inB):
				if inT {
					out[t][id] = th
				}
			case inO && inT && inB:
				var r Row
				ok := true
				for c := range r {
					switch {
					case o[c] == th[c], th[c] == b[c]:
						r[c] = o[c]
					case o[c] == b[c]:
						r[c] = th[c]
					default:
						ok = false
					}
				}
				if ok {
					out[t][id] = r
				} else {
					out[t][id] = o
					conflicts[t] = append(conflicts[t], id)
				}
			default:
				if inO {
					out[t][id] = o
				}
				conflicts[t] = append(conflicts[t], id)
			}
		}
	}
	return out, conflicts
}

// Commit is a commit of the model: its parents (first parent first) and
// its rows.
type Commit struct {
	Parents []int
	State   State
}

// Model is the reference implementation: branches pointing at commits,
// a working set per branch, sessions each on a branch, and a session's
// open transaction holding the row writes no other session sees yet.
type Model struct {
	Commits  []Commit
	Branches map[string]int
	Working  map[string]State
	Sessions []ModelSession
	// Used is the branches a session has been on.
	Used map[string]bool
}

type ModelSession struct {
	Branch string
	// Pending is the session's open transaction's row writes, or nil.
	Pending []Op
	InTxn   bool
}

func NewModel(sessions int, setup State) *Model {
	m := &Model{
		Commits:  []Commit{{State: setup.clone()}},
		Branches: map[string]int{"main": 0},
		Working:  map[string]State{"main": setup.clone()},
		Used:     map[string]bool{"main": true},
	}
	for i := 0; i < sessions; i++ {
		m.Sessions = append(m.Sessions, ModelSession{Branch: "main"})
	}
	return m
}

func (m *Model) head(branch string) Commit { return m.Commits[m.Branches[branch]] }

// Visible is the rows a session sees: its branch's working set, with its
// open transaction's writes.
func (m *Model) Visible(s int) State {
	ms := m.Sessions[s]
	v := m.Working[ms.Branch].clone()
	for _, op := range ms.Pending {
		applyWrite(v, op)
	}
	return v
}

// Dirty is whether a branch's working set differs from its head.
func (m *Model) Dirty(branch string) bool {
	return !m.Working[branch].equal(m.head(branch).State)
}

func applyWrite(s State, op Op) {
	rows := s[op.Table]
	switch op.Kind {
	case OpInsert:
		rows[op.ID] = Row{op.Val, op.Val2}
	case OpUpdate:
		if r, ok := rows[op.ID]; ok {
			r[op.Col] = op.Val
			rows[op.ID] = r
		}
	case OpDelete:
		delete(rows, op.ID)
	}
}

// Resolve resolves a ref: a branch, HEAD (the session's branch), or either
// followed by ~N (N first parents back).
func (m *Model) Resolve(s int, ref string) (int, bool) {
	name, back, _ := strings.Cut(ref, "~")
	if name == "HEAD" {
		name = m.Sessions[s].Branch
	}
	c, ok := m.Branches[name]
	if !ok {
		return 0, false
	}
	n := 0
	if back != "" {
		var err error
		if n, err = strconv.Atoi(back); err != nil {
			return 0, false
		}
	}
	for ; n > 0; n-- {
		if len(m.Commits[c].Parents) == 0 {
			return 0, false
		}
		c = m.Commits[c].Parents[0]
	}
	return c, true
}

func (m *Model) ancestors(c int) map[int]bool {
	seen := map[int]bool{}
	stack := []int{c}
	for len(stack) > 0 {
		x := stack[len(stack)-1]
		stack = stack[:len(stack)-1]
		if seen[x] {
			continue
		}
		seen[x] = true
		stack = append(stack, m.Commits[x].Parents...)
	}
	return seen
}

// MergeBase is the common ancestor of two commits no other common ancestor
// descends from, or false if there are several (a criss-cross, where the
// model doesn't say which an engine takes).
func (m *Model) MergeBase(x, y int) (int, bool) {
	ax, ay := m.ancestors(x), m.ancestors(y)
	common := map[int]bool{}
	for c := range ax {
		if ay[c] {
			common[c] = true
		}
	}
	// (what's beneath a common one: its parents' ancestors, walked once)
	beneath := map[int]bool{}
	var stack []int
	for c := range common {
		stack = append(stack, m.Commits[c].Parents...)
	}
	for len(stack) > 0 {
		c := stack[len(stack)-1]
		stack = stack[:len(stack)-1]
		if beneath[c] {
			continue
		}
		beneath[c] = true
		stack = append(stack, m.Commits[c].Parents...)
	}
	var maximal []int
	for c := range common {
		if !beneath[c] {
			maximal = append(maximal, c)
		}
	}
	if len(maximal) != 1 {
		return 0, false
	}
	return maximal[0], true
}

func (m *Model) commit(branch string, parents []int, s State) {
	m.Commits = append(m.Commits, Commit{Parents: parents, State: s.clone()})
	m.Branches[branch] = len(m.Commits) - 1
	m.Working[branch] = s.clone()
}

func (m *Model) anyTxn() bool {
	for _, s := range m.Sessions {
		if s.InTxn {
			return true
		}
	}
	return false
}

// writing is whether another session's open transaction has written on
// the branch.
func (m *Model) writing(s int, branch string) bool {
	for i, o := range m.Sessions {
		if i != s && o.InTxn && o.Branch == branch && len(o.Pending) > 0 {
			return true
		}
	}
	return false
}

// locked is whether another session's open transaction has written the row.
func (m *Model) locked(s int, table string, id int) bool {
	for i, o := range m.Sessions {
		if i == s || !o.InTxn {
			continue
		}
		for _, p := range o.Pending {
			if p.Table == table && p.ID == id {
				return true
			}
		}
	}
	return false
}

// Legal is whether an op can run in the model's state.  Generated
// histories hold only legal ops; a reduced one can hold ops no longer
// legal, and they are skipped, by every engine.
func (m *Model) Legal(op Op) bool {
	if op.Session < 0 || op.Session >= len(m.Sessions) {
		return false
	}
	ms := m.Sessions[op.Session]
	switch op.Kind {
	case OpInsert, OpUpdate, OpDelete:
		// (rows another open transaction wrote would wait on it)
		return !m.locked(op.Session, op.Table, op.ID)
	case OpBegin:
		return !ms.InTxn
	case OpCommitTxn, OpRollback:
		return ms.InTxn
	}
	if ms.InTxn {
		return false
	}
	switch op.Kind {
	case OpCommit, OpBranch, OpCheckout:
		// (a commit, or a branch made, while another session's open
		// transaction writes on the branch doesn't wait: the commit takes
		// the working set as committed, without that transaction's rows)
		if op.Kind == OpCheckout {
			// A branch's first use makes its tables, copying the
			// session's branch's under a lock that waits for the
			// transactions writing there (in prolly; Dolt's checkout
			// never waits).
			_, ok := m.Branches[op.Ref]
			return ok && (m.Used[op.Ref] || !m.writing(op.Session, ms.Branch))
		}
		if op.Kind == OpBranch {
			_, exists := m.Branches[op.Ref]
			return !exists && op.Ref != ""
		}
		return true
	}
	// the rest move the branch: none while any transaction is open; a
	// hard reset drops uncommitted changes, the others need none
	if m.anyTxn() {
		return false
	}
	if op.Kind == OpReset {
		_, ok := m.Resolve(op.Session, op.Ref)
		return ok
	}
	if m.Dirty(ms.Branch) {
		return false
	}
	head := m.Branches[ms.Branch]
	switch op.Kind {
	case OpMerge:
		theirs, ok := m.Resolve(op.Session, op.Ref)
		if !ok {
			return false
		}
		_, ok = m.MergeBase(head, theirs)
		return ok
	case OpPick, OpRevert:
		c, ok := m.Resolve(op.Session, op.Ref)
		return ok && len(m.Commits[c].Parents) == 1
	}
	return false
}

// Apply runs a legal op and returns its outcome.
func (m *Model) Apply(op Op) Outcome {
	ms := &m.Sessions[op.Session]
	branch := ms.Branch
	head := m.Branches[branch]
	switch op.Kind {
	case OpInsert, OpUpdate, OpDelete:
		if ms.InTxn {
			ms.Pending = append(ms.Pending, op)
		} else {
			applyWrite(m.Working[branch], op)
		}
		return Outcome{Class: ClassOK}
	case OpBegin:
		ms.InTxn = true
		return Outcome{Class: ClassOK}
	case OpCommitTxn:
		for _, p := range ms.Pending {
			applyWrite(m.Working[branch], p)
		}
		ms.Pending, ms.InTxn = nil, false
		return Outcome{Class: ClassOK}
	case OpRollback:
		ms.Pending, ms.InTxn = nil, false
		return Outcome{Class: ClassOK}
	case OpCommit:
		if !m.Dirty(branch) {
			return Outcome{Class: ClassUnchanged}
		}
		m.commit(branch, []int{head}, m.Working[branch])
		return Outcome{Class: ClassOK}
	case OpBranch:
		m.Branches[op.Ref] = head
		m.Working[op.Ref] = m.Commits[head].State.clone()
		return Outcome{Class: ClassOK}
	case OpCheckout:
		ms.Branch = op.Ref
		m.Used[op.Ref] = true
		return Outcome{Class: ClassOK}
	case OpReset:
		c, _ := m.Resolve(op.Session, op.Ref)
		m.Branches[branch] = c
		m.Working[branch] = m.Commits[c].State.clone()
		return Outcome{Class: ClassOK}
	case OpMerge:
		theirs, _ := m.Resolve(op.Session, op.Ref)
		base, _ := m.MergeBase(head, theirs)
		switch {
		case base == theirs:
			return Outcome{Class: ClassUpToDate}
		case base == head && !op.NoFF:
			m.Branches[branch] = theirs
			m.Working[branch] = m.Commits[theirs].State.clone()
			return Outcome{Class: ClassFastForward}
		}
		merged, conflicts := Merge3(m.Commits[base].State, m.Commits[head].State, m.Commits[theirs].State)
		if conflicts.count() > 0 {
			switch op.Mode {
			case ModeOurs, ModeTheirs:
				for t, ids := range conflicts {
					for _, id := range ids {
						if op.Mode == ModeOurs {
							continue
						}
						if r, ok := m.Commits[theirs].State[t][id]; ok {
							merged[t][id] = r
						} else {
							delete(merged[t], id)
						}
					}
				}
				m.commit(branch, []int{head, theirs}, merged)
				return Outcome{Class: ClassConflict, Conflicts: conflicts.count()}
			case ModeAbort:
				return Outcome{Class: ClassConflict, Conflicts: conflicts.count()}
			}
			return Outcome{Class: ClassConflict}
		}
		m.commit(branch, []int{head, theirs}, merged)
		return Outcome{Class: ClassOK}
	case OpPick, OpRevert:
		c, _ := m.Resolve(op.Session, op.Ref)
		parent := m.Commits[c].Parents[0]
		base, theirs := parent, c
		if op.Kind == OpRevert {
			base, theirs = c, parent
		}
		merged, conflicts := Merge3(m.Commits[base].State, m.Commits[head].State, m.Commits[theirs].State)
		if conflicts.count() > 0 {
			return Outcome{Class: ClassConflict}
		}
		if merged.equal(m.Commits[head].State) {
			return Outcome{Class: ClassUnchanged}
		}
		m.commit(branch, []int{head}, merged)
		return Outcome{Class: ClassOK}
	}
	panic("unknown op " + op.Kind)
}
