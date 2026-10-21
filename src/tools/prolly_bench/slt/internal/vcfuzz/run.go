package vcfuzz

import (
	"fmt"
	"sort"
)

// Failure is where an engine and the model, or an engine and an
// invariant, first disagree.
type Failure struct {
	Kind     string `json:"kind"`
	Step     int    `json:"step"`
	Op       Op     `json:"op"`
	Expected any    `json:"expected"`
	Got      any    `json:"got"`
}

func (f *Failure) Error() string {
	return fmt.Sprintf("step %d (%s): %s: expected %v, got %v", f.Step, f.Op, f.Kind, f.Expected, f.Got)
}

// Signature identifies a failure apart from where in a history it falls,
// so a reduced history can be checked to fail the same way: its kind and
// op, and, for an outcome, the outcome classes.  Rows, commit ids and
// hashes, which any removal of an op changes, aren't in it.
func (f *Failure) Signature() string {
	sig := f.Kind + ":" + f.Op.Kind
	if want, ok := f.Expected.(Outcome); ok {
		if got, ok := f.Got.(Outcome); ok {
			sig += ":" + want.Class + ":" + got.Class
		}
	}
	return sig
}

// Report is what a run did: the ops it ran and skipped, the outcomes by
// op ("merge conflict": 3), and its failure, if any.
type Report struct {
	Ran      int            `json:"ran"`
	Skipped  int            `json:"skipped"`
	Outcomes map[string]int `json:"outcomes"`
	Failure  *Failure       `json:"failure,omitempty"`
}

// Run runs a history on an engine and on the model, and checks, after
// every op: the op's outcome; each session's branch, rows, and (outside
// a transaction) whether its branch has uncommitted changes.  For an
// engine with commit identities, it checks each branch's head is the
// commit the model has there, and that commits with the same rows have
// the same root hash and commits with different rows different ones.
// Ops the model holds illegal are skipped.  An error is the harness's.
func Run(h History, open Opener) (Report, error) {
	r := Report{Outcomes: map[string]int{}}
	e, err := open(h)
	if err != nil {
		return r, err
	}
	defer e.Close()
	m := NewModel(h.Sessions, h.Setup)
	ids := map[int]string{}       // the model's commits' ids in the engine
	commits := map[string]int{}   // and back
	roots := map[string]string{}  // a state's root hash
	states := map[string]string{} // and back
	identities, _ := e.(Identities)
	check := func(step int, op Op) (*Failure, error) {
		for s := range m.Sessions {
			// (a session waiting on a row lock can't be asked: it is, once
			// the transaction holding the lock ends)
			if m.Sessions[s].Waiting != nil {
				continue
			}
			fail := func(kind string, expected, got any) *Failure {
				return &Failure{Kind: fmt.Sprintf("session %d %s", s, kind), Step: step, Op: op, Expected: expected, Got: got}
			}
			branch, err := e.Branch(s)
			if err != nil {
				return nil, err
			}
			if branch != m.Sessions[s].Branch {
				return fail("branch", m.Sessions[s].Branch, branch), nil
			}
			rows, err := e.Visible(s)
			if err != nil {
				return nil, err
			}
			if want := m.Visible(s); !rows.equal(want) {
				return fail("rows", want.key(), rows.key()), nil
			}
			if m.Sessions[s].InTxn {
				continue
			}
			dirty, err := e.Dirty(s)
			if err != nil {
				return nil, err
			}
			if want := m.Dirty(branch); dirty != want {
				return fail("uncommitted changes", want, dirty), nil
			}
		}
		if identities == nil {
			return nil, nil
		}
		heads, err := identities.Heads()
		if err != nil {
			return nil, err
		}
		branches := make([]string, 0, len(m.Branches))
		for b := range m.Branches {
			branches = append(branches, b)
		}
		sort.Strings(branches)
		for _, b := range branches {
			fail := func(kind string, expected, got any) *Failure {
				return &Failure{Kind: "branch " + b + " " + kind, Step: step, Op: op, Expected: expected, Got: got}
			}
			c, id := m.Branches[b], heads[b]
			if id == "" {
				return fail("missing", c, nil), nil
			}
			if known, ok := ids[c]; ok {
				if known != id {
					return fail("head", known, id), nil
				}
				continue
			}
			if other, ok := commits[id]; ok {
				return fail("head is another commit", c, other), nil
			}
			ids[c], commits[id] = id, c
			root, err := identities.RootHash(id)
			if err != nil {
				return nil, err
			}
			key := m.Commits[c].State.key()
			if r, ok := roots[key]; ok && r != root {
				return fail("root hash, rows the same as another commit's", r, root), nil
			}
			if k, ok := states[root]; ok && k != key {
				return fail("root hash the same as another commit's, rows not", k, key), nil
			}
			roots[key], states[root] = root, key
		}
		return nil, nil
	}
	if r.Failure, err = check(0, Op{Kind: "setup"}); r.Failure != nil || err != nil {
		return r, err
	}
	for i, op := range h.Ops {
		step := i + 1
		if !m.Legal(op) {
			r.Skipped++
			continue
		}
		want := m.Apply(op)
		got, err := e.Apply(op)
		if err != nil {
			return r, fmt.Errorf("step %d (%s): %w", step, op, err)
		}
		r.Ran++
		r.Outcomes[op.Kind+" "+got.Class]++
		if got != want {
			r.Failure = &Failure{Kind: "outcome", Step: step, Op: op, Expected: want, Got: got}
			return r, nil
		}
		if r.Failure, err = check(step, op); r.Failure != nil || err != nil {
			return r, err
		}
	}
	return r, nil
}
