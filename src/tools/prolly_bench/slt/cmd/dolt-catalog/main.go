// dolt-catalog produces a reproducible source inventory; it does not run tests.
package main

import (
	"bytes"
	"encoding/json"
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"prolly_bench/slt/internal/doltcatalog"
)

func run() error {
	repo := flag.String("repo", "", "local Dolt Git checkout")
	revision := flag.String("revision", "", "full pinned commit SHA")
	out := flag.String("out", "", "new output directory")
	check := flag.String("check", "", "optional committed source-lock.json to compare")
	flag.Parse()
	if *repo == "" || *out == "" || flag.NArg() != 0 {
		return fmt.Errorf("requires -repo, -revision and -out")
	}
	c, err := doltcatalog.Read(*repo, *revision)
	if err != nil {
		return err
	}
	lock, err := json.MarshalIndent(c.Lock, "", "  ")
	if err != nil {
		return err
	}
	lock = append(lock, '\n')
	if *check != "" {
		b, err := os.ReadFile(*check)
		if err != nil {
			return err
		}
		if !bytes.Equal(b, lock) {
			return fmt.Errorf("source lock differs: review the pin, file hashes and counts before updating")
		}
	}
	if err := os.Mkdir(*out, 0755); err != nil {
		return err
	}
	if err := os.WriteFile(filepath.Join(*out, "source-lock.json"), lock, 0644); err != nil {
		return err
	}
	b, err := json.MarshalIndent(c, "", "  ")
	if err != nil {
		return err
	}
	if err := os.WriteFile(filepath.Join(*out, "catalog.json"), append(b, '\n'), 0644); err != nil {
		return err
	}
	fmt.Printf("%d source cases / %d assertion entries (not executed coverage)\n", c.Lock.Counts.Cases, c.Lock.Counts.Assertions)
	return nil
}
func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, "dolt-catalog:", err)
		os.Exit(1)
	}
}
