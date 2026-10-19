// Package vcscenario runs version-control scenarios through engine adapters.
package vcscenario

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"regexp"
)

// Value is either exact SQL text, SQL NULL, a previously captured commit,
// or a variant's variable. Numbers are represented as strings to avoid JSON
// floating-point conversion.
type Value struct {
	Text *string
	Ref  string
	Var  string
}

func (v *Value) UnmarshalJSON(b []byte) error {
	*v = Value{}
	if bytes.Equal(bytes.TrimSpace(b), []byte("null")) {
		return nil
	}
	if err := json.Unmarshal(b, &v.Text); err == nil {
		return nil
	}
	v.Text = nil
	var ref struct {
		Ref string `json:"ref"`
		Var string `json:"var"`
	}
	if err := decode(b, &ref); err != nil || (ref.Ref == "") == (ref.Var == "") {
		return fmt.Errorf("value must be a string, null, {\"ref\":\"name\"} or {\"var\":\"name\"}")
	}
	v.Ref, v.Var = ref.Ref, ref.Var
	return nil
}

func (v Value) MarshalJSON() ([]byte, error) {
	if v.Ref != "" {
		return json.Marshal(map[string]string{"ref": v.Ref})
	}
	if v.Var != "" {
		return json.Marshal(map[string]string{"var": v.Var})
	}
	return json.Marshal(v.Text)
}

type Source struct {
	Repository string `json:"repository"`
	Revision   string `json:"revision"`
	Path       string `json:"path"`
	Case       string `json:"case"`
}

type Scenario struct {
	Version     int       `json:"version"`
	ID          string    `json:"id"`
	Description string    `json:"description"`
	Source      *Source   `json:"source,omitempty"`
	Requires    []string  `json:"requires"`
	Sessions    []string  `json:"sessions"`
	Variants    []Variant `json:"variants,omitempty"`
	Steps       []Step    `json:"steps"`
}

// Variant is one run of a scenario with variables set, as Dolt runs each
// merge case once per direction: {"var":"name"} values take its values, and
// a step that names variants runs in those alone (an asymmetry, recorded).
// Each variant runs on fresh state.
type Variant struct {
	Name string            `json:"name"`
	Vars map[string]string `json:"vars"`
}

// Pass is what one run of a scenario executes: its variant (none: "") and
// the steps that run in it, in order.
type Pass struct {
	Variant string
	Vars    map[string]string
	Steps   []Step
}

// Passes are a scenario's runs: one per variant, or one of every step.
func (s Scenario) Passes() []Pass {
	if len(s.Variants) == 0 {
		return []Pass{{Steps: s.Steps}}
	}
	var out []Pass
	for _, v := range s.Variants {
		p := Pass{Variant: v.Name, Vars: v.Vars}
		for _, st := range s.Steps {
			if len(st.Variants) == 0 || contains(st.Variants, v.Name) {
				p.Steps = append(p.Steps, st)
			}
		}
		out = append(out, p)
	}
	return out
}

// PlannedSteps is how many steps all its passes execute.
func (s Scenario) PlannedSteps() int {
	n := 0
	for _, p := range s.Passes() {
		n += len(p.Steps)
	}
	return n
}

func contains(list []string, x string) bool {
	for _, y := range list {
		if y == x {
			return true
		}
	}
	return false
}

type Step struct {
	ID       string    `json:"id"`
	Session  string    `json:"session"`
	Variants []string  `json:"variants,omitempty"`
	Op       string    `json:"op"`
	Requires []string  `json:"requires,omitempty"`
	SQL      string    `json:"sql,omitempty"`
	Params   []Value   `json:"params,omitempty"`
	Args     []Value   `json:"args,omitempty"`
	Save     string    `json:"save,omitempty"`
	Expect   *Expected `json:"expect,omitempty"`
}

type Expected struct {
	Rows        *[][]Value `json:"rows,omitempty"`
	Order       string     `json:"order,omitempty"` // ordered or bag; duplicates always matter
	Columns     []string   `json:"columns,omitempty"`
	Tag         string     `json:"tag,omitempty"`
	SQLState    string     `json:"sqlstate,omitempty"`
	Transaction string     `json:"transaction,omitempty"`
}

func decode(b []byte, dst any) error {
	d := json.NewDecoder(bytes.NewReader(b))
	d.DisallowUnknownFields()
	if err := d.Decode(dst); err != nil {
		return err
	}
	if err := d.Decode(new(any)); err != io.EOF {
		return fmt.Errorf("expected one JSON document")
	}
	return nil
}

func Parse(b []byte) (Scenario, error) {
	var s Scenario
	if err := decode(b, &s); err != nil {
		return s, err
	}
	return s, s.Validate()
}

var identifier = regexp.MustCompile(`^[a-zA-Z][a-zA-Z0-9_-]*$`)
var sqlstate = regexp.MustCompile(`^[0-9A-Z]{5}$`)

func (s Scenario) Validate() error {
	if s.Version != 1 || !identifier.MatchString(s.ID) || s.Description == "" || len(s.Steps) == 0 {
		return fmt.Errorf("requires version 1, a valid id, description and nonempty steps")
	}
	if p := s.Source; p != nil && (p.Repository == "" || p.Revision == "" || p.Path == "" || p.Case == "") {
		return fmt.Errorf("source provenance must include repository, revision, path and case")
	}
	requires := map[string]bool{}
	for _, r := range s.Requires {
		if _, ok := Capabilities[r]; !ok || requires[r] {
			return fmt.Errorf("unknown or duplicate capability %q", r)
		}
		requires[r] = true
	}
	if !requires["sql"] {
		return fmt.Errorf("sql capability is required")
	}
	// The scenario requires exactly what its steps need: a capability it
	// doesn't use would wrongly report it unsupported on an engine.
	used := map[string]bool{"sql": true}
	sessions := map[string]bool{}
	for _, n := range s.Sessions {
		if !identifier.MatchString(n) || sessions[n] {
			return fmt.Errorf("invalid or duplicate session %q", n)
		}
		sessions[n] = true
	}
	// Variants: distinct names, each setting the same variables.
	variants := map[string]bool{}
	for _, v := range s.Variants {
		if !identifier.MatchString(v.Name) || variants[v.Name] || len(v.Vars) != len(s.Variants[0].Vars) {
			return fmt.Errorf("invalid or duplicate variant %q", v.Name)
		}
		for k := range v.Vars {
			if _, ok := s.Variants[0].Vars[k]; !ok || !identifier.MatchString(k) {
				return fmt.Errorf("variant %q sets variable %q the first doesn't", v.Name, k)
			}
		}
		variants[v.Name] = true
	}
	if len(s.Variants) == 1 {
		return fmt.Errorf("one variant is no variant; give two or more, or none")
	}
	ids := map[string]bool{}
	for _, st := range s.Steps {
		if !identifier.MatchString(st.ID) || ids[st.ID] || !sessions[st.Session] {
			return fmt.Errorf("invalid step id or session: %s", st.ID)
		}
		ids[st.ID] = true
		for _, v := range st.Variants {
			if !variants[v] {
				return fmt.Errorf("%s: unknown variant %q", st.ID, v)
			}
		}
	}
	for _, p := range s.Passes() {
		if len(p.Steps) == 0 {
			return fmt.Errorf("variant %q runs no step", p.Variant)
		}
		if err := validatePass(p, requires, used); err != nil {
			if p.Variant != "" {
				return fmt.Errorf("variant %s: %w", p.Variant, err)
			}
			return err
		}
	}
	for _, r := range s.Requires {
		if !used[r] {
			return fmt.Errorf("requires %s, which no step needs", r)
		}
	}
	return nil
}

// validatePass checks one pass's steps, in order: each step's form, and
// the bindings and variables its values use.
func validatePass(p Pass, requires, used map[string]bool) error {
	bindings := map[string]bool{}
	for _, st := range p.Steps {
		isSQL := st.Op == "exec" || st.Op == "query"
		if _, typed := ops[st.Op]; !isSQL && !typed {
			return fmt.Errorf("%s: unknown operation %q", st.ID, st.Op)
		}
		need, err := stepRequires(st)
		if err != nil {
			return err
		}
		for _, c := range need {
			if !requires[c] {
				return fmt.Errorf("%s: needs capability %s, which the scenario doesn't require", st.ID, c)
			}
			used[c] = true
		}
		if isSQL {
			if st.SQL == "" || len(st.Args) != 0 || st.Save != "" {
				return fmt.Errorf("%s: SQL steps require sql, optional params, no args/save", st.ID)
			}
		} else {
			if st.SQL != "" || len(st.Params) != 0 {
				return fmt.Errorf("%s: VC steps take args only", st.ID)
			}
			n := len(st.Args)
			switch st.Op {
			case "commit":
				// the message, then any of Dolt's other flags
				// (--allow-empty, --author name)
				if n < 1 {
					return fmt.Errorf("%s: commit takes a message and optional flags", st.ID)
				}
			case "connect":
				// none: the case's database; one: a branch of it
				if n > 1 {
					return fmt.Errorf("%s: connect takes at most a branch", st.ID)
				}
			case "checkout", "reset":
				if n != 1 {
					return fmt.Errorf("%s: %s takes one argument", st.ID, st.Op)
				}
			case "branch":
				if n < 1 || n > 2 {
					return fmt.Errorf("%s: branch takes name and optional start ref", st.ID)
				}
			case "merge", "cherrypick", "revert":
				// Dolt's arguments, flags included (--no-ff, --abort,
				// --continue, ...), passed through as they are.
				if n < 1 {
					return fmt.Errorf("%s: %s needs arguments", st.ID, st.Op)
				}
			}
			for _, a := range st.Args {
				if a.Text == nil && a.Ref == "" && a.Var == "" {
					return fmt.Errorf("%s: VC arguments cannot be null", st.ID)
				}
			}
		}
		errExpected := st.Expect != nil && st.Expect.SQLState != ""
		// A commit's id is always captured; a merge's, pick's or revert's
		// when the scenario names it (they can end without a commit).
		saves := ops[st.Op].saves
		if st.Op == "commit" && !errExpected && st.Save == "" {
			return fmt.Errorf("%s: commit needs a unique save binding", st.ID)
		}
		if st.Save != "" && (!saves || errExpected || !identifier.MatchString(st.Save) || bindings[st.Save]) {
			return fmt.Errorf("%s: save is only valid, and unique, on successful commits, merges, picks and reverts", st.ID)
		}
		values := append(append([]Value{}, st.Params...), st.Args...)
		if e := st.Expect; e != nil {
			if e.Transaction != "" && e.Transaction != "I" && e.Transaction != "T" && e.Transaction != "E" {
				return fmt.Errorf("%s: invalid transaction status", st.ID)
			}
			if errExpected {
				if !sqlstate.MatchString(e.SQLState) || e.SQLState == "00000" || e.Rows != nil || e.Order != "" || e.Tag != "" || len(e.Columns) != 0 {
					return fmt.Errorf("%s: invalid error expectation", st.ID)
				}
			} else if e.Rows != nil {
				if st.Op != "query" || (e.Order != "ordered" && e.Order != "bag") {
					return fmt.Errorf("%s: rows require query and explicit order", st.ID)
				}
				width := -1
				for _, row := range *e.Rows {
					if width == -1 {
						width = len(row)
					}
					if len(row) != width || (len(e.Columns) != 0 && len(row) != len(e.Columns)) {
						return fmt.Errorf("%s: inconsistent row width", st.ID)
					}
					values = append(values, row...)
				}
			} else if e.Order != "" || len(e.Columns) != 0 {
				return fmt.Errorf("%s: columns/order need rows", st.ID)
			}
		}
		if st.Op == "query" && !errExpected && (st.Expect == nil || st.Expect.Rows == nil) {
			return fmt.Errorf("%s: query requires expected rows", st.ID)
		}
		for _, v := range values {
			if v.Ref != "" && (v.Text != nil || !bindings[v.Ref]) {
				return fmt.Errorf("%s: invalid or undefined binding %q", st.ID, v.Ref)
			}
			if _, ok := p.Vars[v.Var]; v.Var != "" && !ok {
				return fmt.Errorf("%s: undefined variable %q", st.ID, v.Var)
			}
		}
		if st.Save != "" {
			bindings[st.Save] = true
		}
	}
	return nil
}
