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
	Lock  Lock   `json:"lock"`
	Cases []Case `json:"cases"`
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
	for _, decl := range f.Decls {
		g, ok := decl.(*ast.GenDecl)
		if !ok || g.Tok != token.VAR {
			continue
		}
		for _, spec := range g.Specs {
			v := spec.(*ast.ValueSpec)
			for i, val := range v.Values {
				lit, ok := val.(*ast.CompositeLit)
				if !ok {
					continue
				}
				a, ok := lit.Type.(*ast.ArrayType)
				if !ok {
					continue
				}
				kind := expression(fs, a.Elt)
				if kind != "queries.ScriptTest" && kind != "queries.TransactionTest" && kind != "MergeScriptTest" {
					continue
				}
				for index, el := range lit.Elts {
					row, ok := el.(*ast.CompositeLit)
					if !ok {
						return nil, fmt.Errorf("%s/%s[%d]: nonliteral case requires catalog support", path, v.Names[i].Name, index)
					}
					start, end := fs.Position(row.Pos()), fs.Position(row.End())
					raw := source[start.Offset:end.Offset]
					c := Case{Path: path, Suite: v.Names[i].Name, Index: index, Kind: kind, Line: start.Line, SHA256: Hash(raw), Source: string(raw)}
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
								return nil, fmt.Errorf("%s:%d: computed case name", path, c.Line)
							}
							c.Name, err = strconv.Unquote(x.Value)
							if err != nil {
								return nil, err
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
							if x, ok := e.(*ast.BasicLit); !ok || x.Kind != token.STRING {
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
			}
		}
	}
	return out, nil
}

// Read uses Git objects at an exact commit, ignoring dirty working-tree files.
func Read(repo, revision string) (Catalog, error) {
	c := Catalog{Lock: Lock{Version: 1, Repository: Repository, Revision: revision, Scope: Scope}}
	if !regexp.MustCompile(`^[0-9a-f]{40}$`).MatchString(revision) {
		return c, fmt.Errorf("revision must be a full lowercase commit SHA")
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
		return c, err
	}
	if strings.TrimSpace(string(resolved)) != revision {
		return c, fmt.Errorf("revision must name a commit directly")
	}
	paths, err := git("ls-tree", "-r", "--name-only", revision, "--", Directory)
	if err != nil {
		return c, err
	}
	for _, path := range strings.Split(strings.TrimSpace(string(paths)), "\n") {
		relative := strings.TrimPrefix(path, Directory)
		if strings.Contains(relative, "/") || !strings.HasSuffix(path, ".go") {
			continue
		}
		source, err := git("show", revision+":"+path)
		if err != nil {
			return c, err
		}
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
	return c, nil
}
