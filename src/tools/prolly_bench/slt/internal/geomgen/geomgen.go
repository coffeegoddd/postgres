// Package geomgen makes seeded PostGIS geometries for the differential
// runner and the version-control fuzzer: points, lines, polygons,
// collections and empties, and now and then a large polygon whose value
// is stored out of line.  Each comes as a SQL expression with its bounding
// box, so a caller can predict which rows a window query (&&) returns.
//
// Coordinates are multiples of 1/4 within a few thousand of the origin, so
// they are exact in the float4 boxes PostGIS's && and GiST compare, and
// windows (Window) have edges at odd multiples of 1/8, so no box touches a
// window's edge: whether one overlaps is never a question of rounding.
package geomgen

import (
	"fmt"
	"math/rand"
	"strings"
)

// Box is a bounding box.  An empty geometry has none.
type Box struct {
	MinX, MinY, MaxX, MaxY float64
}

// Overlaps is whether two boxes share a point, as && decides for boxes.
func (b Box) Overlaps(o Box) bool {
	return b.MinX <= o.MaxX && o.MinX <= b.MaxX && b.MinY <= o.MaxY && o.MinY <= b.MaxY
}

// SQL is the box as ST_MakeEnvelope's call.
func (b Box) SQL() string {
	return fmt.Sprintf("ST_MakeEnvelope(%s, %s, %s, %s)", num(b.MinX), num(b.MinY), num(b.MaxX), num(b.MaxY))
}

// Geom is a generated geometry.
type Geom struct {
	Kind  string // point, line, polygon, multipoint, collection, empty, large
	SQL   string // an expression of type geometry
	Box   Box    // its bounding box, unless Empty
	Empty bool
}

// Overlaps is whether the geometry's box overlaps w, as g && w decides.
func (g Geom) Overlaps(w Box) bool { return !g.Empty && g.Box.Overlaps(w) }

func num(f float64) string { return fmt.Sprintf("%g", f) }

// quarter is a random multiple of 1/4 in [lo, hi).
func quarter(r *rand.Rand, lo, hi int) float64 {
	return float64(lo) + float64(r.Intn((hi-lo)*4))/4
}

type pt struct{ x, y float64 }

func boxOf(ps []pt) Box {
	b := Box{ps[0].x, ps[0].y, ps[0].x, ps[0].y}
	for _, p := range ps[1:] {
		b.MinX, b.MaxX = min(b.MinX, p.x), max(b.MaxX, p.x)
		b.MinY, b.MaxY = min(b.MinY, p.y), max(b.MaxY, p.y)
	}
	return b
}

func coords(ps []pt) string {
	parts := make([]string, len(ps))
	for i, p := range ps {
		parts[i] = num(p.x) + " " + num(p.y)
	}
	return strings.Join(parts, ", ")
}

func text(wkt string) string { return "'" + wkt + "'::geometry" }

// Point is a point.
func Point(x, y float64) Geom {
	ps := []pt{{x, y}}
	return Geom{Kind: "point", SQL: text("POINT(" + coords(ps) + ")"), Box: boxOf(ps)}
}

// Line is a line through ps (at least two).
func line(ps []pt) Geom {
	return Geom{Kind: "line", SQL: text("LINESTRING(" + coords(ps) + ")"), Box: boxOf(ps)}
}

// Quad is the convex quadrilateral with corners (x-w, y), (x, y-h),
// (x+w, y), (x, y+h): a rhombus, or a rectangle turned by 45 degrees.
func quad(x, y, w, h float64) Geom {
	ps := []pt{{x - w, y}, {x, y - h}, {x + w, y}, {x, y + h}, {x - w, y}}
	return Geom{Kind: "polygon", SQL: text("POLYGON((" + coords(ps) + "))"), Box: boxOf(ps)}
}

// rect is an axis-aligned rectangle, a building's footprint.
func rect(x, y, w, h float64) Geom {
	ps := []pt{{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}, {x, y}}
	return Geom{Kind: "polygon", SQL: text("POLYGON((" + coords(ps) + "))"), Box: boxOf(ps)}
}

// Large is a polygon of n+3 vertices over [x, x+8] by [y-1, y+0.5]: a flat
// bottom and a top that zigzags between y and y+0.5.  At 16 bytes a
// vertex, n of 100,000 makes a value of about 1.6 MB.  It is built by the
// server (generate_series), so a statement making one stays short.  Only
// its corners' coordinates need be exact: its box is theirs.
func Large(x, y float64, n int) Geom {
	sql := fmt.Sprintf("(SELECT ST_MakePolygon(ST_MakeLine(ARRAY["+
		"ST_MakePoint(%[1]s, %[3]s), ST_MakePoint(%[1]s + 8, %[3]s)] || "+
		"ARRAY(SELECT ST_MakePoint(%[1]s + 8.0 * k / %[4]d, %[2]s + (k %% 2) * 0.5) "+
		"FROM generate_series(%[4]d, 0, -1) k ORDER BY k DESC) || ST_MakePoint(%[1]s, %[3]s))))",
		num(x), num(y), num(y-1), n)
	return Geom{Kind: "large", SQL: sql, Box: Box{x, y - 1, x + 8, y + 0.5}}
}

var empties = []string{"POINT EMPTY", "LINESTRING EMPTY", "POLYGON EMPTY", "GEOMETRYCOLLECTION EMPTY"}

// Extent is the region Random's geometries start in: [0, Extent) on both
// axes (a shape reaches a few units past it).
const Extent = 1000

// Random is a seeded geometry: mostly points, lines and polygons, some
// collections and empties, and with largeEvery > 0 a large polygon (of
// largeN to twice largeN vertices) one time in largeEvery.
func Random(r *rand.Rand, largeEvery, largeN int) Geom {
	x, y := quarter(r, 0, Extent), quarter(r, 0, Extent)
	if largeEvery > 0 && r.Intn(largeEvery) == 0 {
		return Large(x, y, largeN+r.Intn(largeN+1))
	}
	switch k := r.Intn(20); {
	case k < 6:
		return Point(x, y)
	case k < 10:
		// a random walk of 2 to 12 vertices
		ps := []pt{{x, y}}
		for n := 1 + r.Intn(11); n > 0; n-- {
			last := ps[len(ps)-1]
			ps = append(ps, pt{last.x + quarter(r, -5, 5), last.y + quarter(r, -5, 5)})
		}
		if ps[1] == ps[0] {
			ps[1].x += 0.25
		}
		return line(ps)
	case k < 13:
		return rect(x, y, quarter(r, 1, 20), quarter(r, 1, 20))
	case k < 16:
		return quad(x, y, quarter(r, 1, 10), quarter(r, 1, 10))
	case k < 17:
		ps := []pt{{x, y}, {x + quarter(r, 0, 9), y + quarter(r, 0, 9)}, {x - quarter(r, 0, 9), y + quarter(r, 0, 9)}}
		return Geom{Kind: "multipoint", SQL: text("MULTIPOINT(" + coords(ps) + ")"), Box: boxOf(ps)}
	case k < 18:
		p, q := Point(x, y), rect(x+1, y+1, quarter(r, 1, 5), quarter(r, 1, 5))
		b := p.Box
		b.MaxX, b.MaxY = max(b.MaxX, q.Box.MaxX), max(b.MaxY, q.Box.MaxY)
		return Geom{Kind: "collection", SQL: "ST_Collect(" + p.SQL + ", " + q.SQL + ")", Box: b}
	}
	return Geom{Kind: "empty", SQL: text(empties[r.Intn(len(empties))]), Empty: true}
}

// Window is a seeded window over Random's region, from 1/40 of it across
// to 1/4, with edges at odd multiples of 1/8.
func Window(r *rand.Rand) Box {
	eighth := func(lo, hi int) float64 { return quarter(r, lo, hi) + 0.125 }
	x, y := eighth(-10, Extent), eighth(-10, Extent)
	return Box{x, y, x + float64(25+r.Intn(226)), y + float64(25+r.Intn(226))}
}

// Coded is the geometry the fuzzer's table t holds for a row: one of its
// row id and its column b, so a row's geometry changes exactly when b
// does and merges keep the two together.  Kinds go by b mod 4: a point, a
// line, a rhombus, and a fourth that is a large polygon of largeN vertices
// for an id that is a multiple of largeEvery, empty for another even id,
// and two points otherwise.  Rows sit at x = 10 id, y = 10 b.
func Coded(id, b, largeEvery, largeN int) Geom {
	x, y := float64(10*id), float64(10*b)
	switch ((b % 4) + 4) % 4 {
	case 0:
		return Point(x+0.25, y+0.5)
	case 1:
		return line([]pt{{x, y}, {x + 3.5, y + 1.25}, {x + 6, y - 2.75}})
	case 2:
		return quad(x+2, y+2, 2, 2)
	}
	switch {
	case id%largeEvery == 0:
		return Large(x, y, largeN)
	case id%2 == 0:
		return Geom{Kind: "empty", SQL: text("POLYGON EMPTY"), Empty: true}
	}
	ps := []pt{{x, y}, {x + 5, y + 5}}
	return Geom{Kind: "multipoint", SQL: text("MULTIPOINT(" + coords(ps) + ")"), Box: boxOf(ps)}
}
