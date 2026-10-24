package vcfuzz

import (
	"sort"

	"prolly_bench/slt/internal/geomgen"
)

// largeN is the vertices of the large polygon rows 4 and 8 of t hold
// while their b is 3 (mod 4): about 1.6 MB, a value kept out of line.
const largeN = 100000

// Geom is the geometry table t's row id holds in column g for its b.
func Geom(id, b int) geomgen.Geom { return geomgen.Coded(id, b, 4, largeN) }

// Geog is the geography table t's row id holds in column gg for its b.
func Geog(id, b int) string { return geomgen.CodedGeography(id, b) }

// Windows are the windows a spatial check reads t's geometries through.
// Rows sit at x = 10 id (10 to 80, each up to 8 wide) and y = 10 b (0 to
// 30, each from 3 below to 5 above); the windows cut across both, and the
// last holds row 8 whatever its b (its large polygon too).
var Windows = []geomgen.Box{
	{MinX: 9.125, MinY: -5.125, MaxX: 41.875, MaxY: 12.125},
	{MinX: 38.125, MinY: 8.125, MaxX: 66.875, MaxY: 24.875},
	{MinX: 0.125, MinY: 18.125, MaxX: 90.875, MaxY: 35.875},
	{MinX: 78.125, MinY: -5.125, MaxX: 90.875, MaxY: 40.875},
}

// WindowIDs is, for each of Windows, the ids of the rows of t in s whose
// geometries overlap it, in order: what an engine's Windows should give.
func WindowIDs(s State) [][]int {
	ids := make([]int, 0, len(s["t"]))
	for id := range s["t"] {
		ids = append(ids, id)
	}
	sort.Ints(ids)
	out := make([][]int, len(Windows))
	for i, w := range Windows {
		out[i] = []int{}
		for _, id := range ids {
			if Geom(id, s["t"][id][1]).Overlaps(w) {
				out[i] = append(out[i], id)
			}
		}
	}
	return out
}
