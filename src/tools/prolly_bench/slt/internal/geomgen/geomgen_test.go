package geomgen

import (
	"math"
	"math/rand"
	"reflect"
	"regexp"
	"strconv"
	"strings"
	"testing"
)

var number = regexp.MustCompile(`-?[0-9]+(\.[0-9]+)?`)

// coordinates is the numbers in a WKT literal, by axis.
func coordinates(t *testing.T, sql string) (xs, ys []float64) {
	t.Helper()
	wkt := strings.TrimSuffix(strings.TrimPrefix(sql, "'"), "'::geometry")
	for i, s := range number.FindAllString(wkt, -1) {
		f, err := strconv.ParseFloat(s, 64)
		if err != nil {
			t.Fatal(err)
		}
		if i%2 == 0 {
			xs = append(xs, f)
		} else {
			ys = append(ys, f)
		}
	}
	return xs, ys
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
	for _, k := range []string{"point", "line", "polygon", "multipoint", "collection", "empty", "large"} {
		if kinds[k] == 0 {
			t.Fatalf("no %s in %v", k, kinds)
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
// edges; an empty has none, and overlaps nothing.
func TestBoxesAreExactAndHoldTheCoordinates(t *testing.T) {
	r := rand.New(rand.NewSource(7))
	for i := 0; i < 3000; i++ {
		g := Random(r, 0, 0)
		if g.Kind == "collection" {
			continue
		}
		xs, ys := coordinates(t, g.SQL)
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
		1: {"point", "line", "polygon", "multipoint"},
		2: {"point", "line", "polygon", "empty"},
		4: {"point", "line", "polygon", "large"},
		8: {"point", "line", "polygon", "large"},
	} {
		if got := kinds(id); !reflect.DeepEqual(got, want) {
			t.Errorf("id %d: %v, want %v", id, got, want)
		}
	}
	if Coded(3, 1, 4, 1000) != Coded(3, 1, 4, 1000) || Coded(3, 1, 4, 1000) == Coded(3, 5, 4, 1000) ||
		Coded(3, 1, 4, 1000) == Coded(4, 1, 4, 1000) {
		t.Fatal("a row's geometry must be its id's and b's alone")
	}
	if g := Coded(1, -3, 4, 1000); g.Kind != "line" || g.Box.MinY != -30-2.75 {
		t.Fatalf("negative b: %+v", g)
	}
}
