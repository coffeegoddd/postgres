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
        (run.out_dir / "run.json").write_text(json.dumps({
            "suite": run.suite, "label": run.label, "table_am": run.table_am,
            "started": run.started, "command": run.command,
            "warnings": run.warnings}, indent=2) + "\n")
        self.results = open(run.out_dir / "results.jsonl", "w")

    def cell(self, r: CellResult) -> None:
        self.results.write(json.dumps(result_to_json(r)) + "\n")
        self.results.flush()

    def close(self, run: RunInfo, results: list[CellResult]) -> None:
        self.results.close()
        report.write_readme(run.out_dir)
