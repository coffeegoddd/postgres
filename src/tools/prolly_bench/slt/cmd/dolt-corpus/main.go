// dolt-corpus checks provenance and joins execution evidence to source coverage.
package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"os"
	"prolly_bench/slt/internal/doltport"
)

func run() error {
	root := flag.String("root", "", "corpus/dolt directory")
	catalog := flag.String("catalog", "", "optional pinned catalog.json")
	results := flag.String("results", "", "optional engine-vc summary.json")
	out := flag.String("out", "", "new report file")
	markdown := flag.String("markdown", "", "optional new file for the coverage tables, by area and by feature")
	flag.Parse()
	if *root == "" || *out == "" || flag.NArg() != 0 {
		return fmt.Errorf("requires -root and -out")
	}
	report, err := doltport.Verify(*root, *catalog, *results)
	if err != nil {
		return err
	}
	b, err := json.MarshalIndent(report, "", "  ")
	if err != nil {
		return err
	}
	f, err := os.OpenFile(*out, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0644)
	if err != nil {
		return err
	}
	_, err = f.Write(append(b, '\n'))
	closeErr := f.Close()
	if err != nil {
		return err
	}
	if closeErr != nil {
		return closeErr
	}
	if *markdown != "" {
		m, err := os.OpenFile(*markdown, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0644)
		if err != nil {
			return err
		}
		_, err = m.WriteString(report.Classes.Markdown())
		if closeErr := m.Close(); err == nil {
			err = closeErr
		}
		if err != nil {
			return err
		}
	}
	fmt.Printf("%d ported, %d partial, %d deferred, %d unreviewed / %d source cases; %d mapped assertions\n", report.PortedCases, report.PartialCases, report.DeferredCases, report.UnreviewedCases, report.SourceCases, report.MappedAssertions)
	c := report.Classes
	fmt.Printf("classified: %d portable, %d feature-gated, %d not applicable\n", c.Portable, c.FeatureGated, c.NotApplicable)
	for _, a := range c.Areas {
		fmt.Printf("  P %-14s %3d: %d ported, %d partial, %d deferred, %d not started\n", a.Area, a.Portable, a.Ported, a.Partial, a.Deferred, a.NotStarted)
	}
	for _, e := range report.Engines {
		fmt.Printf("%s: %d passed, %d failed, %d unsupported, %d not run; %d passed mapped assertions\n", e.Engine, e.Passed, e.Failed, e.Unsupported, e.NotRun, e.PassedMappedAssertions)
	}
	for _, e := range report.Engines {
		if e.Failed+e.Unsupported+e.NotRun != 0 {
			return fmt.Errorf("incomplete execution coverage; see saved report")
		}
	}
	return nil
}
func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, "dolt-corpus:", err)
		os.Exit(1)
	}
}
