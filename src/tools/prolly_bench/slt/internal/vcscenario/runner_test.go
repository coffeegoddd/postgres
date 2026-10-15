package vcscenario

import (
	"context"
	"encoding/json"
	"errors"
	"reflect"
	"strings"
	"testing"
)

func str(s string) *string { return &s }

type fake struct {
	caps                []string
	verifyErr, closeErr error
	opened, closed      bool
	steps               []Step
	args                [][]*string
	out                 []Outcome
	err                 error
}

func (f *fake) Name() string                              { return "fake" }
func (f *fake) Capabilities() []string                    { return f.caps }
func (f *fake) Verify(context.Context, []string) error    { return f.verifyErr }
func (f *fake) Open(context.Context, []string) error      { f.opened = true; return nil }
func (f *fake) Close(context.Context) error               { f.closed = true; return f.closeErr }
func (f *fake) CheckTarget(context.Context, string) error { return nil }
func (f *fake) Execute(_ context.Context, st Step, params, args []*string) (Outcome, error) {
	f.steps = append(f.steps, st)
	f.args = append(f.args, append(params, args...))
	if f.err != nil {
		return Outcome{}, f.err
	}
	return f.out[len(f.steps)-1], nil
}

func parseTest(t *testing.T, text string) Scenario {
	t.Helper()
	s, e := Parse([]byte(text))
	if e != nil {
		t.Fatal(e)
	}
	return s
}

const oneQuery = `{"version":1,"id":"rows","description":"values","requires":["sql"],"sessions":["a"],"steps":[{"id":"q","op":"query","session":"a","sql":"SELECT x FROM t","expect":{"rows":[[null],[""],["9007199254740993"],["same"],["same"]],"order":"bag"}}]}`

func TestExactValuesAndMultiplicity(t *testing.T) {
	s := parseTest(t, oneQuery)
	for _, tc := range []struct {
		name string
		rows [][]*string
		pass bool
	}{
		{"reordered", [][]*string{{str("same")}, {str("9007199254740993")}, {str("")}, {nil}, {str("same")}}, true},
		{"null_is_not_empty", [][]*string{{str("same")}, {str("9007199254740993")}, {str("")}, {str("")}, {str("same")}}, false},
		{"no_float_rounding", [][]*string{{nil}, {str("")}, {str("9007199254740992")}, {str("same")}, {str("same")}}, false},
		{"duplicates_matter", [][]*string{{nil}, {str("")}, {str("9007199254740993")}, {str("same")}}, false},
	} {
		t.Run(tc.name, func(t *testing.T) {
			f := &fake{caps: []string{"sql"}, out: []Outcome{{Rows: tc.rows}}}
			r := Run(context.Background(), s, f)
			if (r.Status == "passed") != tc.pass || !f.closed {
				t.Fatalf("%+v", r)
			}
		})
	}
	s.Steps[0].Expect.Order = "ordered"
	f := &fake{caps: []string{"sql"}, out: []Outcome{{Rows: [][]*string{{str("same")}, {nil}, {str("")}, {str("9007199254740993")}, {str("same")}}}}}
	if Run(context.Background(), s, f).Status != "failed" {
		t.Fatal("ordered rows were sorted")
	}
}

func TestBindingsSessionsAndReplay(t *testing.T) {
	s := parseTest(t, `{"version":1,"id":"history","description":"bound commits","requires":["sql","vc.core"],"sessions":["a","b"],"steps":[{"id":"c","op":"commit","session":"a","args":["base"],"save":"base"},{"id":"b","op":"branch","session":"b","args":["feature",{"ref":"base"}]},{"id":"q","op":"query","session":"b","sql":"SELECT $1","params":[{"ref":"base"}],"expect":{"rows":[[{"ref":"base"}]],"order":"ordered"}}]}`)
	b, e := json.Marshal(s)
	if e != nil {
		t.Fatal(e)
	}
	replay, e := Parse(b)
	if e != nil || !reflect.DeepEqual(s, replay) {
		t.Fatalf("replay: %s %v", b, e)
	}
	f := &fake{caps: s.Requires, out: []Outcome{{Rows: [][]*string{{str("engine-specific-id")}}}, {}, {Rows: [][]*string{{str("engine-specific-id")}}}}}
	r := Run(context.Background(), s, f)
	if r.Status != "passed" || *f.args[1][1] != "engine-specific-id" || f.steps[1].Session != "b" {
		t.Fatalf("%+v", r)
	}
}

func TestFailureIsNeverUnsupportedOrSuccess(t *testing.T) {
	s := parseTest(t, oneQuery)
	for _, tc := range []struct {
		name          string
		f             fake
		status, phase string
	}{
		{"missing", fake{}, "unsupported", ""},
		{"target", fake{caps: []string{"sql"}, verifyErr: errors.New("wrong AM")}, "failed", "target"},
		{"sql", fake{caps: []string{"sql"}, out: []Outcome{{SQLState: "42703"}}}, "failed", "step"},
		{"transport", fake{caps: []string{"sql"}, err: errors.New("connection lost")}, "failed", "step"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			r := Run(context.Background(), s, &tc.f)
			if r.Status != tc.status || r.Phase != tc.phase {
				t.Fatalf("%+v", r)
			}
			if (tc.phase == "" || tc.phase == "target") && tc.f.opened {
				t.Fatal("opened an unverified target")
			}
		})
	}
	s.Steps[0].Expect = &Expected{SQLState: "23505", Transaction: "E"}
	f := &fake{caps: []string{"sql"}, out: []Outcome{{SQLState: "23505", Transaction: "E"}}, closeErr: errors.New("drop failed")}
	r := Run(context.Background(), s, f)
	if r.Status != "failed" || r.Phase != "cleanup" {
		t.Fatalf("%+v", r)
	}
	f.closeErr = nil
	if r = Run(context.Background(), s, &fake{caps: f.caps, out: f.out}); r.Status != "passed" {
		t.Fatalf("%+v", r)
	}
}

func TestRejectMalformedOrAmbiguousScenarios(t *testing.T) {
	for _, bad := range []string{
		strings.Replace(oneQuery, `"version":1`, `"version":2`, 1),
		strings.Replace(oneQuery, `"description":"values"`, `"description":"values","skip":true`, 1),
		strings.Replace(oneQuery, `"sql"]`, `"unknown"]`, 1),
		strings.Replace(oneQuery, `"session":"a"`, `"session":"missing"`, 1),
		strings.Replace(oneQuery, `"order":"bag"`, `"order":"whatever"`, 1),
		strings.Replace(oneQuery, `[null]`, `[{"ref":"missing"}]`, 1),
		strings.Replace(oneQuery, `[null]`, `[42]`, 1),
		oneQuery + ` {}`,
	} {
		if _, err := Parse([]byte(bad)); err == nil {
			t.Fatalf("accepted %s", bad)
		}
	}
	for _, b := range []string{`{"ref":""}`, `{"ref":"x","ignored":true}`} {
		var v Value
		if json.Unmarshal([]byte(b), &v) == nil {
			t.Fatalf("accepted %s", b)
		}
	}
}
