package vcscenario

import (
	"context"
	"os"
	"path/filepath"
	"testing"
	"time"
)

func TestBundledScenariosAndCapabilityExclusions(t *testing.T) {
	paths, err := filepath.Glob("cases/*.json")
	if err != nil || len(paths) != 2 {
		t.Fatalf("cases: %v %v", paths, err)
	}
	for _, path := range paths {
		b, err := os.ReadFile(path)
		if err != nil {
			t.Fatal(err)
		}
		if _, err = Parse(b); err != nil {
			t.Fatalf("%s: %v", path, err)
		}
	}
	core, err := Parse(targetScenario)
	if err != nil {
		t.Fatal(err)
	}
	for _, am := range []string{"heap", "prolly", "prolly_d2", "prolly_d3a", "prolly_d3b", "prolly_d4"} {
		// A deliberately invalid DSN proves capability reporting requires no
		// connection and cannot run SQL on a fallback engine.
		a, err := NewPostgres("invalid dsn", am, time.Second)
		if err != nil {
			t.Fatal(err)
		}
		r := Run(context.Background(), core, a)
		if r.Status != "unsupported" || len(r.Steps) != 0 {
			t.Fatalf("%s: %+v", am, r)
		}
		if am != "heap" && am != "prolly" && len(r.Missing) != 2 {
			t.Fatalf("stub claimed a capability: %+v", r)
		}
	}
	if _, err := NewPostgres("", "typo", time.Second); err == nil {
		t.Fatal("unknown engine accepted")
	}
}
