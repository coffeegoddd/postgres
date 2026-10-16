"""Results on disk under specs/results/<date>-<suite>-<label>/:

  README.md         what was run, warnings, and the summary tables
  config.json       the resolved configuration
  environment.json  machine, build and device (with fio baseline)
  results.jsonl     one line per cell, written as cells finish
  raw/              per-cell raw output (pgbench logs, server logs, ...)
"""

from __future__ import annotations

import dataclasses
import json

from pbench.core import registry
from pbench.core.interfaces import CellResult, RunInfo, Sink
from pbench import report
from pbench.core.validation import (SCHEMA_VERSION, atomic_json, comparison_identity,
                                    sha256)


def result_to_json(r: CellResult) -> dict:
    d = dataclasses.asdict(r)
    d["status"] = r.status.value
    return d


@registry.sinks.register("files")
class FilesSink(Sink):
    name = "files"

    def open(self, run: RunInfo) -> None:
        (run.out_dir / "config.json").write_text(json.dumps(run.config, indent=2) + "\n")
        (run.out_dir / "environment.json").write_text(
            json.dumps(run.environment, indent=2, default=str) + "\n")
        self.run = run
        self.manifest = {
            "schema_version": SCHEMA_VERSION, "state": "running",
            "planned_cells": [dataclasses.asdict(k) for k in run.planned_cells],
            "comparison_identity": comparison_identity(run.config, run.environment),
            "suite": run.suite, "label": run.label, "table_am": run.table_am,
            "started": run.started, "command": run.command,
            "warnings": run.warnings}
        atomic_json(run.out_dir / "run.json", self.manifest)
        self.results = open(run.out_dir / "results.jsonl", "w")

    def cell(self, r: CellResult) -> None:
        d = result_to_json(r)
        if r.raw_dir:
            from pathlib import Path
            d["raw_dir"] = str(Path(r.raw_dir).relative_to(self.run.out_dir))
        self.results.write(json.dumps(d, allow_nan=False) + "\n")
        self.results.flush()

    def close(self, run: RunInfo, results: list[CellResult]) -> None:
        self.results.close()
        names = ["config.json", "environment.json", "results.jsonl"]
        names += [str(p.relative_to(run.out_dir)) for p in sorted((run.out_dir / "raw").rglob("*"))
                  if p.is_file()]
        self.manifest.update(state="complete", artifacts={n: sha256(run.out_dir / n) for n in names})
        atomic_json(run.out_dir / "run.json", self.manifest)
        report.write_readme(run.out_dir)
