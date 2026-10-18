package doltport

import (
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"prolly_bench/slt/internal/doltcatalog"
	"prolly_bench/slt/internal/vcscenario"
)

const fixture = "../../../corpus/dolt"

func put(t *testing.T, path string, v any) {
	t.Helper()
	b, err := json.MarshalIndent(v, "", "  ")
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, append(b, '\n'), 0644); err != nil {
		t.Fatal(err)
	}
}
func copyFixture(t *testing.T) string {
	t.Helper()
	root := filepath.Join(t.TempDir(), "corpus")
	if err := os.CopyFS(root, os.DirFS(fixture)); err != nil {
		t.Fatal(err)
	}
	return root
}
func manifestAt(t *testing.T, root string) Manifest {
	t.Helper()
	var m Manifest
	if err := readJSON(filepath.Join(root, "ports.json"), &m); err != nil {
		t.Fatal(err)
	}
	return m
}

// The reviewed corpus: a port added or deferred updates these.
const (
	reviewedPorts      = 4
	reviewedAssertions = 24
	reviewedDeferred   = 4
)

func TestReviewedCorpus(t *testing.T) {
	r, err := Verify(fixture, "", "")
	if err != nil {
		t.Fatal(err)
	}
	if r.SourceCases != 1233 || r.SourceAssertions != 5820 || r.PortedCases != reviewedPorts || r.MappedAssertions != reviewedAssertions || r.DeferredCases != reviewedDeferred || r.UnreviewedCases != 1233-reviewedPorts-reviewedDeferred || len(r.Engines) != 0 {
		t.Fatalf("%+v", r)
	}
}
func TestManifestDrift(t *testing.T) {
	tests := []struct {
		name, want string
		mutate     func(*Manifest)
	}{
		{"missing assertion", "mapping has", func(m *Manifest) { m.Ports[0].Assertions = m.Ports[0].Assertions[1:] }},
		{"duplicate assertion", "duplicate source index", func(m *Manifest) { m.Ports[0].Assertions[1].Index = 0 }},
		{"unknown step", "mapped step", func(m *Manifest) { m.Ports[0].Assertions[0].Steps = []string{"typo"} }},
		{"duplicate port", "duplicate source selection", func(m *Manifest) { m.Ports = append(m.Ports, m.Ports[0]) }},
		{"silent omission", "missing review note", func(m *Manifest) { m.Ports[0].Assertions[0].Steps = nil; m.Ports[0].Assertions[0].Note = "" }},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			root := copyFixture(t)
			m := manifestAt(t, root)
			tt.mutate(&m)
			put(t, filepath.Join(root, "ports.json"), m)
			if _, err := Verify(root, "", ""); err == nil || !strings.Contains(err.Error(), tt.want) {
				t.Fatalf("expected %s, got %v", tt.want, err)
			}
		})
	}
	for _, sub := range []string{"sources/dolt-hashof.go.txt", "scenarios/dolt-hashof.json"} {
		t.Run(sub, func(t *testing.T) {
			root := copyFixture(t)
			p := filepath.Join(root, sub)
			b, err := os.ReadFile(p)
			if err != nil {
				t.Fatal(err)
			}
			if err = os.WriteFile(p, append(b, ' '), 0644); err != nil {
				t.Fatal(err)
			}
			if _, err = Verify(root, "", ""); err == nil || !strings.Contains(err.Error(), "hash mismatch") {
				t.Fatalf("accepted changed bytes: %v", err)
			}
		})
	}
}
func TestExplicitOmissionIsPartial(t *testing.T) {
	root := copyFixture(t)
	m := manifestAt(t, root)
	p := &m.Ports[0]
	removed := p.Assertions[0].Steps[0]
	p.Assertions[0].Steps = nil
	p.Assertions[0].Note = "Deliberate test omission; not credited."
	file := filepath.Join(root, "scenarios", p.ID+".json")
	b, err := os.ReadFile(file)
	if err != nil {
		t.Fatal(err)
	}
	s, err := vcscenario.Parse(b)
	if err != nil {
		t.Fatal(err)
	}
	var kept []vcscenario.Step
	for _, st := range s.Steps {
		if st.ID != removed {
			kept = append(kept, st)
		}
	}
	s.Steps = kept
	put(t, file, s)
	b, err = os.ReadFile(file)
	if err != nil {
		t.Fatal(err)
	}
	p.ScenarioSHA256 = doltcatalog.Hash(b)
	put(t, filepath.Join(root, "ports.json"), m)
	r, err := Verify(root, "", "")
	if err != nil {
		t.Fatal(err)
	}
	if r.PortedCases != reviewedPorts-1 || r.PartialCases != 1 || r.MappedAssertions != reviewedAssertions-1 || r.OmittedAssertions != 1 {
		t.Fatalf("%+v", r)
	}
}

type execution struct {
	Profiles []vcscenario.Profile `json:"profiles"`
	Hashes   map[string]string    `json:"scenario_sha256"`
	Results  []vcscenario.Result  `json:"results"`
}

func passing(t *testing.T) execution {
	t.Helper()
	m := manifestAt(t, fixture)
	e := execution{Profiles: []vcscenario.Profile{{Engine: "prolly", Supported: []string{"sql", "vc.core"}}}, Hashes: map[string]string{}}
	for _, p := range m.Ports {
		b, err := os.ReadFile(filepath.Join(fixture, "scenarios", p.ID+".json"))
		if err != nil {
			t.Fatal(err)
		}
		s, err := vcscenario.Parse(b)
		if err != nil {
			t.Fatal(err)
		}
		r := vcscenario.Result{ID: p.ID, Engine: "prolly", Status: "passed", Requires: s.Requires, PlannedSteps: len(s.Steps)}
		for _, st := range s.Steps {
			r.Steps = append(r.Steps, vcscenario.StepResult{ID: st.ID, Op: st.Op, Session: st.Session})
		}
		e.Hashes[p.ID] = p.ScenarioSHA256
		e.Results = append(e.Results, r)
	}
	return e
}
func TestExecutionAccounting(t *testing.T) {
	// what the first port earns when it passes
	first := 0
	for _, a := range manifestAt(t, fixture).Ports[0].Assertions {
		if len(a.Steps) > 0 {
			first++
		}
	}
	tests := []struct {
		name                        string
		mutate                      func(*execution)
		wantErr                     bool
		passed, credit, unsupported int
	}{
		{"all pass", func(*execution) {}, false, reviewedPorts, reviewedAssertions, 0},
		{"unsupported earns no credit", func(e *execution) { e.Results[0].Status = "unsupported"; e.Results[0].Steps = nil }, false, reviewedPorts - 1, reviewedAssertions - first, 1},
		{"failed earns no credit", func(e *execution) { e.Results[0].Status = "failed" }, false, reviewedPorts - 1, reviewedAssertions - first, 0},
		{"missing case", func(e *execution) { e.Results = e.Results[1:] }, true, 0, 0, 0},
		{"duplicate case", func(e *execution) { e.Results = append(e.Results, e.Results[0]) }, true, 0, 0, 0},
		{"truncated pass", func(e *execution) { e.Results[0].Steps = nil }, true, 0, 0, 0},
		{"changed inputs", func(e *execution) { e.Hashes[e.Results[0].ID] = "different" }, true, 0, 0, 0},
		{"wrong step", func(e *execution) { e.Results[0].Steps[0].ID = "wrong" }, true, 0, 0, 0},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			e := passing(t)
			tt.mutate(&e)
			path := filepath.Join(t.TempDir(), "summary.json")
			put(t, path, e)
			r, err := Verify(fixture, "", path)
			if tt.wantErr {
				if err == nil {
					t.Fatal("accepted invalid execution evidence")
				}
				return
			}
			if err != nil {
				t.Fatal(err)
			}
			if len(r.Engines) != 1 || r.Engines[0].Passed != tt.passed || r.Engines[0].PassedMappedAssertions != tt.credit || r.Engines[0].Unsupported != tt.unsupported {
				t.Fatalf("%+v", r)
			}
		})
	}
}

func TestClassification(t *testing.T) {
	r, err := Verify(fixture, "", "")
	if err != nil {
		t.Fatal(err)
	}
	c := r.Classes
	if c.Portable+c.FeatureGated+c.NotApplicable != r.SourceCases || c.Portable == 0 || len(c.Features) == 0 {
		t.Fatalf("%+v", c)
	}
	ported := 0
	for _, a := range c.Areas {
		if a.Ported+a.Partial+a.Deferred+a.NotStarted != a.Portable {
			t.Fatalf("area doesn't add up: %+v", a)
		}
		ported += a.Ported + a.Partial
	}
	if ported != r.PortedCases+r.PartialCases {
		t.Fatalf("ports counted %d times by area, %d ported", ported, r.PortedCases+r.PartialCases)
	}
	m := manifestAt(t, fixture)
	first := classKey(m.Ports[0].Suite, m.Ports[0].Index)
	tests := []struct {
		name, want string
		mutate     func(*Classification)
	}{
		{"unknown class", "unknown class", func(c *Classification) { c.Cases[0].Class = "X" }},
		{"F without feature", "names the feature", func(c *Classification) { c.Cases[0].Class = "F"; c.Cases[0].Feature = "" }},
		{"P with feature", "only F cases", func(c *Classification) { c.Cases[0].Class = "P"; c.Cases[0].Feature = "tags" }},
		{"no reason", "a reason", func(c *Classification) { c.Cases[0].Reason = " " }},
		{"duplicate", "classified twice", func(c *Classification) { c.Cases[1] = c.Cases[0] }},
		{"missing", "classification has", func(c *Classification) { c.Cases = c.Cases[1:] }},
		{"other revision", "another revision", func(c *Classification) { c.Revision = "0" }},
		{"port of an N case", "full port must be of a P case", func(c *Classification) {
			for i := range c.Cases {
				if classKey(c.Cases[i].Suite, c.Cases[i].Index) == first {
					c.Cases[i].Class = "N"
				}
			}
		}},
		{"port classified from other source", "classified from other source", func(c *Classification) {
			for i := range c.Cases {
				if classKey(c.Cases[i].Suite, c.Cases[i].Index) == first {
					c.Cases[i].SHA256 = "0"
				}
			}
		}},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			root := copyFixture(t)
			var c Classification
			if err := readJSON(filepath.Join(root, "classification.json"), &c); err != nil {
				t.Fatal(err)
			}
			tt.mutate(&c)
			put(t, filepath.Join(root, "classification.json"), c)
			if _, err := Verify(root, "", ""); err == nil || !strings.Contains(err.Error(), tt.want) {
				t.Fatalf("expected %s, got %v", tt.want, err)
			}
		})
	}
}
