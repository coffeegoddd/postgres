package vcscenario

import (
	"context"
	"fmt"
	"strings"
	"time"
)

type claimedVC struct{ *Postgres }

func (p claimedVC) Capabilities() []string { return []string{"sql", "vc.core"} }

// SelfTest proves that native adapter failures on engine am cannot be
// reported as passes or silently excluded as unsupported. These are separate
// from corpus coverage.
func SelfTest(ctx context.Context, dsn, am string, timeout time.Duration) ([]Result, error) {
	var results []Result
	for _, tc := range []struct{ id, sql, match string }{
		{"wrong-result", "SELECT 'wrong'::text", "rows differ"},
		{"unexpected-error", "SELECT missing_column", "SQLSTATE"},
	} {
		rows := [][]Value{{{Text: func() *string { s := "expected"; return &s }()}}}
		st := Step{ID: tc.id, Session: "a", Op: "query", SQL: tc.sql, Expect: &Expected{Rows: &rows, Order: "ordered"}}
		s := Scenario{Version: 1, ID: tc.id, Description: "negative control", Requires: []string{"sql"}, Sessions: []string{"a"}, Steps: []Step{st}}
		a, err := NewPostgres(dsn, am, timeout)
		if err != nil {
			return results, err
		}
		r := Run(ctx, s, a)
		results = append(results, r)
		if r.Status != "failed" || r.Phase != "step" || !strings.Contains(r.Error, tc.match) {
			return results, fmt.Errorf("negative control %s did not fail as intended: %+v", tc.id, r)
		}
	}
	// A table of another AM where the adapter looks fails targeting. Only a
	// heap adapter can have one (a versioned public holds prolly tables
	// alone), so the control runs on heap whatever am is: the AM is heap's
	// under another name.
	wrong := []Step{
		{ID: "rogue-am", Session: "a", Op: "exec", SQL: "CREATE ACCESS METHOD vc_rogue_am TYPE TABLE HANDLER heap_tableam_handler"},
		{ID: "wrong-am", Session: "a", Op: "exec", SQL: "CREATE TABLE rogue (id int) USING vc_rogue_am"},
	}
	ws := Scenario{Version: 1, ID: "wrong-am", Description: "negative control", Requires: []string{"sql"}, Sessions: []string{"a"}, Steps: wrong}
	wa, err := NewPostgres(dsn, "heap", timeout)
	if err != nil {
		return results, err
	}
	r := Run(ctx, ws, wa)
	results = append(results, r)
	if r.Status != "failed" || r.Phase != "step" || !strings.HasPrefix(r.Error, "wrong-am: ") ||
		!strings.Contains(r.Error, "wrong AM") {
		return results, fmt.Errorf("negative control wrong-am did not fail as intended: %+v", r)
	}
	s, err := Parse(targetScenario)
	if err != nil {
		return results, err
	}
	p, _ := NewPostgres(dsn, "heap", timeout)
	r = Run(ctx, s, claimedVC{p})
	results = append(results, r)
	// Heap tables aren't branched: one branch's edits show on another, the
	// probe's isolation step failing.
	if r.Status != "failed" || r.Phase != "target" || !strings.Contains(r.Error, "VC canary: main-isolation:") {
		return results, fmt.Errorf("a false VC capability claim did not fail targeting: %+v", r)
	}
	// (heap declares no VC capability, and must report the probe unsupported)
	p, _ = NewPostgres(dsn, "heap", timeout)
	r = Run(ctx, s, p)
	results = append(results, r)
	if r.Status != "unsupported" || len(r.Missing) != 1 || r.Missing[0] != "vc.core" || len(r.Steps) != 0 {
		return results, fmt.Errorf("missing VC capability was counted as coverage: %+v", r)
	}
	return results, nil
}
