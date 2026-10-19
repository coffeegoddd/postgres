package vcscenario

import (
	"context"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/jackc/pgx/v5/pgconn"
)

func TestBundledScenariosAndCapabilityExclusions(t *testing.T) {
	paths, err := filepath.Glob("cases/*.json")
	if err != nil || len(paths) != 4 {
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
	for _, am := range []string{"heap"} {
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
	}
	if _, err := NewPostgres("", "typo", time.Second); err == nil {
		t.Fatal("unknown engine accepted")
	}
}

// A connect step's database: the case's, or a branch of it, and the branch
// the connection must be on (the default one for the database alone); a
// name the server would cut short is refused.
func TestConnectTarget(t *testing.T) {
	db := "vc_case_0123456789abcdef"
	fits := strings.Repeat("b", maxDatabaseName-len(db)-1)
	long := fits + "b"
	for i, tc := range []struct {
		args           []*string
		target, branch string
		ok             bool
	}{
		{nil, db, "main", true},
		{[]*string{str("b2")}, db + "/b2", "b2", true},
		{[]*string{&fits}, db + "/" + fits, fits, true},
		{[]*string{&long}, "", "", false},
	} {
		target, branch, err := connectTarget(db, tc.args)
		if target != tc.target || branch != tc.branch || (err == nil) != tc.ok {
			t.Fatalf("case %d: %q %q %v", i, target, branch, err)
		}
	}
}

// Only a refused branch is a connect step's outcome; any other refusal is
// the target's failure.
func TestBranchRefusal(t *testing.T) {
	for _, tc := range []struct {
		err   error
		named bool
		code  string
	}{
		{&pgconn.PgError{Severity: "FATAL", Code: "42704"}, true, "42704"},
		{fmt.Errorf("connect: %w", &pgconn.PgError{Severity: "FATAL", Code: "42704"}), true, "42704"},
		{&pgconn.PgError{Severity: "FATAL", Code: "42704"}, false, ""},
		{&pgconn.PgError{Severity: "FATAL", Code: "53300"}, true, ""},
		{&pgconn.PgError{Severity: "FATAL", Code: "57P03"}, true, ""},
		{&pgconn.PgError{Severity: "FATAL", Code: "28P01"}, true, ""},
		{&pgconn.PgError{Severity: "FATAL", Code: "3D000"}, true, ""},
		{&pgconn.PgError{Severity: "ERROR", Code: "42704"}, true, ""},
		{errors.New("dial: connection refused"), true, ""},
	} {
		if code := branchRefusal(tc.err, tc.named); code != tc.code {
			t.Fatalf("%v (branch named %v): %q", tc.err, tc.named, code)
		}
	}
}

type closer struct {
	err    error
	closed int
}

func (c *closer) Close(context.Context) error { c.closed++; return c.err }

// A session's connection replaced: the old one closed, and if that fails
// neither is the session's, the new one closed too.
func TestReplace(t *testing.T) {
	ctx := context.Background()
	old, c := &closer{}, &closer{}
	sessions := map[string]*closer{"a": old}
	if err := replace(ctx, sessions, "a", c); err != nil || sessions["a"] != c || old.closed != 1 || c.closed != 0 {
		t.Fatalf("%v %v %+v %+v", err, sessions, old, c)
	}
	old, c = &closer{err: errors.New("close")}, &closer{}
	sessions = map[string]*closer{"a": old}
	if err := replace(ctx, sessions, "a", c); err == nil || len(sessions) != 0 || old.closed != 1 || c.closed != 1 {
		t.Fatalf("%v %v %+v %+v", err, sessions, old, c)
	}
}
