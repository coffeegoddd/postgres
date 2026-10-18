package main

import (
	"fmt"
	"strings"

	"prolly_bench/slt/internal/vcfuzz"
)

// selfTestHistory commits a change and its undoing (rows the same as the
// setup's), then merges a conflicting branch, resolving with ours.
func selfTestHistory() vcfuzz.History {
	setup := vcfuzz.State{"t": {1: {1, 1}}, "u": {}}
	update := func(v int) vcfuzz.Op {
		return vcfuzz.Op{Kind: vcfuzz.OpUpdate, Table: "t", ID: 1, Val: v}
	}
	commit := vcfuzz.Op{Kind: vcfuzz.OpCommit}
	return vcfuzz.History{Version: 1, Sessions: 1, Setup: setup, Ops: []vcfuzz.Op{
		{Kind: vcfuzz.OpBranch, Ref: "x"},
		update(2), commit, update(1), commit, update(2), commit,
		{Kind: vcfuzz.OpCheckout, Ref: "x"}, update(3), commit,
		{Kind: vcfuzz.OpCheckout, Ref: "main"},
		{Kind: vcfuzz.OpMerge, Ref: "x", Mode: vcfuzz.ModeOurs},
	}}
}

// mutant is an engine wrong in one known way.
type mutant struct {
	vcfuzz.Engine
	apply func(e vcfuzz.Engine, op vcfuzz.Op) (vcfuzz.Outcome, error)
	// rootHash, if set, replaces the engine's root hashes.
	rootHash func(commit string) string
}

func (m *mutant) Apply(op vcfuzz.Op) (vcfuzz.Outcome, error) { return m.apply(m.Engine, op) }

func (m *mutant) Heads() (map[string]string, error) {
	return m.Engine.(vcfuzz.Identities).Heads()
}

func (m *mutant) RootHash(commit string) (string, error) {
	if m.rootHash != nil {
		return m.rootHash(commit), nil
	}
	return m.Engine.(vcfuzz.Identities).RootHash(commit)
}

// selfTest proves the checks pass the history on the engine and reject
// engines wrong in known ways: resolving conflicts with the other side,
// committing nothing, and hashing commits of the same rows differently.
func selfTest(open vcfuzz.Opener) error {
	h := selfTestHistory()
	r, err := vcfuzz.Run(h, open)
	if err != nil {
		return err
	}
	if r.Failure != nil {
		return fmt.Errorf("the self-test history fails: %s", r.Failure.Error())
	}
	mutants := []struct {
		name, kind string
		m          mutant
	}{
		{"resolves conflicts with the other side", "rows", mutant{apply: func(e vcfuzz.Engine, op vcfuzz.Op) (vcfuzz.Outcome, error) {
			if op.Mode == vcfuzz.ModeOurs {
				op.Mode = vcfuzz.ModeTheirs
			}
			return e.Apply(op)
		}}},
		{"commits nothing", "uncommitted changes", mutant{apply: func(e vcfuzz.Engine, op vcfuzz.Op) (vcfuzz.Outcome, error) {
			if op.Kind == vcfuzz.OpCommit {
				return vcfuzz.Outcome{Class: vcfuzz.ClassOK}, nil
			}
			return e.Apply(op)
		}}},
		{"hashes the same rows differently", "root hash", mutant{
			apply:    func(e vcfuzz.Engine, op vcfuzz.Op) (vcfuzz.Outcome, error) { return e.Apply(op) },
			rootHash: func(commit string) string { return commit },
		}},
	}
	for _, mu := range mutants {
		m := mu.m
		r, err := vcfuzz.Run(h, func(h vcfuzz.History) (vcfuzz.Engine, error) {
			e, err := open(h)
			m.Engine = e
			return &m, err
		})
		if err != nil {
			return fmt.Errorf("an engine that %s: %w", mu.name, err)
		}
		if r.Failure == nil || !strings.Contains(r.Failure.Kind, mu.kind) {
			return fmt.Errorf("an engine that %s passes (%v)", mu.name, r.Failure)
		}
		fmt.Printf("self-test: rejected an engine that %s: %s\n", mu.name, r.Failure.Error())
	}
	return nil
}
