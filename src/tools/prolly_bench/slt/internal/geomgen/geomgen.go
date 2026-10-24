// Package geomgen makes seeded PostGIS values for the differential runner
// and the version-control fuzzer: points, lines, polygons, collections and
// empties, in 2D and with Z, M or both; circular arcs, compound curves and
// curve polygons; invalid polygons; values whose bounding box is cached in
// them (postgis_addbbox); now and then a large polygon whose value is
// stored out of line; values PostGIS's binary receive refuses though its
// text input takes them (Refused); geometries of several SRIDs; and
// geographies.  Each geometry comes as a SQL expression with its bounding
// box, so a caller can predict which rows a window query (&&) returns.
//
// Coordinates are multiples of 1/4 within a few thousand of the origin, so
// they are exact in the float4 boxes PostGIS's && and GiST compare, and
// windows (Window) have edges at odd multiples of 1/8, so no box touches a
// window's edge: whether one overlaps is never a question of rounding.
package geomgen

import (
	"encoding/binary"
	"encoding/hex"
	"fmt"
	"math"
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
	// point, line, polygon, multipoint, collection, empty, large, curve,
	// invalid, refused
	Kind  string
	SQL   string // an expression of type geometry
	Box   Box    // its bounding box (x and y), unless Empty
	Empty bool
	// Refused: binary receive refuses it, and GEOS and distance functions
	// raise errors on it (a one-point line, an unclosed ring); && and
	// functions that only read its points take it
	Refused bool
}

// Overlaps is whether the geometry's box overlaps w, as g && w decides.
func (g Geom) Overlaps(w Box) bool { return !g.Empty && g.Box.Overlaps(w) }

func num(f float64) string { return fmt.Sprintf("%g", f) }

// quarter is a random multiple of 1/4 in [lo, hi).
func quarter(r *rand.Rand, lo, hi int) float64 {
	return float64(lo) + float64(r.Intn((hi-lo)*4))/4
}

// pt is a vertex; z and m are written where the geometry's dims say.
type pt struct{ x, y, z, m float64 }

func boxOf(ps []pt) Box {
	b := Box{ps[0].x, ps[0].y, ps[0].x, ps[0].y}
	for _, p := range ps[1:] {
		b.MinX, b.MaxX = min(b.MinX, p.x), max(b.MaxX, p.x)
		b.MinY, b.MaxY = min(b.MinY, p.y), max(b.MaxY, p.y)
	}
	return b
}

// Dims are a geometry's dimensions beyond x and y: "", "Z", "M" or "ZM".
type Dims string

func coordsIn(d Dims, ps []pt) string {
	parts := make([]string, len(ps))
	for i, p := range ps {
		parts[i] = num(p.x) + " " + num(p.y)
		if d == "Z" || d == "ZM" {
			parts[i] += " " + num(p.z)
		}
		if d == "M" || d == "ZM" {
			parts[i] += " " + num(p.m)
		}
	}
	return strings.Join(parts, ", ")
}

func coords(ps []pt) string { return coordsIn("", ps) }

// tagged is a WKT type name with its dims: "POINT ZM ".
func tagged(name string, d Dims) string {
	if d == "" {
		return name
	}
	return name + " " + string(d) + " "
}

func text(wkt string) string { return "'" + wkt + "'::geometry" }

func shape(kind, name string, d Dims, body string, ps []pt) Geom {
	return Geom{Kind: kind, SQL: text(tagged(name, d) + body), Box: boxOf(ps)}
}

// Point is a point.
func Point(x, y float64) Geom { return pointIn("", pt{x: x, y: y}) }

func pointIn(d Dims, p pt) Geom {
	ps := []pt{p}
	return shape("point", "POINT", d, "("+coordsIn(d, ps)+")", ps)
}

// line is a line through ps (at least two).
func line(ps []pt) Geom { return lineIn("", ps) }

func lineIn(d Dims, ps []pt) Geom { return shape("line", "LINESTRING", d, "("+coordsIn(d, ps)+")", ps) }

// quad is the convex quadrilateral with corners (x-w, y), (x, y-h),
// (x+w, y), (x, y+h): a rhombus, or a rectangle turned by 45 degrees.
func quad(x, y, w, h float64) Geom { return quadIn("", x, y, w, h, 0) }

func quadIn(d Dims, x, y, w, h, z float64) Geom {
	ps := []pt{{x - w, y, z, 1}, {x, y - h, z, 2}, {x + w, y, z, 3}, {x, y + h, z, 4}, {x - w, y, z, 1}}
	return shape("polygon", "POLYGON", d, "(("+coordsIn(d, ps)+"))", ps)
}

// rect is an axis-aligned rectangle, a building's footprint.
func rect(x, y, w, h float64) Geom {
	ps := []pt{{x: x, y: y}, {x: x + w, y: y}, {x: x + w, y: y + h}, {x: x, y: y + h}, {x: x, y: y}}
	return shape("polygon", "POLYGON", "", "(("+coords(ps)+"))", ps)
}

// Arc is the half circle from (x, y) over (x+r, y+r) to (x+2r, y): its box
// is [x, x+2r] by [y, y+r], the arc's, not only its control points'.
func Arc(x, y, r float64) Geom {
	ps := []pt{{x: x, y: y}, {x: x + r, y: y + r}, {x: x + 2*r, y: y}}
	return shape("curve", "CIRCULARSTRING", "", "("+coords(ps)+")", ps)
}

// circle is the curve polygon bounded by the circle through (x, y) and
// (x+2r, y): its box is [x, x+2r] by [y-r, y+r].
func circle(x, y, r float64) Geom {
	ps := []pt{{x: x, y: y}, {x: x + 2*r, y: y}, {x: x, y: y}}
	g := shape("curve", "CURVEPOLYGON", "", "(CIRCULARSTRING("+coords(ps)+"))", ps)
	g.Box = Box{x, y - r, x + 2*r, y + r}
	return g
}

// compound is a half circle (Arc) and then a straight run of w to the
// right.
func compound(x, y, r, w float64) Geom {
	arc := []pt{{x: x, y: y}, {x: x + r, y: y + r}, {x: x + 2*r, y: y}}
	run := []pt{{x: x + 2*r, y: y}, {x: x + 2*r + w, y: y}}
	g := shape("curve", "COMPOUNDCURVE", "", "(CIRCULARSTRING("+coords(arc)+"), ("+coords(run)+"))", append(arc, run...))
	return g
}

// bowtie is a self-intersecting polygon: invalid, which PostGIS stores
// and reads as it is.
func bowtie(x, y, w, h float64) Geom {
	ps := []pt{{x: x, y: y}, {x: x + w, y: y + h}, {x: x + w, y: y}, {x: x, y: y + h}, {x: x, y: y}}
	g := shape("invalid", "POLYGON", "", "(("+coords(ps)+"))", ps)
	return g
}

// cached is g with its bounding box cached in the value, as
// postgis_addbbox() makes it (a binary send doesn't hold the cache).
func cached(g Geom) Geom {
	g.SQL = "postgis_addbbox(" + g.SQL + ")"
	return g
}

// ewkb is a little-endian WKB value of type typ, as a hex literal.
func ewkb(typ uint32, body []byte) string {
	b := []byte{1}
	b = binary.LittleEndian.AppendUint32(b, typ)
	return "'" + hex.EncodeToString(append(b, body...)) + "'::geometry"
}

func wkbPoints(ps []pt) []byte {
	b := binary.LittleEndian.AppendUint32(nil, uint32(len(ps)))
	for _, p := range ps {
		b = binary.LittleEndian.AppendUint64(b, math.Float64bits(p.x))
		b = binary.LittleEndian.AppendUint64(b, math.Float64bits(p.y))
	}
	return b
}

// onePointLine is a LINESTRING of one point, as hex EWKB: the text input
// takes it, the binary receive refuses it.
func onePointLine(x, y float64) Geom {
	ps := []pt{{x: x, y: y}}
	return Geom{Kind: "refused", SQL: ewkb(2, wkbPoints(ps)), Box: boxOf(ps), Refused: true}
}

// unclosedRing is a polygon whose ring doesn't end where it starts, as hex
// EWKB: the text input takes it, the binary receive refuses it.
func unclosedRing(x, y, w, h float64) Geom {
	ps := []pt{{x: x, y: y}, {x: x + w, y: y}, {x: x + w, y: y + h}, {x: x, y: y + h}}
	body := binary.LittleEndian.AppendUint32(nil, 1)
	return Geom{Kind: "refused", SQL: ewkb(3, append(body, wkbPoints(ps)...)), Box: boxOf(ps), Refused: true}
}

// Refused is a seeded value PostGIS's binary receive refuses though its
// text input takes it: a one-point line or a ring left open.  A value
// table holds one is kept as its text form (in prolly).
func Refused(r *rand.Rand) Geom {
	x, y := quarter(r, 0, Extent), quarter(r, 0, Extent)
	if r.Intn(2) == 0 {
		return onePointLine(x, y)
	}
	return unclosedRing(x, y, quarter(r, 1, 10), quarter(r, 1, 10))
}

// SRIDs are those WithSRID's geometries take, all in spatial_ref_sys.
var SRIDs = []int{0, 4326, 3857, 2154}

// WithSRID is g in a seeded SRID of SRIDs.
func WithSRID(r *rand.Rand, g Geom) (Geom, int) {
	srid := SRIDs[r.Intn(len(SRIDs))]
	if srid != 0 {
		g.SQL = fmt.Sprintf("ST_SetSRID(%s, %d)", g.SQL, srid)
	}
	return g, srid
}

// Geography is a seeded geography in SRID 4326: a point, a line or a
// small polygon, at quarter degrees within 80 degrees of the equator.
func Geography(r *rand.Rand) string {
	lon, lat := quarter(r, -180, 175), quarter(r, -80, 75)
	var wkt string
	switch r.Intn(3) {
	case 0:
		wkt = fmt.Sprintf("POINT(%s %s)", num(lon), num(lat))
	case 1:
		wkt = fmt.Sprintf("LINESTRING(%s %s, %s %s)", num(lon), num(lat), num(lon+quarter(r, 1, 5)), num(lat+quarter(r, -4, 4)))
	default:
		w, h := quarter(r, 1, 4), quarter(r, 1, 4)
		wkt = fmt.Sprintf("POLYGON((%[1]s %[2]s, %[3]s %[2]s, %[3]s %[4]s, %[1]s %[4]s, %[1]s %[2]s))",
			num(lon), num(lat), num(lon+w), num(lat+h))
	}
	return "'SRID=4326;" + wkt + "'::geography"
}

// dimsOf is a seeded choice of dims, 2D most often.
func dimsOf(r *rand.Rand) Dims {
	return []Dims{"", "", "", "Z", "M", "ZM"}[r.Intn(6)]
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

// Random is a seeded geometry: mostly points, lines and polygons (in 2D,
// or with Z, M or both), some collections, curves, invalid polygons and
// empties, a cached box now and then, and with largeEvery > 0 a large
// polygon (of largeN to twice largeN vertices) one time in largeEvery.
// Its SRID is 0; it is never Refused.
func Random(r *rand.Rand, largeEvery, largeN int) Geom {
	x, y := quarter(r, 0, Extent), quarter(r, 0, Extent)
	if largeEvery > 0 && r.Intn(largeEvery) == 0 {
		return Large(x, y, largeN+r.Intn(largeN+1))
	}
	z, m := quarter(r, -50, 50), quarter(r, 0, 100)
	var g Geom
	switch k := r.Intn(24); {
	case k < 6:
		g = pointIn(dimsOf(r), pt{x, y, z, m})
	case k < 10:
		// a random walk of 2 to 12 vertices
		ps := []pt{{x, y, z, m}}
		for n := 1 + r.Intn(11); n > 0; n-- {
			last := ps[len(ps)-1]
			ps = append(ps, pt{last.x + quarter(r, -5, 5), last.y + quarter(r, -5, 5), last.z + 1, last.m + 1})
		}
		if ps[1].x == ps[0].x && ps[1].y == ps[0].y {
			ps[1].x += 0.25
		}
		g = lineIn(dimsOf(r), ps)
	case k < 13:
		g = rect(x, y, quarter(r, 1, 20), quarter(r, 1, 20))
	case k < 16:
		g = quadIn(dimsOf(r), x, y, quarter(r, 1, 10), quarter(r, 1, 10), z)
	case k < 17:
		ps := []pt{{x: x, y: y}, {x: x + quarter(r, 0, 9), y: y + quarter(r, 0, 9)}, {x: x - quarter(r, 0, 9), y: y + quarter(r, 0, 9)}}
		g = shape("multipoint", "MULTIPOINT", "", "("+coords(ps)+")", ps)
	case k < 18:
		p, q := Point(x, y), rect(x+1, y+1, quarter(r, 1, 5), quarter(r, 1, 5))
		b := p.Box
		b.MaxX, b.MaxY = max(b.MaxX, q.Box.MaxX), max(b.MaxY, q.Box.MaxY)
		g = Geom{Kind: "collection", SQL: "ST_Collect(" + p.SQL + ", " + q.SQL + ")", Box: b}
	case k < 21:
		rad := quarter(r, 1, 6)
		g = []Geom{Arc(x, y, rad), circle(x, y, rad), compound(x, y, rad, quarter(r, 1, 6))}[r.Intn(3)]
	case k < 22:
		g = bowtie(x, y, quarter(r, 1, 10), quarter(r, 1, 10))
	default:
		return Geom{Kind: "empty", SQL: text(empties[r.Intn(len(empties))]), Empty: true}
	}
	if r.Intn(8) == 0 {
		g = cached(g)
	}
	return g
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
// does and merges keep the two together.  Rows sit at x = 10 id, y = 10 b,
// each within 8 to the right, 3 below and 5 above.  The kinds go by b mod
// 4 and the id:
//
//	b mod 4 = 0: a point: with Z and M (id mod 3 = 0), 2D (1), or in SRID
//	             4326 (2)
//	1: a line (0), a half circle (1), or a one-point line (2, Refused)
//	2: a rhombus, its box cached for an even id (0), a bowtie (1, invalid),
//	   or a ring left open (2, Refused)
//	3: a large polygon of largeN vertices for an id that is a multiple of
//	   largeEvery, empty for another even id, a curve polygon for id mod 4
//	   = 1, and two points with M otherwise
func Coded(id, b, largeEvery, largeN int) Geom {
	x, y := float64(10*id), float64(10*b)
	third := ((id % 3) + 3) % 3
	switch ((b % 4) + 4) % 4 {
	case 0:
		switch third {
		case 0:
			return pointIn("ZM", pt{x + 0.25, y + 0.5, float64(id), float64(b)})
		case 1:
			return Point(x+0.25, y+0.5)
		}
		g := Point(x+0.25, y+0.5)
		g.SQL = "ST_SetSRID(" + g.SQL + ", 4326)"
		return g
	case 1:
		switch third {
		case 0:
			return line([]pt{{x: x, y: y}, {x: x + 3.5, y: y + 1.25}, {x: x + 6, y: y - 2.75}})
		case 1:
			return Arc(x, y, 2)
		}
		return onePointLine(x+1, y+1)
	case 2:
		switch third {
		case 0:
			if id%2 == 0 {
				return cached(quad(x+2, y+2, 2, 2))
			}
			return quad(x+2, y+2, 2, 2)
		case 1:
			return bowtie(x, y, 4, 4)
		}
		return unclosedRing(x, y, 4, 4)
	}
	switch {
	case id%largeEvery == 0:
		return Large(x, y, largeN)
	case id%2 == 0:
		return Geom{Kind: "empty", SQL: text("POLYGON EMPTY"), Empty: true}
	case ((id%4)+4)%4 == 1:
		return circle(x, y, 2)
	}
	ps := []pt{{x: x, y: y, m: 1}, {x: x + 5, y: y + 5, m: 2}}
	return shape("multipoint", "MULTIPOINT", "M", "("+coordsIn("M", ps)+")", ps)
}

// CodedGeography is the geography the fuzzer's table t holds for a row,
// like Coded: a point for an even b, a short line for an odd one.
func CodedGeography(id, b int) string {
	lon, lat := float64(10*id)-90, float64(((b%60)+60)%60)-30
	if ((b%2)+2)%2 == 0 {
		return fmt.Sprintf("'SRID=4326;POINT(%s %s)'::geography", num(lon), num(lat))
	}
	return fmt.Sprintf("'SRID=4326;LINESTRING(%s %s, %s %s)'::geography", num(lon), num(lat), num(lon+2.5), num(lat+1.25))
}
