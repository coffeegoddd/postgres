"""The C suite: SQL correctness.  Dolthub's sqllogictest corpus, run by
slt-runner (slt/, built by build_slt.sh) against the table AM under test.

Each workload is a part of the corpus and one cell: the runner takes the
whole part, with its own worker processes.  The cell's metrics are counts of
records by status; the per-record results (records.jsonl.gz, with digests of
the actual answers) stay in the cell's raw directory for comparison with
other engines.
"""

from __future__ import annotations

import json
import os
from pbench.core import process
from pathlib import Path
from typing import Any, ClassVar

from pbench.core import registry, targets
from pbench.core.interfaces import Context, Outcome, Unavailable, Window, Workload

# Record statuses slt-runner reports (slt/merge.go).
STATUSES = ["ok", "not_ok", "not_implemented", "crashed", "timeout",
            "did_not_run", "skipped"]


class CWorkload(Workload):
    suite = "C"
    # Paths below the corpus's test/ directory.
    paths: ClassVar[list[str]] = []

    def client_counts(self, configured, variant):
        return [1]

    def _corpus(self, ctx: Context) -> Path:
        return Path(ctx.tools.get("sqllogictest", "")).expanduser() / "test"

    def _runner(self, ctx: Context) -> Path:
        return Path(ctx.tools.get("slt_runner", ""))

    def check(self, ctx: Context) -> None:
        if not self._runner(ctx).is_file():
            raise Unavailable(f"no slt-runner at {self._runner(ctx)} (run build_slt.sh)")
        if not self._corpus(ctx).is_dir():
            raise Unavailable(f"no sqllogictest corpus at {self._corpus(ctx)}")

    def prepare(self, ctx: Context) -> None:
        pass

    def files(self, ctx: Context) -> list[str]:
        """The test files or directories to run.  With sample =
        "per_directory", the first file of each directory."""
        roots = [self._corpus(ctx) / p for p in self.paths]
        if ctx.params.get("sample", "all") == "all":
            return [str(r) for r in roots]
        first: dict[Path, Path] = {}
        for r in roots:
            for f in sorted(r.rglob("*.test")) if r.is_dir() else [r]:
                first.setdefault(f.parent, f)
        return [str(f) for f in sorted(first.values())]

    def measure(self, ctx: Context, window: Window) -> Outcome:
        target = targets.for_am(ctx.table_am)
        out = ctx.raw_dir / "slt"
        args = [str(self._runner(ctx)),
                "-dsn", ctx.cluster.conninfo(),
                "-am", ctx.table_am,
                "-workers", str(int(ctx.params.get("workers") or os.cpu_count() or 1)),
                "-timeout", str(int(ctx.params.get("timeout_seconds", 30))),
                "-out", str(out), "-scratch", str(ctx.scratch_dir / "slt"),
                "-server-log", str(ctx.cluster.log_file)]
        if target.schema:
            args += ["-schema", target.schema]
        args += self.files(ctx)
        with window:
            r = process.run(args, capture_output=True, text=True)
        (ctx.raw_dir / "slt-runner.out").write_text(" ".join(args) + "\n\n" + r.stdout + r.stderr)
        summary_path = out / "summary.json"
        if r.returncode not in (0, 3) or not summary_path.exists():
            raise RuntimeError(f"slt-runner failed (exit {r.returncode}): {r.stderr[-2000:]}")
        s = json.loads(summary_path.read_text())
        if s["wrong_am_files"]:
            raise RuntimeError(f"tables not on {ctx.table_am} in {len(s['wrong_am_files'])} "
                               f"files, e.g. {s['wrong_am_files'][0]} (see slt/files.jsonl)")
        return Outcome(metrics=metrics(s), transactions=s["total"] - s["counts"].get("skipped", 0),
                       unit="record", read_only=True)


def metrics(summary: dict[str, Any]) -> dict[str, Any]:
    ran = summary["total"] - summary["counts"].get("skipped", 0)
    m: dict[str, Any] = {"records": summary["total"], "files": summary["files"],
                         "pass_percent": summary["pass_percent"] if ran else None}
    for st in STATUSES:
        m[st] = summary["counts"].get(st, 0)
    m["reruns"] = len(summary["reruns"])
    m["unmatched_digests"] = summary["unmatched_digests"]
    return m


@registry.workloads.register("C1")
class C1(CWorkload):
    id, title = "C1", "select1-5: joins, subqueries, expressions"
    paths = [f"select{i}.test" for i in range(1, 6)]


@registry.workloads.register("C2")
class C2(CWorkload):
    id, title = "C2", "evidence: statements from SQLite's documentation"
    paths = ["evidence"]


@registry.workloads.register("C3")
class C3(CWorkload):
    id, title = "C3", "index: queries with and without indexes"
    paths = ["index"]


@registry.workloads.register("C4")
class C4(CWorkload):
    id, title = "C4", "random: generated aggregates, expressions, GROUP BY, SELECT"
    paths = ["random"]


# The corpus's ddl/ is left out: every record in it is onlyif mysql.
