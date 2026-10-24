package enginediff

import (
	"encoding/json"
	"reflect"
	"strings"
	"testing"
)

func TestGenerateReplayAndCoverage(t *testing.T) {
	p := Generate(42, 36)
	b, err := json.Marshal(p)
	if err != nil {
		t.Fatal(err)
	}
	var replay Program
	if err := json.Unmarshal(b, &replay); err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(p, replay) || !reflect.DeepEqual(p, Generate(42, 36)) {
		t.Fatal("seed and serialized replay must reproduce the same program")
	}
	if reflect.DeepEqual(p, Generate(43, 36)) {
		t.Fatal("seeds did not change the workload")
	}
	seen := map[string]int{}
	for _, g := range p.Groups {
		seen[g.Name]++
	}
	if len(seen) != 18 {
		t.Fatalf("missing workload families: %v", seen)
	}
	for _, n := range seen {
		if n != 2 {
			t.Fatalf("unbalanced families: %v", seen)
		}
	}
}

func TestReduceKeepsFailureAndSetup(t *testing.T) {
	p := Generate(7, 28)
	calls := 0
	r := Reduce(p, 48, func(trial Program) bool {
		calls++
		for _, g := range trial.Groups {
			if g.Name == "savepoints" {
				return true
			}
		}
		return false
	})
	if calls > 48 || len(r.Groups) != 1 || r.Groups[0].Name != "savepoints" || !reflect.DeepEqual(r.Setup, p.Setup) {
		t.Fatalf("bad reduction: calls=%d, program=%+v", calls, r)
	}
	r = Reduce(p, 1, func(Program) bool { return false })
	if !reflect.DeepEqual(r, p) {
		t.Fatal("a non-reproducing reduction was retained")
	}
}

func TestNullAndEmptyDiffer(t *testing.T) {
	null, _ := json.Marshal([][][]byte{{nil}})
	empty, _ := json.Marshal([][][]byte{{[]byte{}}})
	if string(null) == string(empty) {
		t.Fatal("NULL and empty bytes compare equal")
	}
}

func TestPostGISOptionAddsSpatialFamilies(t *testing.T) {
	if !reflect.DeepEqual(Generate(42, 36), GenerateWith(42, 36, Options{})) {
		t.Fatal("without options a program must be Generate's")
	}
	p := GenerateWith(42, 48, Options{PostGIS: true})
	b, err := json.Marshal(p)
	if err != nil {
		t.Fatal(err)
	}
	var replay Program
	if err := json.Unmarshal(b, &replay); err != nil {
		t.Fatal(err)
	}
	if !p.PostGIS || !reflect.DeepEqual(p, replay) || !reflect.DeepEqual(p, GenerateWith(42, 48, Options{PostGIS: true})) {
		t.Fatal("a spatial program must replay and reproduce")
	}
	if reflect.DeepEqual(p, GenerateWith(43, 48, Options{PostGIS: true})) {
		t.Fatal("seeds did not change the spatial workload")
	}
	setup := strings.Join(p.Setup, "\n")
	for _, want := range []string{"CREATE EXTENSION postgis", "CREATE TABLE geo", "USING gist (g)", "generate_series(100000",
		"gg geography", "USING gist (gg)", "USING gist (r)", "'SRID=4326;", "ST_SetSRID(", " ZM (", "CIRCULARSTRING(",
		"postgis_addbbox(", "'0102", "'0103"} {
		if !strings.Contains(setup, want) {
			t.Fatalf("setup lacks %q", want)
		}
	}
	seen := map[string]int{}
	for _, g := range p.Groups {
		seen[g.Name]++
	}
	if len(seen) != 24 {
		t.Fatalf("missing families: %v", seen)
	}
	for _, f := range []string{"geo_writes", "geo_reads", "geo_index_reads", "geo_knn", "geo_savepoints", "geo_large"} {
		if seen[f] != 2 {
			t.Fatalf("family %s ran %d times: %v", f, seen[f], seen)
		}
	}
	// spatial predicates in the reads, and large values among the writes
	// over enough seeds
	reads, large := "", false
	for seed := int64(1); seed <= 20; seed++ {
		for _, g := range GenerateWith(seed, 24, Options{PostGIS: true}).Groups {
			switch g.Name {
			case "geo_reads", "geo_index_reads", "geo_knn":
				reads += strings.Join(g.SQL, "\n")
			case "geo_writes":
				large = large || strings.Contains(g.SQL[0], "generate_series")
			}
		}
	}
	for _, want := range []string{"&&", "ST_Intersects", "ST_Contains", "ST_DWithin", "<->", "enable_seqscan = off",
		"ST_SetSRID(ST_MakeEnvelope", "ST_DWithin(gg", "ST_Area(gg)", "r && "} {
		if !strings.Contains(reads, want) {
			t.Fatalf("reads lack %s", want)
		}
	}
	if !large {
		t.Fatal("no large polygon written in 20 seeds")
	}
	if Generate(1, 1).PostGIS || strings.Contains(strings.Join(Generate(1, 36).Setup, ""), "postgis") {
		t.Fatal("a program without the option touches PostGIS")
	}
}
