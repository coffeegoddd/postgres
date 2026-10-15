package vcscenario

import (
	"context"
	"encoding/json"
	"fmt"
	"reflect"
	"sort"
)

type Outcome struct {
	Rows        [][]*string `json:"rows"`
	Columns     []string    `json:"columns,omitempty"`
	Tag         string      `json:"tag,omitempty"`
	SQLState    string      `json:"sqlstate,omitempty"`
	Transaction string      `json:"transaction"`
}

// Verify must prove the storage/VC path before opening the case's fresh state.
// SQL errors are outcomes; connection failures and timeouts are adapter errors.
type Adapter interface {
	Name() string
	Capabilities() []string
	Verify(context.Context, []string) error
	Open(context.Context, []string) error
	Execute(context.Context, Step, []*string, []*string) (Outcome, error)
	CheckTarget(context.Context, string) error
	Close(context.Context) error
}

type StepResult struct {
	ID      string  `json:"id"`
	Session string  `json:"session"`
	Op      string  `json:"op"`
	Actual  Outcome `json:"actual"`
	Error   string  `json:"error,omitempty"`
}

type Result struct {
	ID           string       `json:"id"`
	Engine       string       `json:"engine"`
	Status       string       `json:"status"`
	Phase        string       `json:"phase,omitempty"`
	Error        string       `json:"error,omitempty"`
	Requires     []string     `json:"requires"`
	Missing      []string     `json:"missing,omitempty"`
	PlannedSteps int          `json:"planned_steps"`
	Steps        []StepResult `json:"steps"`
}

func Run(ctx context.Context, s Scenario, a Adapter) (r Result) {
	r = Result{ID: s.ID, Engine: a.Name(), Requires: s.Requires, PlannedSteps: len(s.Steps), Status: "failed"}
	if err := s.Validate(); err != nil {
		r.Phase, r.Error = "validation", err.Error()
		return
	}
	have := map[string]bool{}
	for _, c := range a.Capabilities() {
		have[c] = true
	}
	for _, c := range s.Requires {
		if !have[c] {
			r.Missing = append(r.Missing, c)
		}
	}
	if len(r.Missing) != 0 {
		r.Status = "unsupported"
		return
	}
	if err := a.Verify(ctx, s.Requires); err != nil {
		r.Phase, r.Error = "target", err.Error()
		return
	}
	defer func() {
		if err := a.Close(context.Background()); err != nil {
			r.Status = "failed"
			r.Error += " cleanup: " + err.Error()
			if r.Phase == "" {
				r.Phase = "cleanup"
			}
		}
	}()
	if err := a.Open(ctx, s.Sessions); err != nil {
		r.Phase, r.Error = "setup", err.Error()
		return
	}
	r.Steps, r.Error = execute(ctx, s, a)
	if r.Error != "" {
		r.Phase = "step"
		return
	}
	r.Status = "passed"
	return
}

func resolve(vs []Value, bindings map[string]*string) ([]*string, error) {
	var out []*string
	for _, v := range vs {
		p := v.Text
		if v.Ref != "" {
			var ok bool
			p, ok = bindings[v.Ref]
			if !ok {
				return nil, fmt.Errorf("undefined binding %q", v.Ref)
			}
		}
		out = append(out, p)
	}
	return out, nil
}

func rowKeys(rows [][]*string) []string {
	keys := make([]string, len(rows))
	for i, row := range rows {
		b, _ := json.Marshal(row)
		keys[i] = string(b)
	}
	return keys
}

func compare(st Step, o Outcome, bindings map[string]*string) error {
	e := st.Expect
	if e == nil {
		e = &Expected{}
	}
	if o.SQLState != e.SQLState {
		return fmt.Errorf("SQLSTATE: expected %q, got %q", e.SQLState, o.SQLState)
	}
	if e.Transaction != "" && o.Transaction != e.Transaction {
		return fmt.Errorf("transaction: expected %s, got %s", e.Transaction, o.Transaction)
	}
	if e.SQLState != "" {
		return nil
	}
	if e.Tag != "" && e.Tag != o.Tag {
		return fmt.Errorf("command tag: expected %q, got %q", e.Tag, o.Tag)
	}
	if len(e.Columns) != 0 && !reflect.DeepEqual(e.Columns, o.Columns) {
		return fmt.Errorf("column names differ: expected %v, got %v", e.Columns, o.Columns)
	}
	if e.Rows != nil {
		want := make([][]*string, len(*e.Rows))
		for i, row := range *e.Rows {
			var err error
			want[i], err = resolve(row, bindings)
			if err != nil {
				return err
			}
		}
		x, y := rowKeys(want), rowKeys(o.Rows)
		if e.Order == "bag" {
			sort.Strings(x)
			sort.Strings(y)
		}
		if !reflect.DeepEqual(x, y) {
			return fmt.Errorf("rows differ: expected %v, got %v", x, y)
		}
	} else if st.Op == "exec" && len(o.Columns) != 0 {
		return fmt.Errorf("exec returned columns; use query with explicit expected rows")
	}
	return nil
}

func execute(ctx context.Context, s Scenario, a Adapter) ([]StepResult, string) {
	bindings := map[string]*string{}
	var trace []StepResult
	for _, st := range s.Steps {
		params, err := resolve(st.Params, bindings)
		var args []*string
		if err == nil {
			args, err = resolve(st.Args, bindings)
		}
		var o Outcome
		if err == nil {
			o, err = a.Execute(ctx, st, params, args)
		}
		if err == nil {
			err = compare(st, o, bindings)
		}
		if err == nil && st.Save != "" {
			if len(o.Rows) != 1 || len(o.Rows[0]) != 1 || o.Rows[0][0] == nil || *o.Rows[0][0] == "" {
				err = fmt.Errorf("commit did not return one nonempty identifier")
			} else {
				bindings[st.Save] = o.Rows[0][0]
			}
		}
		if err == nil {
			err = a.CheckTarget(ctx, st.Session)
		}
		detail := ""
		if err != nil {
			detail = err.Error()
		}
		trace = append(trace, StepResult{st.ID, st.Session, st.Op, o, detail})
		if err != nil {
			return trace, st.ID + ": " + detail
		}
	}
	return trace, ""
}
