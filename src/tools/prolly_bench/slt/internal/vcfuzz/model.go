// Package vcfuzz runs seeded version-control histories (branches, commits,
// merges, cherry-picks, reverts, resets, schema changes, several sessions,
// some waiting on others' row locks) against engines and checks each
// against a reference model of branches and three-way row merges, and
// against invariants of the engine's own.
package vcfuzz

import (
	"fmt"
	"maps"
	"sort"
	"strconv"
	"strings"
)

// Tables are the keyed versioned tables every history writes: (id int
// PRIMARY KEY, a int, b int), and c int while the history has added it.
var Tables = []string{"t", "u"}

// Keyless is the versioned table without a key, (a int, b int): its rows
// are counted by content, each content numbered 1..keys (a is (n-1)/4, b
// is (n-1)%4).
const Keyless = "k"

// Columns is the state's record of which keyed tables have column c: row
// i+1 is there, as {1}, while Tables[i] has it.
const Columns = "columns"

// stateTables are a State's tables: the rows of each, and the columns.
var stateTables = []string{"t", "u", Keyless, Columns}

// Row holds a keyed row's non-key columns a, b and c (c 0 for NULL, and
// while its table has no column c); a keyless row's copies are Row[0].
type Row [3]int

// State is the rows of every table, by table and key.
type State map[string]map[int]Row

func emptyState() State {
	s := State{}
	for _, t := range stateTables {
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
	for _, t := range stateTables {
		if !maps.Equal(s[t], o[t]) {
			return false
		}
	}
	return true
}

// key is a canonical encoding of the state, for grouping equal states.
func (s State) key() string {
	var b strings.Builder
	for _, t := range stateTables {
		ids := make([]int, 0, len(s[t]))
		for id := range s[t] {
			ids = append(ids, id)
		}
		sort.Ints(ids)
		b.WriteString(t + ":")
		for _, id := range ids {
			r := s[t][id]
			switch t {
			case Keyless:
				fmt.Fprintf(&b, "%dx%d;", id, r[0])
			case Columns:
				fmt.Fprintf(&b, "%d;", id)
			default:
				fmt.Fprintf(&b, "%d=%d,%d,%d;", id, r[0], r[1], r[2])
			}
		}
		b.WriteString("|")
	}
	return b.String()
}

// tableIndex is a keyed table's row in Columns, or 0.
func tableIndex(table string) int {
	for i, t := range Tables {
		if t == table {
			return i + 1
		}
	}
	return 0
}

// HasC is whether keyed table has column c.
func (s State) HasC(table string) bool {
	_, ok := s[Columns][tableIndex(table)]
	return ok
}

// dropC drops column c of a keyed table, and its values.
func (s State) dropC(table string) {
	delete(s[Columns], tableIndex(table))
	for id, r := range s[table] {
		r[2] = 0
		s[table][id] = r
	}
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

// LostChange is whether a merge would lose changes: a side kept column c
// of a table the other dropped, and changed its value in a row both base
// and that side have (a row added or deleted loses the column as every
// row does).  The merge is refused.
func LostChange(base, ours, theirs State) bool {
	for _, t := range Tables {
		if !base.HasC(t) {
			continue
		}
		for _, sides := range [][2]State{{ours, theirs}, {theirs, ours}} {
			kept, dropped := sides[0], sides[1]
			if !kept.HasC(t) || dropped.HasC(t) {
				continue
			}
			for id, b := range base[t] {
				if k, ok := kept[t][id]; ok && k[2] != b[2] {
					return true
				}
			}
		}
	}
	return false
}

// Merge3 merges ours and theirs from base: the columns, then the rows.
// Column c is a table's if both sides keep it or one side added it; a
// column dropped is no change to the rows it leaves.  Keyed rows merge row
// by row and, for a row both changed, cell by cell.  A row both sides
// changed differently (a cell both changed to different values, a row one
// side deleted and the other changed, a row both added differently) is a
// conflict, and the result keeps ours for it.  A keyless row changed by
// the same number of copies on both sides is changed once; otherwise it
// has the base's copies, those each side added, less the most either
// removed; it never conflicts.
func Merge3(base, ours, theirs State) (State, Conflicts) {
	out := emptyState()
	conflicts := Conflicts{}
	mergeKeyed(Columns, base, ours, theirs, out, conflicts)
	// (a column dropped: gone from every side's rows)
	for _, t := range Tables {
		if !out.HasC(t) && base.HasC(t) {
			base, ours, theirs = base.clone(), ours.clone(), theirs.clone()
			for _, s := range []State{base, ours, theirs} {
				s.dropC(t)
			}
		}
	}
	for _, t := range Tables {
		mergeKeyed(t, base, ours, theirs, out, conflicts)
	}
	ids := map[int]bool{}
	for _, s := range []State{base, ours, theirs} {
		for id := range s[Keyless] {
			ids[id] = true
		}
	}
	for id := range ids {
		b := base[Keyless][id][0]
		do, dt := ours[Keyless][id][0]-b, theirs[Keyless][id][0]-b
		n := b + do
		if do != dt {
			n = b + max(do, 0) + max(dt, 0) - max(-do, -dt, 0)
		}
		if n > 0 {
			out[Keyless][id] = Row{n}
		}
	}
	return out, conflicts
}

func mergeKeyed(t string, base, ours, theirs, out State, conflicts Conflicts) {
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
	// Waiting is the write the session waits to make, for the row lock
	// the open transaction of session WaitsOn holds, or nil.
	Waiting *Op
	WaitsOn int
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
	if op.Table == Keyless {
		switch op.Kind {
		case OpInsert:
			rows[op.ID] = Row{rows[op.ID][0] + 1}
		case OpUpdate, OpDelete:
			n := rows[op.ID][0]
			if n == 0 {
				return
			}
			if n == 1 {
				delete(rows, op.ID)
			} else {
				rows[op.ID] = Row{n - 1}
			}
			if op.Kind == OpUpdate {
				rows[op.Val] = Row{rows[op.Val][0] + 1}
			}
		}
		return
	}
	switch op.Kind {
	case OpInsert:
		rows[op.ID] = Row{op.Val, op.Val2, op.Val3}
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
		if s.InTxn || s.Waiting != nil {
			return true
		}
	}
	return false
}

// writing is whether another session's open transaction has written on
// the branch, or another session waits to write there.
func (m *Model) writing(s int, branch string) bool {
	for i, o := range m.Sessions {
		if i != s && o.Branch == branch && (o.InTxn && len(o.Pending) > 0 || o.Waiting != nil) {
			return true
		}
	}
	return false
}

// touches is the rows a write may change: its row, and a keyless update's
// content too.
func touches(op Op) []int {
	if op.Table == Keyless && op.Kind == OpUpdate {
		return []int{op.ID, op.Val}
	}
	return []int{op.ID}
}

// holder is the other session whose open transaction has written a row op
// may change, or -1.  (Its writes are made again on the working set as it
// commits: so none of those rows may change meanwhile.)
func (m *Model) holder(op Op) int {
	for i, o := range m.Sessions {
		if i == op.Session || !o.InTxn {
			continue
		}
		for _, p := range o.Pending {
			if p.Table != op.Table {
				continue
			}
			for _, x := range touches(p) {
				for _, y := range touches(op) {
					if x == y {
						return i
					}
				}
			}
		}
	}
	return -1
}

// canWait is whether op's session may wait on holder h for op's row: a
// keyed row (a keyless write picks its copy before it waits, and finds it
// gone after) on the same branch, committed there, so h's first write of
// it locked it, and h's writes of it since were made on it (no insert
// after a delete, which would make another row than the one waited on);
// with no other session waiting for it, nor on op's, and h not waiting
// itself, so no session waits on a session that waits (and none
// deadlocks).
func (m *Model) canWait(op Op, h int) bool {
	hs := m.Sessions[h]
	if op.Table == Keyless || hs.Waiting != nil || hs.Branch != m.Sessions[op.Session].Branch {
		return false
	}
	if _, ok := m.Working[hs.Branch][op.Table][op.ID]; !ok {
		return false
	}
	deleted := false
	for _, p := range hs.Pending {
		if p.Table == op.Table && p.ID == op.ID {
			if deleted && p.Kind == OpInsert {
				return false
			}
			deleted = deleted || p.Kind == OpDelete
		}
	}
	for _, o := range m.Sessions {
		if o.Waiting != nil && (o.WaitsOn == op.Session || o.Waiting.Table == op.Table && o.Waiting.ID == op.ID) {
			return false
		}
	}
	return true
}

// Legal is whether an op can run in the model's state.  Generated
// histories hold only legal ops; a reduced one can hold ops no longer
// legal, and they are skipped, by every engine.
func (m *Model) Legal(op Op) bool {
	if op.Session < 0 || op.Session >= len(m.Sessions) {
		return false
	}
	ms := m.Sessions[op.Session]
	if ms.Waiting != nil {
		return false
	}
	switch op.Kind {
	case OpInsert, OpUpdate, OpDelete:
		if op.Table == Keyless {
			if op.Kind == OpUpdate && (op.Val < 1 || op.Val > keys) {
				return false
			}
		} else {
			// (a write names column c where its table has it)
			working := m.Working[ms.Branch]
			if op.Kind == OpInsert && op.C != working.HasC(op.Table) ||
				op.Kind == OpUpdate && op.Col == 2 && !working.HasC(op.Table) {
				return false
			}
		}
		// (rows another open transaction wrote: waited on, if the op says
		// so and may)
		h := m.holder(op)
		if op.Wait == 0 {
			return h < 0
		}
		return h == op.Wait-1 && m.canWait(op, h)
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
	// the rest move the branch or change its tables: none while any
	// transaction is open (each would wait on those that read the tables);
	// a hard reset drops uncommitted changes, the others need none
	if m.anyTxn() {
		return false
	}
	switch op.Kind {
	case OpAddColumn, OpDropColumn:
		return tableIndex(op.Table) > 0 && m.Working[ms.Branch].HasC(op.Table) == (op.Kind == OpDropColumn)
	case OpReset:
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

// write makes a session's row write: its open transaction's, or committed.
func (m *Model) write(s int, op Op) {
	ms := &m.Sessions[s]
	if ms.InTxn {
		ms.Pending = append(ms.Pending, op)
	} else {
		applyWrite(m.Working[ms.Branch], op)
	}
}

// Apply runs a legal op and returns its outcome.
func (m *Model) Apply(op Op) Outcome {
	ms := &m.Sessions[op.Session]
	branch := ms.Branch
	head := m.Branches[branch]
	switch op.Kind {
	case OpInsert, OpUpdate, OpDelete:
		if op.Wait > 0 {
			w := op
			ms.Waiting, ms.WaitsOn = &w, op.Wait-1
			return Outcome{Class: ClassWaiting}
		}
		m.write(op.Session, op)
		return Outcome{Class: ClassOK}
	case OpBegin:
		ms.InTxn = true
		return Outcome{Class: ClassOK}
	case OpCommitTxn, OpRollback:
		if op.Kind == OpCommitTxn {
			for _, p := range ms.Pending {
				applyWrite(m.Working[branch], p)
			}
		}
		ms.Pending, ms.InTxn = nil, false
		// (its row locks released, the writes waiting for them are made)
		for i := range m.Sessions {
			if w := m.Sessions[i].Waiting; w != nil && m.Sessions[i].WaitsOn == op.Session {
				m.Sessions[i].Waiting = nil
				m.write(i, *w)
			}
		}
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
	case OpAddColumn:
		m.Working[branch][Columns][tableIndex(op.Table)] = Row{1}
		return Outcome{Class: ClassOK}
	case OpDropColumn:
		m.Working[branch].dropC(op.Table)
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
		b, o, t := m.Commits[base].State, m.Commits[head].State, m.Commits[theirs].State
		if LostChange(b, o, t) {
			return Outcome{Class: ClassLostChange}
		}
		merged, conflicts := Merge3(b, o, t)
		if conflicts.count() > 0 {
			switch op.Mode {
			case ModeOurs, ModeTheirs:
				for tb, ids := range conflicts {
					for _, id := range ids {
						if op.Mode == ModeOurs {
							continue
						}
						if r, ok := t[tb][id]; ok {
							if tb != Columns && !merged.HasC(tb) {
								r[2] = 0
							}
							merged[tb][id] = r
						} else {
							delete(merged[tb], id)
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
		b, o, t := m.Commits[base].State, m.Commits[head].State, m.Commits[theirs].State
		if LostChange(b, o, t) {
			return Outcome{Class: ClassLostChange}
		}
		merged, conflicts := Merge3(b, o, t)
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
