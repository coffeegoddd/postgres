package doltport

import (
	"fmt"
	"regexp"
	"sort"
	"strings"

	"prolly_bench/slt/internal/doltcatalog"
)

// Class is a catalog case's portability:
//
//	P: version control prolly supports; ported, or to be.
//	F: needs a named feature prolly lacks; ported with that feature.
//	N: doesn't apply (MySQL-only, the staging area, Dolt internals, not
//	   version control).
type Class struct {
	Suite   string `json:"suite"`
	Index   int    `json:"index"`
	SHA256  string `json:"sha256"`
	Class   string `json:"class"`
	Area    string `json:"area"`
	Feature string `json:"feature,omitempty"`
	Reason  string `json:"reason"`
}
type Classification struct {
	Version  int     `json:"version"`
	Revision string  `json:"revision"`
	Cases    []Class `json:"cases"`
}

// AreaCoverage is how far the portable cases of an area are ported:
// partial ports and reviewed deferrals are drafted.
type AreaCoverage struct {
	Area       string `json:"area"`
	Portable   int    `json:"portable"`
	Ported     int    `json:"ported"`
	Partial    int    `json:"partial"`
	Deferred   int    `json:"deferred"`
	NotStarted int    `json:"not_started"`
}

// FeatureCoverage is the cases waiting on a feature, by the area they're
// in, with those partially ported and those deferred.
type FeatureCoverage struct {
	Feature         string         `json:"feature"`
	Cases           int            `json:"cases"`
	PartiallyPorted int            `json:"partially_ported"`
	Deferred        int            `json:"deferred"`
	Areas           map[string]int `json:"areas"`
}

type ClassCoverage struct {
	Portable      int `json:"portable"`
	FeatureGated  int `json:"feature_gated"`
	NotApplicable int `json:"not_applicable"`
	// deferrals of N cases (the manifest records why they don't apply)
	DeferredNotApplicable int               `json:"deferred_not_applicable"`
	Areas                 []AreaCoverage    `json:"areas"`
	Features              []FeatureCoverage `json:"features"`
}

// Areas and features are slugs, so the coverage tables render as tables.
var slug = regexp.MustCompile(`^[a-z][a-z0-9-]*$`)

func classKey(suite string, index int) string { return fmt.Sprintf("%s[%d]", suite, index) }

// checkClassification checks the classification against the lock (and the
// catalog, if given: every case classified once, by its literal's hash),
// and that ports and deferrals agree with it: a full port is of a P case, a
// partial one of a P or F case.
func checkClassification(c Classification, lock doltcatalog.Lock, catalog map[string]doltcatalog.Case, m Manifest, partial map[string]bool) (ClassCoverage, error) {
	var cov ClassCoverage
	if c.Version != 1 || c.Revision != lock.Revision {
		return cov, fmt.Errorf("classification is for another revision")
	}
	byKey := map[string]Class{}
	for _, k := range c.Cases {
		key := classKey(k.Suite, k.Index)
		if _, dup := byKey[key]; dup {
			return cov, fmt.Errorf("case %s classified twice", key)
		}
		if strings.TrimSpace(k.Reason) == "" || !slug.MatchString(k.Area) ||
			(k.Feature != "" && !slug.MatchString(k.Feature)) {
			return cov, fmt.Errorf("case %s needs an area and a reason (area and feature lower-case slugs)", key)
		}
		switch k.Class {
		case "P", "N":
			if k.Feature != "" {
				return cov, fmt.Errorf("case %s: only F cases name a feature", key)
			}
		case "F":
			if k.Feature == "" {
				return cov, fmt.Errorf("case %s: an F case names the feature it needs", key)
			}
		default:
			return cov, fmt.Errorf("case %s: unknown class %q", key, k.Class)
		}
		byKey[key] = k
	}
	if len(byKey) != lock.Counts.Cases {
		return cov, fmt.Errorf("classification has %d cases, the source %d", len(byKey), lock.Counts.Cases)
	}
	if catalog != nil {
		suites := map[string]string{}
		for _, cc := range catalog {
			if p, ok := suites[cc.Suite]; ok && p != cc.Path {
				return cov, fmt.Errorf("suite %s is in two files", cc.Suite)
			}
			suites[cc.Suite] = cc.Path
			k, ok := byKey[classKey(cc.Suite, cc.Index)]
			if !ok || k.SHA256 != cc.SHA256 {
				return cov, fmt.Errorf("case %s unclassified or changed since classified", classKey(cc.Suite, cc.Index))
			}
		}
	}
	ported, deferred, partialKey := map[string]bool{}, map[string]bool{}, map[string]bool{}
	for _, p := range m.Ports {
		key := classKey(p.Suite, p.Index)
		k, ok := byKey[key]
		if !ok || k.SHA256 != p.SourceSHA256 {
			return cov, fmt.Errorf("port %s: case unclassified or classified from other source", p.ID)
		}
		if k.Class != "P" && !(k.Class == "F" && partial[p.ID]) {
			return cov, fmt.Errorf("port %s: a full port must be of a P case, a partial one of P or F (is %s)", p.ID, k.Class)
		}
		ported[key] = true
		if partial[p.ID] {
			partialKey[key] = true
		}
	}
	for _, d := range m.Deferred {
		key := classKey(d.Suite, d.Index)
		if k, ok := byKey[key]; !ok || k.SHA256 != d.SourceSHA256 {
			return cov, fmt.Errorf("deferred %s: case unclassified or classified from other source", key)
		}
		deferred[key] = true
	}
	areas, features := map[string]*AreaCoverage{}, map[string]*FeatureCoverage{}
	for key, k := range byKey {
		switch k.Class {
		case "N":
			cov.NotApplicable++
			if deferred[key] {
				cov.DeferredNotApplicable++
			}
		case "F":
			cov.FeatureGated++
			f := features[k.Feature]
			if f == nil {
				f = &FeatureCoverage{Feature: k.Feature, Areas: map[string]int{}}
				features[k.Feature] = f
			}
			f.Cases++
			f.Areas[k.Area]++
			if partialKey[key] {
				f.PartiallyPorted++
			}
			if deferred[key] {
				f.Deferred++
			}
		case "P":
			cov.Portable++
			a := areas[k.Area]
			if a == nil {
				a = &AreaCoverage{Area: k.Area}
				areas[k.Area] = a
			}
			a.Portable++
			switch {
			case partialKey[key]:
				a.Partial++
			case ported[key]:
				a.Ported++
			case deferred[key]:
				a.Deferred++
			default:
				a.NotStarted++
			}
		}
	}
	for _, a := range areas {
		cov.Areas = append(cov.Areas, *a)
	}
	sort.Slice(cov.Areas, func(i, j int) bool { return cov.Areas[i].Area < cov.Areas[j].Area })
	for _, f := range features {
		cov.Features = append(cov.Features, *f)
	}
	sort.Slice(cov.Features, func(i, j int) bool {
		if cov.Features[i].Cases != cov.Features[j].Cases {
			return cov.Features[i].Cases > cov.Features[j].Cases
		}
		return cov.Features[i].Feature < cov.Features[j].Feature
	})
	return cov, nil
}

// Markdown renders the coverage tables.
func (c ClassCoverage) Markdown() string {
	var b strings.Builder
	fmt.Fprintf(&b, "%d portable (P), %d feature-gated (F), %d not applicable (N).\n\n", c.Portable, c.FeatureGated, c.NotApplicable)
	b.WriteString("| Area | P cases | Ported | Partial | Deferred | Not started |\n| --- | ---: | ---: | ---: | ---: | ---: |\n")
	var t AreaCoverage
	for _, a := range c.Areas {
		fmt.Fprintf(&b, "| %s | %d | %d | %d | %d | %d |\n", a.Area, a.Portable, a.Ported, a.Partial, a.Deferred, a.NotStarted)
		t.Portable += a.Portable
		t.Ported += a.Ported
		t.Partial += a.Partial
		t.Deferred += a.Deferred
		t.NotStarted += a.NotStarted
	}
	fmt.Fprintf(&b, "| **total** | %d | %d | %d | %d | %d |\n\n", t.Portable, t.Ported, t.Partial, t.Deferred, t.NotStarted)
	if c.DeferredNotApplicable > 0 {
		fmt.Fprintf(&b, "Also deferred: %d N cases (the manifest says why they don't apply).\n\n", c.DeferredNotApplicable)
	}
	b.WriteString("| Feature | F cases | Partially ported | Deferred | Areas |\n| --- | ---: | ---: | ---: | --- |\n")
	for _, f := range c.Features {
		var areas []string
		for a, n := range f.Areas {
			areas = append(areas, fmt.Sprintf("%s %d", a, n))
		}
		sort.Strings(areas)
		fmt.Fprintf(&b, "| %s | %d | %d | %d | %s |\n", f.Feature, f.Cases, f.PartiallyPorted, f.Deferred, strings.Join(areas, ", "))
	}
	return b.String()
}
