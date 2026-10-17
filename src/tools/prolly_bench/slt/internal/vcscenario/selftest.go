package vcscenario

import (
	"context"
	"fmt"
	"strings"
	"time"
)

type claimedVC struct{ *Postgres }

func (p claimedVC) Capabilities() []string { return []string{"sql", "vc.core"} }

// SelfTest proves that native adapter failures cannot be reported as passes or
// silently excluded as unsupported. These are separate from corpus coverage.
func SelfTest(ctx context.Context, dsn string, timeout time.Duration) ([]Result, error) {
	var results []Result
	for _, tc := range []struct{ id, sql, match string }{
		{"wrong-result", "SELECT 'wrong'::text", "rows differ"},
		{"unexpected-error", "SELECT missing_column", "SQLSTATE"},
		{"wrong-am", "CREATE TABLE rogue (id int) USING heap", "wrong AM"},
	} {
		rows := [][]Value{{{Text: func() *string { s := "expected"; return &s }()}}}
		st := Step{ID: tc.id, Session: "a", Op: "query", SQL: tc.sql, Expect: &Expected{Rows: &rows, Order: "ordered"}}
		if tc.id == "wrong-am" {
			st.Op = "exec"
			st.Expect = nil
		}
		s := Scenario{Version: 1, ID: tc.id, Description: "negative control", Requires: []string{"sql"}, Sessions: []string{"a"}, Steps: []Step{st}}
		a, _ := NewPostgres(dsn, "prolly_d1", timeout)
		r := Run(ctx, s, a)
		results = append(results, r)
		if r.Status != "failed" || r.Phase != "step" || !strings.Contains(r.Error, tc.match) {
			return results, fmt.Errorf("negative control %s did not fail as intended: %+v", tc.id, r)
		}
	}
	s, err := Parse(targetScenario)
	if err != nil {
		return results, err
	}
	p, _ := NewPostgres(dsn, "heap", timeout)
	r := Run(ctx, s, claimedVC{p})
	results = append(results, r)
	// Heap tables aren't branched: one branch's edits show on another, the
	// probe's isolation step failing.
	if r.Status != "failed" || r.Phase != "target" || !strings.Contains(r.Error, "VC canary: main-isolation:") {
		return results, fmt.Errorf("a false VC capability claim did not fail targeting: %+v", r)
	}
	p, _ = NewPostgres(dsn, "prolly", timeout)
	r = Run(ctx, s, p)
	results = append(results, r)
	if r.Status != "unsupported" || len(r.Missing) != 1 || r.Missing[0] != "vc.core" || len(r.Steps) != 0 {
		return results, fmt.Errorf("missing VC capability was counted as coverage: %+v", r)
	}
	return results, nil
}
