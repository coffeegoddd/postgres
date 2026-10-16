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
