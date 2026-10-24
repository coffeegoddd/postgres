package geomgen

import (
	"fmt"
	"math"
	"math/rand"
	"reflect"
	"regexp"
	"strconv"
	"strings"
	"testing"
)

var number = regexp.MustCompile(`-?[0-9]+(\.[0-9]+)?`)

var dimsTag = regexp.MustCompile(`^[A-Z]+ (ZM|Z|M) `)

// coordinates is the x and y of each vertex of a WKT literal (its Z and M
// left out), or false for an expression that isn't one.
func coordinates(t *testing.T, sql string) (xs, ys []float64, ok bool) {
	t.Helper()
	sql = strings.TrimSuffix(strings.TrimPrefix(sql, "postgis_addbbox("), ")")
	if !strings.HasPrefix(sql, "'") || !strings.HasSuffix(sql, "'::geometry") {
		return nil, nil, false
	}
	wkt := strings.TrimSuffix(strings.TrimPrefix(sql, "'"), "'::geometry")
	if !strings.ContainsAny(wkt, "(") {
		return nil, nil, true // EMPTY
	}
	per := 2
	if m := dimsTag.FindStringSubmatch(wkt); m != nil {
		per += len(m[1])
	}
	for i, s := range number.FindAllString(wkt, -1) {
		f, err := strconv.ParseFloat(s, 64)
		if err != nil {
			t.Fatal(err)
		}
		switch i % per {
		case 0:
			xs = append(xs, f)
		case 1:
			ys = append(ys, f)
		}
	}
	return xs, ys, true
}

func TestRandomIsSeededAndCoversEveryKind(t *testing.T) {
	gen := func(seed int64) []Geom {
		r := rand.New(rand.NewSource(seed))
		var gs []Geom
		for i := 0; i < 2000; i++ {
			gs = append(gs, Random(r, 50, 1000))
		}
		return gs
	}
	a := gen(1)
	if !reflect.DeepEqual(a, gen(1)) {
		t.Fatal("a seed must make the same geometries")
	}
	if reflect.DeepEqual(a, gen(2)) {
		t.Fatal("seeds made the same geometries")
	}
	kinds := map[string]int{}
	for _, g := range a {
		kinds[g.Kind]++
	}
	for _, k := range []string{"point", "line", "polygon", "multipoint", "collection", "empty", "large", "curve", "invalid"} {
		if kinds[k] == 0 {
			t.Fatalf("no %s in %v", k, kinds)
		}
	}
	all := ""
	for _, g := range a {
		all += g.SQL + "\n"
		if g.Refused {
			t.Fatalf("Random made a refused value: %s", g.SQL)
		}
	}
	for _, want := range []string{"POINT Z (", "POINT M (", "POINT ZM (", "LINESTRING ZM (", "POLYGON Z ((",
		"CIRCULARSTRING(", "COMPOUNDCURVE(", "CURVEPOLYGON(", "postgis_addbbox("} {
		if !strings.Contains(all, want) {
			t.Fatalf("no %s among Random's values", want)
		}
	}
	r := rand.New(rand.NewSource(3))
	for i := 0; i < 500; i++ {
		if g := Random(r, 0, 1000); g.Kind == "large" {
			t.Fatal("a large polygon without largeEvery")
		}
	}
}

// Every literal's coordinates are quarters, inside its box and at its
// edges (a curve's box is its arcs', checked apart); an empty has none,
// and overlaps nothing.
func TestBoxesAreExactAndHoldTheCoordinates(t *testing.T) {
	r := rand.New(rand.NewSource(7))
	checked := 0
	for i := 0; i < 3000; i++ {
		g := Random(r, 0, 0)
		xs, ys, ok := coordinates(t, g.SQL)
		if !ok || g.Kind == "curve" {
			continue
		}
		if g.Empty {
			if len(xs) != 0 || g.Overlaps(Box{-1e9, -1e9, 1e9, 1e9}) {
				t.Fatalf("empty %s", g.SQL)
			}
			continue
		}
		if len(xs) == 0 || len(xs) != len(ys) {
			t.Fatalf("bad literal %s", g.SQL)
		}
		minX, maxX, minY, maxY := math.Inf(1), math.Inf(-1), math.Inf(1), math.Inf(-1)
		for j := range xs {
			for _, v := range []float64{xs[j], ys[j]} {
				if v*4 != math.Trunc(v*4) {
					t.Fatalf("%v isn't a quarter in %s", v, g.SQL)
				}
			}
			minX, maxX = min(minX, xs[j]), max(maxX, xs[j])
			minY, maxY = min(minY, ys[j]), max(maxY, ys[j])
		}
		if (Box{minX, minY, maxX, maxY}) != g.Box {
			t.Fatalf("box %v of %s", g.Box, g.SQL)
		}
		checked++
	}
	if checked < 2000 {
		t.Fatalf("only %d literals checked", checked)
	}
}

// A curve's box is its arcs': a half circle reaches its radius above its
// ends, a circle its radius either side.
func TestCurveBoxes(t *testing.T) {
	if b := Arc(1, 2, 3).Box; b != (Box{1, 2, 7, 5}) {
		t.Fatalf("arc box %v", b)
	}
	if b := circle(1, 2, 3).Box; b != (Box{1, -1, 7, 5}) {
		t.Fatalf("circle box %v", b)
	}
	if b := compound(1, 2, 3, 4).Box; b != (Box{1, 2, 11, 5}) {
		t.Fatalf("compound box %v", b)
	}
}

// Refused values are hex EWKB a one-point line or an open ring, with
// their points' box.
func TestRefused(t *testing.T) {
	if got := onePointLine(1.25, 2.5).SQL; got != "'010200000001000000000000000000f43f0000000000000440'::geometry" {
		t.Fatalf("one-point line %s", got)
	}
	if g := unclosedRing(0, 0, 4, 4); g.Box != (Box{0, 0, 4, 4}) || !g.Refused ||
		g.SQL != "'0103000000010000000400000000000000000000000000000000000000000000000000104000000000000000000000000000001040000000000000104000000000000000000000000000001040'::geometry" {
		t.Fatalf("open ring %+v", g)
	}
	r := rand.New(rand.NewSource(4))
	lines, rings := 0, 0
	for i := 0; i < 100; i++ {
		g := Refused(r)
		if !g.Refused || g.Kind != "refused" {
			t.Fatalf("%+v", g)
		}
		if strings.HasPrefix(g.SQL, "'0102") {
			lines++
		} else if strings.HasPrefix(g.SQL, "'0103") {
			rings++
		}
	}
	if lines == 0 || rings == 0 {
		t.Fatalf("lines %d, rings %d", lines, rings)
	}
}

func TestSRIDsAndGeography(t *testing.T) {
	r := rand.New(rand.NewSource(5))
	seen := map[int]bool{}
	for i := 0; i < 200; i++ {
		g, srid := WithSRID(r, Point(1, 2))
		seen[srid] = true
		if (srid == 0) != (g.SQL == Point(1, 2).SQL) || srid != 0 && !strings.Contains(g.SQL, fmt.Sprintf(", %d)", srid)) {
			t.Fatalf("srid %d: %s", srid, g.SQL)
		}
	}
	if len(seen) != len(SRIDs) || !seen[4326] {
		t.Fatalf("SRIDs %v", seen)
	}
	a, b := rand.New(rand.NewSource(6)), rand.New(rand.NewSource(6))
	kinds := map[string]bool{}
	for i := 0; i < 100; i++ {
		g := Geography(a)
		if g != Geography(b) || !strings.HasPrefix(g, "'SRID=4326;") || !strings.HasSuffix(g, "'::geography") {
			t.Fatalf("geography %s", g)
		}
		kinds[strings.SplitN(strings.TrimPrefix(g, "'SRID=4326;"), "(", 2)[0]] = true
	}
	if len(kinds) != 3 {
		t.Fatalf("geography kinds %v", kinds)
	}
}

func TestWindowsNeverTouchAQuarter(t *testing.T) {
	r := rand.New(rand.NewSource(9))
	for i := 0; i < 1000; i++ {
		w := Window(r)
		for _, v := range []float64{w.MinX, w.MinY, w.MaxX, w.MaxY} {
			if math.Mod(v*8, 2) != 1 && math.Mod(v*8, 2) != -1 {
				t.Fatalf("window edge %v isn't an odd eighth", v)
			}
		}
		if w.MaxX-w.MinX < 25 || w.MaxY-w.MinY < 25 {
			t.Fatalf("window %v too small", w)
		}
	}
}

func TestOverlaps(t *testing.T) {
	w := Box{0.125, 0.125, 10.125, 10.125}
	for _, c := range []struct {
		g    Geom
		want bool
	}{
		{Point(5, 5), true},
		{Point(10.25, 5), false},
		{rect(10, 10, 1, 1), true},
		{rect(-2, -2, 2, 2), false},
		{quad(12, 5, 2, 1), true},
		{Large(-7.75, 5, 10), true},
		{Large(-8, 5, 10), false},
		{Geom{Kind: "empty", Empty: true}, false},
	} {
		if got := c.g.Overlaps(w); got != c.want {
			t.Errorf("%s (box %v) overlaps %v: %v", c.g.SQL, c.g.Box, w, got)
		}
	}
}

func TestLargeIsBuiltByTheServer(t *testing.T) {
	g := Large(1.5, 2, 100000)
	if len(g.SQL) > 400 || !strings.Contains(g.SQL, "generate_series(100000, 0, -1)") {
		t.Fatalf("large SQL: %s", g.SQL)
	}
	if g.Box != (Box{1.5, 1, 9.5, 2.5}) || g.Empty {
		t.Fatalf("large box %v", g.Box)
	}
}

func TestCodedFollowsIDAndB(t *testing.T) {
	kinds := func(id int) []string {
		var ks []string
		for b := 0; b < 4; b++ {
			ks = append(ks, Coded(id, b, 4, 1000).Kind)
		}
		return ks
	}
	for id, want := range map[int][]string{
		1: {"point", "curve", "invalid", "curve"},
		2: {"point", "refused", "refused", "empty"},
		3: {"point", "line", "polygon", "multipoint"},
		4: {"point", "curve", "invalid", "large"},
		8: {"point", "refused", "refused", "large"},
	} {
		if got := kinds(id); !reflect.DeepEqual(got, want) {
			t.Errorf("id %d: %v, want %v", id, got, want)
		}
	}
	if Coded(3, 1, 4, 1000) != Coded(3, 1, 4, 1000) || Coded(3, 1, 4, 1000) == Coded(3, 5, 4, 1000) ||
		Coded(3, 1, 4, 1000) == Coded(6, 1, 4, 1000) {
		t.Fatal("a row's geometry must be its id's and b's alone")
	}
	if g := Coded(3, -3, 4, 1000); g.Kind != "line" || g.Box.MinY != -30-2.75 {
		t.Fatalf("negative b: %+v", g)
	}
	// every special value among the rows the fuzzer makes, each in its
	// row's region
	all := ""
	for id := 1; id <= 8; id++ {
		for b := 0; b < 4; b++ {
			g := Coded(id, b, 4, 1000)
			all += g.SQL + "\n"
			x, y := float64(10*id), float64(10*b)
			if !g.Empty && (g.Box.MinX < x || g.Box.MaxX > x+8 || g.Box.MinY < y-3 || g.Box.MaxY > y+5) {
				t.Fatalf("row %d b %d: box %v outside its region", id, b, g.Box)
			}
		}
	}
	for _, want := range []string{"POINT ZM (", "MULTIPOINT M (", ", 4326)", "CIRCULARSTRING(", "CURVEPOLYGON(",
		"postgis_addbbox(", "'0102", "'0103", "POLYGON EMPTY", "generate_series"} {
		if !strings.Contains(all, want) {
			t.Fatalf("no %s among the fuzzer's values", want)
		}
	}
	if CodedGeography(2, 3) != CodedGeography(2, 3) || CodedGeography(2, 3) == CodedGeography(2, 4) ||
		!strings.Contains(CodedGeography(2, 3), "LINESTRING") || !strings.Contains(CodedGeography(2, 4), "POINT") {
		t.Fatal("a row's geography must be its id's and b's")
	}
}
