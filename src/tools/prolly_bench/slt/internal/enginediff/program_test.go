package enginediff

import (
	"encoding/json"
	"reflect"
	"testing"
)

func TestGenerateReplayAndCoverage(t *testing.T) {
	p := Generate(42, 28)
	b, err := json.Marshal(p)
	if err != nil {
		t.Fatal(err)
	}
	var replay Program
	if err := json.Unmarshal(b, &replay); err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(p, replay) || !reflect.DeepEqual(p, Generate(42, 28)) {
		t.Fatal("seed and serialized replay must reproduce the same program")
	}
	if reflect.DeepEqual(p, Generate(43, 28)) {
		t.Fatal("seeds did not change the workload")
	}
	seen := map[string]int{}
	for _, g := range p.Groups {
		seen[g.Name]++
	}
	if len(seen) != 14 {
		t.Fatalf("missing workload families: %v", seen)
	}
	for _, n := range seen {
		if n != 2 {
			t.Fatalf("unbalanced families: %v", seen)
		}
	}
}

func TestReduceKeepsFailureAndSetup(t *testing.T) {
	p := Generate(7, 28)
	calls := 0
	r := Reduce(p, 48, func(trial Program) bool {
		calls++
		for _, g := range trial.Groups {
			if g.Name == "savepoints" {
				return true
			}
		}
		return false
	})
	if calls > 48 || len(r.Groups) != 1 || r.Groups[0].Name != "savepoints" || !reflect.DeepEqual(r.Setup, p.Setup) {
		t.Fatalf("bad reduction: calls=%d, program=%+v", calls, r)
	}
	r = Reduce(p, 1, func(Program) bool { return false })
	if !reflect.DeepEqual(r, p) {
		t.Fatal("a non-reproducing reduction was retained")
	}
}

func TestNullAndEmptyDiffer(t *testing.T) {
	null, _ := json.Marshal([][][]byte{{nil}})
	empty, _ := json.Marshal([][][]byte{{[]byte{}}})
	if string(null) == string(empty) {
		t.Fatal("NULL and empty bytes compare equal")
	}
}
