package doltcatalog

import (
	"strings"
	"testing"
)

func TestLiteralInventory(t *testing.T) {
	src := []byte(`package fixture
var Tests = []queries.ScriptTest{
 {Name:"quoted \"case\"", SetUpScript:[]string{"create table t (x int)", fmt.Sprintf("insert %d", 1)}, SkipPrepared:true,
 Assertions:[]queries.ScriptTestAssertion{{Query:"select 1", Expected:[]sql.Row{{1}}}, {Query:computed, SkipResultsCheck:true}}},
}
var Single = queries.ScriptTest{Name:"outside declared scope"}
var Other = []queries.QueryTest{{Query:"select 2"}}
`)
	cases, err := Scan("fixture.go", src)
	if err != nil {
		t.Fatal(err)
	}
	if len(cases) != 1 {
		t.Fatalf("cases: %v", cases)
	}
	c := cases[0]
	if c.Name != "quoted \"case\"" || c.Suite != "Tests" || c.Index != 0 || c.Setup != 2 || c.Assertions != 2 || c.SQLExpressions != 2 || len(c.Flags) != 2 {
		t.Fatalf("%+v", c)
	}
	if c.SHA256 != Hash([]byte(c.Source)) || !strings.Contains(string(src), c.Source) {
		t.Fatal("source bytes were changed")
	}
}
func TestComputedEntriesFailClosed(t *testing.T) {
	for _, body := range []string{
		`var T = []queries.ScriptTest{makeCase()}`,
		`var T = []queries.ScriptTest{{Name:computed}}`,
	} {
		if _, err := Scan("fixture.go", []byte("package fixture\n"+body)); err == nil {
			t.Fatalf("accepted %s", body)
		}
	}
}

func TestComputedFieldsRemainUnresolved(t *testing.T) {
	cases, err := Scan("fixture.go", []byte(`package fixture
var T = []queries.ScriptTest{{Name:"a", SetUpScript:nil, Assertions:shared}}`))
	if err != nil {
		t.Fatal(err)
	}
	if len(cases) != 1 || cases[0].Assertions != 0 || len(cases[0].Unresolved) != 1 || cases[0].Unresolved[0].Field != "Assertions" {
		t.Fatalf("%+v", cases)
	}
}

func TestSharedSetups(t *testing.T) {
	src := []byte(`package fixture
var shared = []string{
	"create table t (x int)",
	"insert into t values (1)",
}
var computed = []string{"select 1", fmt.Sprint(2)}
var unused = []string{"select 3"}
var T = []queries.ScriptTest{{Name:"a", SetUpScript:shared}, {Name:"b", SetUpScript:computed}, {Name:"c", SetUpScript:append(shared, "x")}}
`)
	cases, err := Scan("fixture.go", src)
	if err != nil {
		t.Fatal(err)
	}
	names := SharedSetupNames(cases)
	if len(names) != 2 || !names["shared"] || !names["computed"] {
		t.Fatalf("%v", names)
	}
	setups, computed, err := ScanSetups("fixture.go", src, names)
	if err != nil || len(computed) != 0 {
		t.Fatal(err, computed)
	}
	if len(setups) != 2 || setups[0].Name != "shared" || setups[0].Statements != 2 || setups[0].SQLExpressions != 0 ||
		setups[1].Name != "computed" || setups[1].SQLExpressions != 1 {
		t.Fatalf("%+v", setups)
	}
	if setups[0].SHA256 != Hash([]byte(setups[0].Source)) || !strings.HasPrefix(setups[0].Source, "[]string{") || !strings.Contains(string(src), setups[0].Source) {
		t.Fatal("setup bytes were changed")
	}
	// A computed setup is reported, not an error, and not a setup: the
	// catalog records it unresolved, as it does a name no file declares.
	setups, computed, err = ScanSetups("fixture.go", []byte("package fixture\nvar shared = makeSetup()\n"), names)
	if err != nil || len(setups) != 0 || len(computed) != 1 || computed[0] != "shared" {
		t.Fatalf("computed shared setup: %v %+v %v", err, setups, computed)
	}
	// (one declared without a value, set elsewhere, is computed too)
	setups, computed, err = ScanSetups("fixture.go", []byte("package fixture\nvar shared []string\nvar a, local = []string{}, []string{\"x\"}\n"), names)
	if err != nil || len(setups) != 0 || len(computed) != 1 || computed[0] != "shared" {
		t.Fatalf("shared setup without a value: %v %+v %v", err, setups, computed)
	}
	gaps := Undeclared(map[string]bool{"shared": true, "local": true, "a": true}, map[string]bool{"shared": true})
	if len(gaps) != 2 || gaps[0] != (Flag{"a", SetupUndeclared}) || gaps[1] != (Flag{"local", SetupUndeclared}) {
		t.Fatalf("undeclared: %+v", gaps)
	}
}
