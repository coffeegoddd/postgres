package vcfuzz

import (
	"encoding/json"
	"reflect"
	"strings"
	"testing"
)

func TestWindowIDsFollowTheModel(t *testing.T) {
	s := emptyState()
	s["t"][1] = Row{0, 1} // a line over x 10..16, y 7.25..11.25
	s["t"][2] = Row{0, 3} // empty
	s["t"][4] = Row{0, 1} // a line over x 40..46
	s["t"][6] = Row{0, 3} // empty
	s["t"][5] = Row{0, 2} // a rhombus over x 50..54, y 20..24
	s["t"][8] = Row{0, 3} // the large polygon, over x 80..88
	s["u"][1] = Row{0, 1} // (u has no geometries)
	got := WindowIDs(s)
	want := [][]int{{1, 4}, {4, 5}, {5, 8}, {8}}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("windows %v, want %v", got, want)
	}
	if Geom(8, 3).Kind != "large" || Geom(4, 7).Kind != "large" || Geom(2, 3).Kind != "empty" || Geom(6, 3).Kind != "empty" {
		t.Fatal("rows 4 and 8's fourth kind is the large polygon, another even row's empty")
	}
	// every window holds some row the generator can make
	held := make([]bool, len(Windows))
	for id := 1; id <= keys; id++ {
		for b := 0; b < 4; b++ {
			one := emptyState()
			one["t"][id] = Row{0, b}
			for i, ids := range WindowIDs(one) {
				held[i] = held[i] || len(ids) > 0
			}
		}
	}
	for i, h := range held {
		if !h {
			t.Fatalf("window %d holds no row", i)
		}
	}
}

// A write of b writes g with it, for t in a history with PostGIS only.
func TestRowSQLWritesGeometryWithB(t *testing.T) {
	ins := Op{Kind: OpInsert, Table: "t", ID: 3, Val: 1, Val2: 2}
	if sql := rowSQL(ins, true); !strings.Contains(sql, Geom(3, 2).SQL) || !strings.Contains(sql, "g = excluded.g") {
		t.Fatalf("insert: %s", sql)
	}
	upd := Op{Kind: OpUpdate, Table: "t", ID: 3, Col: 1, Val: 0}
	if sql := rowSQL(upd, true); !strings.Contains(sql, "b = 0, g = "+Geom(3, 0).SQL) {
		t.Fatalf("update of b: %s", sql)
	}
	for _, op := range []Op{{Kind: OpUpdate, Table: "t", ID: 3, Col: 0, Val: 2}, {Kind: OpUpdate, Table: "t", ID: 3, Col: 2, Val: 2}} {
		if sql := rowSQL(op, true); strings.Contains(sql, "g =") {
			t.Fatalf("update of another column writes g: %s", sql)
		}
	}
	for _, op := range []Op{ins, upd} {
		if sql := rowSQL(op, false); strings.Contains(sql, "geometry") || strings.Contains(sql, " g") {
			t.Fatalf("without PostGIS: %s", sql)
		}
	}
	if (&Postgres{postgis: true}).geo("u") || (&Postgres{}).geo("t") || !(&Postgres{postgis: true}).geo("t") {
		t.Fatal("only t has geometries, only with PostGIS")
	}
}

func TestHistoryRecordsPostGIS(t *testing.T) {
	h := Generate(5, 40, 2)
	h.PostGIS = true
	b, err := json.Marshal(h)
	if err != nil {
		t.Fatal(err)
	}
	var back History
	if err := json.Unmarshal(b, &back); err != nil || !back.PostGIS || back.Validate() != nil {
		t.Fatalf("replayed: %v %v", back.PostGIS, err)
	}
	plain, _ := json.Marshal(Generate(5, 40, 2))
	if strings.Contains(string(plain), "postgis") {
		t.Fatal("a history without PostGIS records it")
	}
}
