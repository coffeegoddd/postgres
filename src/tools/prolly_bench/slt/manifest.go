package main

import (
	"compress/gzip"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"

	"github.com/dolthub/sqllogictest/go/logictest/parser"
)

// Write the coverage denominator before starting workers. An early exit
// or merge omission must not shrink a successful run.
func writePlan(cfg *config, files []string) error {
	f, err := os.Create(filepath.Join(cfg.out, "planned.jsonl.gz"))
	if err != nil {
		return err
	}
	defer f.Close()
	gz := gzip.NewWriter(f)
	enc := json.NewEncoder(gz)
	hashes := map[string]string{}
	for _, path := range files {
		name := testFilePath(path)
		if _, found := hashes[name]; found {
			return fmt.Errorf("duplicate corpus file %s", name)
		}
		data, err := os.ReadFile(path)
		if err != nil {
			return err
		}
		h := sha256.Sum256(data)
		hashes[name] = hex.EncodeToString(h[:])
		recs, err := parser.ParseTestFile(path)
		if err != nil {
			return err
		}
		for _, r := range recs {
			if r.Type() == parser.Halt {
				if r.ShouldExecuteForEngine(engineStr) {
					break
				}
				continue
			}
			if err := enc.Encode(map[string]any{"file": name, "line": r.LineNum(),
				"skip": !r.ShouldExecuteForEngine(engineStr)}); err != nil {
				return err
			}
		}
	}
	if err := gz.Close(); err != nil {
		return err
	}
	b, err := json.MarshalIndent(hashes, "", "  ")
	if err != nil {
		return err
	}
	return os.WriteFile(filepath.Join(cfg.out, "corpus.json"), b, 0o644)
}
