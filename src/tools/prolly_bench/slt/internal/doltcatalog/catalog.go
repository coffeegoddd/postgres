// Package doltcatalog inventories pinned upstream source without executing Dolt.
package doltcatalog

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"go/ast"
	"go/parser"
	"go/printer"
	"go/token"
	"os/exec"
	"regexp"
	"sort"
	"strconv"
	"strings"
)

const Repository = "https://github.com/dolthub/dolt"
const Directory = "go/libraries/doltcore/sqle/enginetest/"
const Scope = "top-level literal arrays of queries.ScriptTest, queries.TransactionTest and MergeScriptTest; source entries, not executed tests; excludes singletons, generated/appended cases, other Go tests and Bats"

type Counts struct {
	Cases            int `json:"cases"`
	Assertions       int `json:"assertions"`
	UnresolvedFields int `json:"unresolved_fields"`
}
type File struct {
	Path   string `json:"path"`
	SHA256 string `json:"sha256"`
	Counts Counts `json:"counts"`
}
type Lock struct {
	Version    int    `json:"version"`
	Repository string `json:"repository"`
	Revision   string `json:"revision"`
	Scope      string `json:"scope"`
	Counts     Counts `json:"counts"`
	Files      []File `json:"files"`
	// Sources other ports derive from, pinned by file hash only: Dolt's Go
	// tests that don't run its engine (merge truth tables, merge bases,
	// revision specs), at Revision, and Doltgres's Postgres-dialect tests.
	GoTests  *Pin `json:"go_tests,omitempty"`
	Doltgres *Pin `json:"doltgres,omitempty"`
}

// Pin is files of a repository at a commit, by hash.
type Pin struct {
	Repository string       `json:"repository"`
	Revision   string       `json:"revision"`
	Files      []PinnedFile `json:"files"`
}
type PinnedFile struct {
	Path   string `json:"path"`
	SHA256 string `json:"sha256"`
}

// The pinned Dolt Go tests, and Doltgres's version-control tests.
var GoTestFiles = []string{
	"go/libraries/doltcore/merge/merge_test.go",
	"go/libraries/doltcore/merge/row_merge_test.go",
	"go/libraries/doltcore/merge/row_merge_policy_test.go",
	"go/libraries/doltcore/merge/keyless_integration_test.go",
	"go/libraries/doltcore/merge/schema_merge_test.go",
	"go/libraries/doltcore/merge/integration_test.go",
	"go/store/prolly/tree/three_way_differ_test.go",
	"go/store/datas/commit_test.go",
	"go/libraries/doltcore/doltdb/ancestor_spec_test.go",
	"go/libraries/doltcore/doltdb/commit_spec_test.go",
	"go/libraries/doltcore/doltdb/branch_case_test.go",
	"go/libraries/doltcore/ref/branchname_test.go",
	"go/libraries/doltcore/ref/ref_test.go",
}

const DoltgresRepository = "https://github.com/dolthub/doltgresql"

var DoltgresFiles = []string{
	"testing/go/auth_test.go",
	"testing/go/conflicts_root_object_test.go",
	"testing/go/cursor_test.go",
	"testing/go/dolt_functions_test.go",
	"testing/go/dolt_procedures_record_test.go",
	"testing/go/dolt_tables_test.go",
	"testing/go/foreign_keys_test.go",
	"testing/go/information_schema_test.go",
	"testing/go/merge_test.go",
	"testing/go/pgcatalog_test.go",
	"testing/go/root_object_collections_test.go",
	"testing/go/schemas_test.go",
	"testing/go/sequences_test.go",
}

// PinFiles hashes files of repo at revision, from Git objects.
func PinFiles(repository, repo, revision string, paths []string) (*Pin, error) {
	git, err := gitAt(repo, revision)
	if err != nil {
		return nil, err
	}
	p := &Pin{Repository: repository, Revision: revision}
	for _, path := range paths {
		b, err := git("show", revision+":"+path)
		if err != nil {
			return nil, err
		}
		p.Files = append(p.Files, PinnedFile{Path: path, SHA256: Hash(b)})
	}
	return p, nil
}

// gitAt runs git in repo, once revision is checked to name a commit exactly.
func gitAt(repo, revision string) (func(...string) ([]byte, error), error) {
	if !regexp.MustCompile(`^[0-9a-f]{40}$`).MatchString(revision) {
		return nil, fmt.Errorf("revision must be a full lowercase commit SHA")
	}
	git := func(args ...string) ([]byte, error) {
		b, err := exec.Command("git", append([]string{"-C", repo}, args...)...).Output()
		if err != nil {
			return nil, fmt.Errorf("git %v: %w", args, err)
		}
		return b, nil
	}
	resolved, err := git("rev-parse", "--verify", revision+"^{commit}")
	if err != nil {
		return nil, err
	}
	if strings.TrimSpace(string(resolved)) != revision {
		return nil, fmt.Errorf("revision must name a commit directly")
	}
	return git, nil
}

type Flag struct {
	Field      string `json:"field"`
	Expression string `json:"expression"`
}
type Case struct {
	Path           string `json:"path"`
	Suite          string `json:"suite"`
	Index          int    `json:"index"` // zero-based position in the upstream literal
	Kind           string `json:"kind"`
	Name           string `json:"name"`
	Line           int    `json:"line"`
	SHA256         string `json:"sha256"`
	Setup          int    `json:"setup"`
	Assertions     int    `json:"assertions"`
	SQLExpressions int    `json:"sql_expressions_requiring_review"`
	Flags          []Flag `json:"upstream_flags,omitempty"`
	Unresolved     []Flag `json:"unresolved_fields,omitempty"`
	Source         string `json:"source"` // exact literal bytes; no SQL translation
}
type Catalog struct {
	Lock   Lock    `json:"lock"`
	Cases  []Case  `json:"cases"`
	Setups []Setup `json:"setups,omitempty"`
	// The shared setups cases name that no setup stands for: computed
	// rather than a literal, or not declared at top level in scope.
	UnresolvedSetups []Flag `json:"unresolved_setups,omitempty"`
}

// The reasons a case-named shared setup is unresolved.
const (
	SetupComputed   = "not a []string literal (computed, or declared without a value)"
	SetupUndeclared = "not a top-level var of the scanned files"
)

// Setup is a shared setup script: a top-level []string literal that cases
// name as their SetUpScript (an unresolved field of theirs). Its exact
// literal bytes are kept, as a case's are; the file's hash in the lock pins
// them.
type Setup struct {
	Path           string `json:"path"`
	Name           string `json:"name"`
	Line           int    `json:"line"`
	SHA256         string `json:"sha256"`
	Statements     int    `json:"statements"`
	SQLExpressions int    `json:"sql_expressions_requiring_review"`
	Source         string `json:"source"` // exact literal bytes
}

func Hash(b []byte) string { h := sha256.Sum256(b); return hex.EncodeToString(h[:]) }
func expression(fs *token.FileSet, e ast.Expr) string {
	var b bytes.Buffer
	_ = printer.Fprint(&b, fs, e)
	return b.String()
}

// Scan deliberately inventories only direct literals. It never evaluates Go,
// infers that a skipped assertion passes, or guesses at computed SQL.
func Scan(path string, source []byte) ([]Case, error) {
	fs := token.NewFileSet()
	f, err := parser.ParseFile(fs, path, source, 0)
	if err != nil {
		return nil, err
	}
	var out []Case
	err = topLevelVars(f, func(name string, val ast.Expr) error {
		lit, ok := val.(*ast.CompositeLit)
		if !ok {
			return nil
		}
		a, ok := lit.Type.(*ast.ArrayType)
		if !ok {
			return nil
		}
		kind := expression(fs, a.Elt)
		if kind != "queries.ScriptTest" && kind != "queries.TransactionTest" && kind != "MergeScriptTest" {
			return nil
		}
		for index, el := range lit.Elts {
			row, ok := el.(*ast.CompositeLit)
			if !ok {
				return fmt.Errorf("%s/%s[%d]: nonliteral case requires catalog support", path, name, index)
			}
			start, end := fs.Position(row.Pos()), fs.Position(row.End())
			raw := source[start.Offset:end.Offset]
			c := Case{Path: path, Suite: name, Index: index, Kind: kind, Line: start.Line, SHA256: Hash(raw), Source: string(raw)}
			for _, field := range row.Elts {
				kv, ok := field.(*ast.KeyValueExpr)
				if !ok {
					continue
				}
				key, ok := kv.Key.(*ast.Ident)
				if !ok {
					continue
				}
				switch key.Name {
				case "Name":
					x, ok := kv.Value.(*ast.BasicLit)
					if !ok || x.Kind != token.STRING {
						return fmt.Errorf("%s:%d: computed case name", path, c.Line)
					}
					var err error
					c.Name, err = strconv.Unquote(x.Value)
					if err != nil {
						return err
					}
				case "Assertions", "SetUpScript", "AncSetUpScript", "LeftSetUpScript", "RightSetUpScript":
					if x, ok := kv.Value.(*ast.Ident); ok && x.Name == "nil" {
						continue
					}
					x, ok := kv.Value.(*ast.CompositeLit)
					if !ok {
						c.Unresolved = append(c.Unresolved, Flag{key.Name, expression(fs, kv.Value)})
						continue
					}
					if key.Name == "Assertions" {
						c.Assertions = len(x.Elts)
					} else {
						c.Setup += len(x.Elts)
					}
				}
			}
			ast.Inspect(row, func(n ast.Node) bool {
				kv, ok := n.(*ast.KeyValueExpr)
				if !ok {
					return true
				}
				key, ok := kv.Key.(*ast.Ident)
				if !ok {
					return true
				}
				if strings.HasPrefix(key.Name, "Skip") {
					c.Flags = append(c.Flags, Flag{key.Name, expression(fs, kv.Value)})
				}
				count := func(e ast.Expr) {
					if !literalSQL(e) {
						c.SQLExpressions++
					}
				}
				switch key.Name {
				case "Query":
					count(kv.Value)
				case "SetUpScript", "AncSetUpScript", "LeftSetUpScript", "RightSetUpScript":
					if x, ok := kv.Value.(*ast.CompositeLit); ok {
						for _, e := range x.Elts {
							count(e)
						}
					}
				}
				return true
			})
			out = append(out, c)
		}
		return nil
	})
	return out, err
}

// literalSQL is whether a statement is literal SQL, a string literal: the
// one rule cases' and shared setups' statements are both counted by.
func literalSQL(e ast.Expr) bool {
	x, ok := e.(*ast.BasicLit)
	return ok && x.Kind == token.STRING
}

// topLevelVars calls fn with each name and value of f's top-level var
// declarations (a nil value for one declared without), stopping at its
// first error.
func topLevelVars(f *ast.File, fn func(name string, val ast.Expr) error) error {
	for _, decl := range f.Decls {
		g, ok := decl.(*ast.GenDecl)
		if !ok || g.Tok != token.VAR {
			continue
		}
		for _, spec := range g.Specs {
			v := spec.(*ast.ValueSpec)
			for i, name := range v.Names {
				var val ast.Expr // (nil: declared without a value)
				if i < len(v.Values) {
					val = v.Values[i]
				}
				if err := fn(name.Name, val); err != nil {
					return err
				}
			}
		}
	}
	return nil
}

// SharedSetupNames are the shared setups cases name: the identifiers their
// unresolved SetUpScript fields are. (An expression, an append() say, is
// left unresolved: no setup stands for it.)
func SharedSetupNames(cases []Case) map[string]bool {
	names := map[string]bool{}
	for _, c := range cases {
		for _, u := range c.Unresolved {
			if u.Field == "SetUpScript" && token.IsIdentifier(u.Expression) {
				names[u.Expression] = true
			}
		}
	}
	return names
}

// ScanSetups inventories the top-level []string literals of source that
// names lists, as Scan does cases: their exact bytes, never evaluated. A
// named var computed instead is returned among computed, never guessed at.
func ScanSetups(path string, source []byte, names map[string]bool) (setups []Setup, computed []string, err error) {
	fs := token.NewFileSet()
	f, err := parser.ParseFile(fs, path, source, 0)
	if err != nil {
		return nil, nil, err
	}
	err = topLevelVars(f, func(name string, val ast.Expr) error {
		if !names[name] {
			return nil
		}
		lit, ok := val.(*ast.CompositeLit)
		if !ok || expression(fs, lit.Type) != "[]string" {
			computed = append(computed, name)
			return nil
		}
		start, end := fs.Position(lit.Pos()), fs.Position(lit.End())
		raw := source[start.Offset:end.Offset]
		s := Setup{Path: path, Name: name, Line: start.Line, SHA256: Hash(raw), Statements: len(lit.Elts), Source: string(raw)}
		for _, e := range lit.Elts {
			if !literalSQL(e) {
				s.SQLExpressions++
			}
		}
		setups = append(setups, s)
		return nil
	})
	return setups, computed, err
}

// Undeclared flags, in name order, each of names that found lacks.
func Undeclared(names, found map[string]bool) []Flag {
	var out []Flag
	for name := range names {
		if !found[name] {
			out = append(out, Flag{name, SetupUndeclared})
		}
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Field < out[j].Field })
	return out
}

// Read uses Git objects at an exact commit, ignoring dirty working-tree files.
func Read(repo, revision string) (Catalog, error) {
	c := Catalog{Lock: Lock{Version: 1, Repository: Repository, Revision: revision, Scope: Scope}}
	git, err := gitAt(repo, revision)
	if err != nil {
		return c, err
	}
	paths, err := git("ls-tree", "-r", "--name-only", revision, "--", Directory)
	if err != nil {
		return c, err
	}
	sources := map[string][]byte{}
	for _, path := range strings.Split(strings.TrimSpace(string(paths)), "\n") {
		relative := strings.TrimPrefix(path, Directory)
		if strings.Contains(relative, "/") || !strings.HasSuffix(path, ".go") {
			continue
		}
		source, err := git("show", revision+":"+path)
		if err != nil {
			return c, err
		}
		sources[path] = source
		cases, err := Scan(path, source)
		if err != nil {
			return c, err
		}
		file := File{Path: path, SHA256: Hash(source), Counts: Counts{Cases: len(cases)}}
		for _, item := range cases {
			file.Counts.Assertions += item.Assertions
			file.Counts.UnresolvedFields += len(item.Unresolved)
		}
		c.Lock.Files = append(c.Lock.Files, file) // include zero-case files to detect scope changes
		c.Lock.Counts.Cases += file.Counts.Cases
		c.Lock.Counts.Assertions += file.Counts.Assertions
		c.Lock.Counts.UnresolvedFields += file.Counts.UnresolvedFields
		c.Cases = append(c.Cases, cases...)
	}
	if len(c.Cases) == 0 {
		return c, fmt.Errorf("no cases found in pinned source")
	}
	// (one package: a shared setup is declared once, in any of its files)
	// A named setup no literal stands for is recorded unresolved, not an
	// error: only the cases naming it can't be ported by it.
	names := SharedSetupNames(c.Cases)
	found := map[string]bool{}
	for _, file := range c.Lock.Files {
		setups, computed, err := ScanSetups(file.Path, sources[file.Path], names)
		if err != nil {
			return c, err
		}
		for _, s := range setups {
			found[s.Name] = true
		}
		for _, name := range computed {
			found[name] = true
			c.UnresolvedSetups = append(c.UnresolvedSetups, Flag{name, SetupComputed})
		}
		c.Setups = append(c.Setups, setups...)
	}
	c.UnresolvedSetups = append(c.UnresolvedSetups, Undeclared(names, found)...)
	c.Lock.GoTests, err = PinFiles(Repository, repo, revision, GoTestFiles)
	return c, err
}
