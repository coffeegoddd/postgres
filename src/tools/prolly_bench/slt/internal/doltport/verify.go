// Package doltport validates reviewed source-to-scenario mappings.
package doltport

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"reflect"
	"strings"

	"prolly_bench/slt/internal/doltcatalog"
	"prolly_bench/slt/internal/vcscenario"
)

type Mapping struct {
	Index int      `json:"index"`
	Steps []string `json:"steps"`
	Note  string   `json:"note"`
}
type Port struct {
	ID             string    `json:"id"`
	Path           string    `json:"path"`
	Suite          string    `json:"suite"`
	Index          int       `json:"index"`
	Name           string    `json:"name"`
	SourceSHA256   string    `json:"source_sha256"`
	ScenarioSHA256 string    `json:"scenario_sha256"`
	Setup          []Mapping `json:"setup"`
	Assertions     []Mapping `json:"assertions"`
	Adaptations    []string  `json:"adaptations"`
}
type Deferred struct {
	Path         string `json:"path"`
	Suite        string `json:"suite"`
	Index        int    `json:"index"`
	Name         string `json:"name"`
	SourceSHA256 string `json:"source_sha256"`
	Reason       string `json:"reason"`
}
type Manifest struct {
	Version  int        `json:"version"`
	Ports    []Port     `json:"ports"`
	Deferred []Deferred `json:"deferred"`
}
type EngineCoverage struct {
	Engine                 string `json:"engine"`
	Passed                 int    `json:"passed"`
	Failed                 int    `json:"failed"`
	Unsupported            int    `json:"unsupported"`
	NotRun                 int    `json:"not_run"`
	PassedMappedAssertions int    `json:"passed_mapped_assertions"`
}
type Report struct {
	Revision               string           `json:"revision"`
	Scope                  string           `json:"scope"`
	SourceCases            int              `json:"source_cases"`
	SourceAssertions       int              `json:"source_literal_assertions"`
	UnresolvedFields       int              `json:"source_unresolved_fields"`
	PortedCases            int              `json:"ported_cases"`
	PartialCases           int              `json:"partial_cases"`
	MappedAssertions       int              `json:"mapped_assertions"`
	OmittedAssertions      int              `json:"omitted_assertions"`
	DeferredCases          int              `json:"explicitly_deferred_cases"`
	UnreviewedCases        int              `json:"unreviewed_cases"`
	VerifiedAgainstCatalog bool             `json:"verified_against_catalog"`
	Classes                ClassCoverage    `json:"classes"`
	Engines                []EngineCoverage `json:"engines"`
}

func readJSON(path string, v any) error {
	b, err := os.ReadFile(path)
	if err != nil {
		return err
	}
	d := json.NewDecoder(bytes.NewReader(b))
	d.DisallowUnknownFields()
	if err := d.Decode(v); err != nil {
		return fmt.Errorf("%s: %w", path, err)
	}
	if err := d.Decode(new(any)); err != io.EOF {
		return fmt.Errorf("%s: expected one JSON document", path)
	}
	return nil
}
func key(path, suite string, index int) string { return fmt.Sprintf("%s/%s[%d]", path, suite, index) }
func mappings(ms []Mapping, count int, steps map[string]bool, used map[string]bool) (mapped int, err error) {
	if len(ms) != count {
		return 0, fmt.Errorf("mapping has %d entries, source has %d", len(ms), count)
	}
	seen := map[int]bool{}
	for _, m := range ms {
		if m.Index < 0 || m.Index >= count || seen[m.Index] || strings.TrimSpace(m.Note) == "" {
			return 0, fmt.Errorf("invalid/duplicate source index or missing review note")
		}
		seen[m.Index] = true
		local := map[string]bool{}
		for _, id := range m.Steps {
			if !steps[id] || local[id] {
				return 0, fmt.Errorf("unknown/duplicate mapped step %q", id)
			}
			local[id] = true
			used[id] = true
		}
		if len(m.Steps) > 0 {
			mapped++
		}
	}
	return mapped, nil
}

// Verify can run offline on retained excerpts, or against a freshly generated
// pinned catalog. Every setup/assertion must be mapped or explicitly omitted.
func Verify(root, catalogPath, resultsPath string) (Report, error) {
	var lock doltcatalog.Lock
	var manifest Manifest
	report := Report{}
	if err := readJSON(filepath.Join(root, "source-lock.json"), &lock); err != nil {
		return report, err
	}
	if err := readJSON(filepath.Join(root, "ports.json"), &manifest); err != nil {
		return report, err
	}
	if lock.Version != 1 || lock.Repository != doltcatalog.Repository || lock.Scope != doltcatalog.Scope || manifest.Version != 1 || len(manifest.Ports) == 0 {
		return report, fmt.Errorf("unsupported or empty corpus manifest")
	}
	report.Revision, report.Scope = lock.Revision, lock.Scope
	report.SourceCases, report.SourceAssertions, report.UnresolvedFields = lock.Counts.Cases, lock.Counts.Assertions, lock.Counts.UnresolvedFields
	sourceCases := map[string]doltcatalog.Case{}
	partial := map[string]bool{}
	if catalogPath != "" {
		var catalog doltcatalog.Catalog
		if err := readJSON(catalogPath, &catalog); err != nil {
			return report, err
		}
		if !reflect.DeepEqual(catalog.Lock, lock) {
			return report, fmt.Errorf("catalog does not match source lock")
		}
		for _, c := range catalog.Cases {
			k := key(c.Path, c.Suite, c.Index)
			if _, ok := sourceCases[k]; ok {
				return report, fmt.Errorf("duplicate catalog case %s", k)
			}
			sourceCases[k] = c
		}
		if len(sourceCases) != lock.Counts.Cases {
			return report, fmt.Errorf("catalog case count mismatch")
		}
		report.VerifiedAgainstCatalog = true
	}
	selected := map[string]bool{}
	scenarios := map[string]vcscenario.Scenario{}
	hashes := map[string]string{}
	credits := map[string]int{}
	checkSource := func(path, suite string, index int, name, sha string) error {
		k := key(path, suite, index)
		if selected[k] {
			return fmt.Errorf("duplicate source selection %s", k)
		}
		selected[k] = true
		if catalogPath != "" {
			c, ok := sourceCases[k]
			if !ok || c.Name != name || c.SHA256 != sha || doltcatalog.Hash([]byte(c.Source)) != sha {
				return fmt.Errorf("source identity drift: %s", k)
			}
		}
		return nil
	}
	for _, p := range manifest.Ports {
		// IDs become filenames, so validate through the scenario before using them.
		if strings.ContainsAny(p.ID, "/\\.") || p.ID == "" {
			return report, fmt.Errorf("invalid port id")
		}
		if err := checkSource(p.Path, p.Suite, p.Index, p.Name, p.SourceSHA256); err != nil {
			return report, err
		}
		raw, err := os.ReadFile(filepath.Join(root, "sources", p.ID+".go.txt"))
		if err != nil {
			return report, err
		}
		if doltcatalog.Hash(raw) != p.SourceSHA256 {
			return report, fmt.Errorf("%s: source excerpt hash mismatch", p.ID)
		}
		cases, err := doltcatalog.Scan(p.Path, []byte("package excerpt\nvar "+p.Suite+" = []queries.ScriptTest{"+string(raw)+"}\n"))
		if err != nil {
			return report, err
		}
		if len(cases) != 1 || cases[0].Name != p.Name || len(cases[0].Unresolved) != 0 {
			return report, fmt.Errorf("%s: invalid or unresolved source excerpt", p.ID)
		}
		b, err := os.ReadFile(filepath.Join(root, "scenarios", p.ID+".json"))
		if err != nil {
			return report, err
		}
		if doltcatalog.Hash(b) != p.ScenarioSHA256 {
			return report, fmt.Errorf("%s: scenario hash mismatch; review the mapping before updating", p.ID)
		}
		s, err := vcscenario.Parse(b)
		if err != nil {
			return report, err
		}
		if _, exists := scenarios[s.ID]; exists {
			return report, fmt.Errorf("duplicate scenario %s", s.ID)
		}
		expectedSource := &vcscenario.Source{Repository: lock.Repository, Revision: lock.Revision, Path: p.Path, Case: p.Suite + ": " + p.Name}
		if s.ID != p.ID || !reflect.DeepEqual(s.Source, expectedSource) || len(p.Adaptations) == 0 {
			return report, fmt.Errorf("%s: scenario provenance or adaptation notes missing/mismatched", p.ID)
		}
		steps, used := map[string]bool{}, map[string]bool{}
		for _, st := range s.Steps {
			steps[st.ID] = true
		}
		setup, err := mappings(p.Setup, cases[0].Setup, steps, used)
		if err != nil {
			return report, fmt.Errorf("%s setup: %w", p.ID, err)
		}
		assertions, err := mappings(p.Assertions, cases[0].Assertions, steps, used)
		if err != nil {
			return report, fmt.Errorf("%s assertions: %w", p.ID, err)
		}
		if len(used) != len(steps) {
			return report, fmt.Errorf("%s: scenario contains unmapped steps", p.ID)
		}
		if setup != len(p.Setup) || assertions != len(p.Assertions) {
			report.PartialCases++
			partial[p.ID] = true
		} else {
			report.PortedCases++
		}
		report.MappedAssertions += assertions
		report.OmittedAssertions += len(p.Assertions) - assertions
		scenarios[s.ID], hashes[s.ID], credits[s.ID] = s, p.ScenarioSHA256, assertions
	}
	files, err := filepath.Glob(filepath.Join(root, "scenarios", "*.json"))
	if err != nil {
		return report, err
	}
	if len(files) != len(scenarios) {
		return report, fmt.Errorf("unlisted scenario files")
	}
	for _, d := range manifest.Deferred {
		if strings.TrimSpace(d.Reason) == "" {
			return report, fmt.Errorf("deferred case needs a reason")
		}
		if err := checkSource(d.Path, d.Suite, d.Index, d.Name, d.SourceSHA256); err != nil {
			return report, err
		}
		report.DeferredCases++
	}
	report.UnreviewedCases = report.SourceCases - len(selected)
	if report.UnreviewedCases < 0 {
		return report, fmt.Errorf("more selected cases than source inventory")
	}
	var classes Classification
	if err := readJSON(filepath.Join(root, "classification.json"), &classes); err != nil {
		return report, err
	}
	var catalog map[string]doltcatalog.Case
	if catalogPath != "" {
		catalog = sourceCases
	}
	cov, err := checkClassification(classes, lock, catalog, manifest, partial)
	if err != nil {
		return report, err
	}
	report.Classes = cov
	if resultsPath != "" {
		engines, err := coverage(resultsPath, scenarios, hashes, credits)
		if err != nil {
			return report, err
		}
		report.Engines = engines
	}
	return report, nil
}

// Count only complete, passed executions with matching saved scenario hashes.
// Missing cases, duplicate records and truncated passing traces are errors.
func coverage(path string, scenarios map[string]vcscenario.Scenario, hashes map[string]string, credits map[string]int) ([]EngineCoverage, error) {
	var summary struct {
		Profiles []vcscenario.Profile `json:"profiles"`
		Hashes   map[string]string    `json:"scenario_sha256"`
		Counts   map[string]int       `json:"counts"`
		Results  []vcscenario.Result  `json:"results"`
	}
	if err := readJSON(path, &summary); err != nil {
		return nil, err
	}
	if !reflect.DeepEqual(summary.Hashes, hashes) {
		return nil, fmt.Errorf("execution scenario hashes do not match reviewed ports")
	}
	engines := map[string]*EngineCoverage{}
	var order []string
	for _, p := range summary.Profiles {
		if p.Engine == "" || engines[p.Engine] != nil {
			return nil, fmt.Errorf("invalid/duplicate engine profile")
		}
		engines[p.Engine] = &EngineCoverage{Engine: p.Engine}
		order = append(order, p.Engine)
	}
	if len(engines) == 0 {
		return nil, fmt.Errorf("no execution profiles")
	}
	seen := map[string]bool{}
	for _, r := range summary.Results {
		s, ok := scenarios[r.ID]
		e := engines[r.Engine]
		k := r.Engine + "/" + r.ID
		if !ok || e == nil || seen[k] || r.PlannedSteps != len(s.Steps) {
			return nil, fmt.Errorf("unknown, duplicate or mismatched execution %s", k)
		}
		seen[k] = true
		switch r.Status {
		case "passed":
			if r.Error != "" || r.Phase != "" || len(r.Missing) > 0 || len(r.Steps) != len(s.Steps) {
				return nil, fmt.Errorf("incomplete passing execution %s", k)
			}
			for i, step := range r.Steps {
				expected := s.Steps[i]
				if step.ID != expected.ID || step.Op != expected.Op || step.Session != expected.Session || step.Error != "" {
					return nil, fmt.Errorf("invalid passing trace %s", k)
				}
			}
			e.Passed++
			e.PassedMappedAssertions += credits[r.ID]
		case "failed":
			e.Failed++
		case "unsupported":
			e.Unsupported++
		case "not_run":
			e.NotRun++
		default:
			return nil, fmt.Errorf("unknown execution status %q", r.Status)
		}
	}
	if len(seen) != len(engines)*len(scenarios) {
		return nil, fmt.Errorf("execution summary omits engine/case pairs")
	}
	var out []EngineCoverage
	for _, name := range order {
		out = append(out, *engines[name])
	}
	return out, nil
}
