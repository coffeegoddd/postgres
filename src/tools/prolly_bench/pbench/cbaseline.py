"""Heap's outcome on the pinned corpus, checked in both directions.

  prolly_bench cbaseline check RESULTS...    a heap run against the file
  prolly_bench cbaseline update RESULTS...   rewrite the file from a full run

configs/c_heap_baseline.tsv.gz has one line per record heap doesn't pass,
sorted by file and line:

    file  line  status  sqlstate  sorted-answer-digest

("-" for an empty field), after a first line naming the corpus commit.
Every other record of the corpus passes or is skipped, so the file is
heap's whole outcome.  cgate compares an engine with heap from the same
build, so it can't see a change to shared code (the planner, the executor)
that breaks both alike; this check can.  It covers the files the run
covered, so a shard or a sample checks its own part, and fails:

  - on a record heap fails that the file doesn't list, or lists with
    another status, SQLSTATE or answer (a regression in shared code);
  - on a listed record heap now passes (the file must be updated, so it
    stays exact);
  - on records that timed out or didn't run (incomplete, not a
    regression: those are never written to the file).

The corpus pin, configs/sqllogictest.commit, is also what CI checks out;
bumping it regenerates the file in the same change.
"""

from __future__ import annotations

import gzip
import io
import json
import os
import subprocess
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable, Iterator

from pbench import ccompare, cshard
from pbench.table import ResultSet

HERE = Path(__file__).resolve().parents[1]
BASELINE = HERE / "configs" / "c_heap_baseline.tsv.gz"
PIN = HERE / "configs" / "sqllogictest.commit"

# Statuses that mean a record has no result: never in the file.
INCOMPLETE = ("timeout", "did_not_run")
PASSING = ("ok", "skipped")
LIMIT = 50


def pin(path: Path = PIN) -> str:
    return path.read_text().strip()


def _field(v: Any) -> str:
    return str(v) if v not in (None, "") else "-"


def entry(r: dict[str, Any]) -> tuple[str, str, str]:
    """A failing record's signature: status, SQLSTATE, sorted-answer digest."""
    return r["status"], _field(r.get("sqlstate")), _field(r.get("s"))


@dataclass
class Baseline:
    commit: str
    entries: dict[tuple[str, int], tuple[str, str, str]]


def read(path: Path = BASELINE) -> Baseline:
    with gzip.open(path, "rt") as f:
        header = f.readline().split()
        if header[:2] != ["#", "sqllogictest"] or len(header) != 3:
            raise ValueError(f"{path}: no '# sqllogictest COMMIT' first line")
        entries = {}
        previous = None
        for n, line in enumerate(f, 2):
            parts = line.rstrip("\n").split("\t")
            if len(parts) != 5:
                raise ValueError(f"{path}:{n}: expected 5 tab-separated fields")
            key = (parts[0], int(parts[1]))
            if previous is not None and key <= previous:
                raise ValueError(f"{path}:{n}: duplicate or out of order")
            entries[key] = (parts[2], parts[3], parts[4])
            previous = key
    return Baseline(header[2], entries)


def write(path: Path, commit: str, entries: dict[tuple[str, int], tuple[str, str, str]]) -> None:
    text = io.StringIO()
    text.write(f"# sqllogictest {commit}\n")
    for (file, line), sig in sorted(entries.items()):
        text.write("\t".join([file, str(line), *sig]) + "\n")
    # (no name or time in the header: the same outcome gives the same bytes;
    # written beside and renamed, so an interrupted update leaves the old file)
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_bytes(gzip.compress(text.getvalue().encode(), mtime=0))
    os.replace(tmp, path)


@dataclass
class Check:
    """A heap run against the baseline."""
    files: set[str] = field(default_factory=set)
    records: int = 0
    new: list[dict[str, Any]] = field(default_factory=list)       # failing, not listed
    changed: list[dict[str, Any]] = field(default_factory=list)   # failing differently
    fixed: list[dict[str, Any]] = field(default_factory=list)     # listed, now passing
    incomplete: list[dict[str, Any]] = field(default_factory=list)
    problems: list[str] = field(default_factory=list)             # the run or the file
    workloads: list[str] = field(default_factory=list)

    @property
    def ok(self) -> bool:
        return not (self.new or self.changed or self.fixed or self.incomplete or self.problems)

    def messages(self, limit: int = LIMIT) -> list[str]:
        out = list(self.problems)
        if self.incomplete:
            out.append(_incomplete(self.incomplete))

        def listed(kind, items, text):
            for d in items[:limit]:
                out.append(f"{kind}: {d['file']}:{d['line']}: {text(d)}")
            if len(items) > limit:
                out.append(f"{kind}: ... {len(items) - limit:,} more")
        listed("regression in shared code", self.new,
               lambda d: f"heap now fails ({' '.join(d['now'])}), not in the baseline")
        listed("regression in shared code", self.changed,
               lambda d: f"heap fails differently: baseline {' '.join(d['listed'])}, "
                         f"now {' '.join(d['now'])}")
        listed("fixed", self.fixed,
               lambda d: "heap passes now; remove it from the baseline "
                         "(prolly_bench cbaseline update)")
        return out

    def summary(self) -> str:
        return (f"heap baseline over {len(self.files):,} files, {self.records:,} records: "
                f"{len(self.new) + len(self.changed):,} new failures "
                f"({len(self.changed):,} changed), {len(self.fixed):,} fixed, "
                f"{len(self.incomplete):,} incomplete: {'pass' if self.ok else 'FAIL'}")

    def to_json(self) -> dict[str, Any]:
        return {"ok": self.ok, "files": len(self.files), "records": self.records,
                "workloads": self.workloads, "new": self.new, "changed": self.changed,
                "fixed": self.fixed, "incomplete": self.incomplete, "problems": self.problems}


def _incomplete(items: list[dict[str, Any]]) -> str:
    return (f"incomplete: {len(items):,} records timed out or didn't run (not a "
            "regression; never written to the baseline), e.g. "
            + ", ".join(f"{d['file']}:{d['line']} {d['status']}" for d in items[:5]))


def _records(d: Path) -> Iterator[dict[str, Any]]:
    with gzip.open(d / "records.jsonl.gz", "rt") as f:
        for line in f:
            yield json.loads(line)


def _corpus_state(rs: ResultSet) -> tuple[str, int]:
    """The corpus checkout a run used: its commit and how many files differ
    from it (-1 if unknown)."""
    try:
        env = json.loads((rs.path / "environment.json").read_text())
        checkout = env.get("sqllogictest") or {}
        return str(checkout.get("commit", "")), int(checkout.get("dirty_files", -1))
    except (OSError, ValueError, TypeError, AttributeError):
        return "", -1


def _checkout_state(corpus: Path) -> tuple[str, int]:
    """The commit of the corpus checkout holding corpus (its test/
    directory), and how many files differ from it."""
    def run(*args: str) -> str:
        r = subprocess.run(["git", "-C", str(corpus.parent), *args],
                           capture_output=True, text=True)
        if r.returncode:
            raise ValueError(f"{corpus.parent}: not a checkout of the corpus")
        return r.stdout
    return run("rev-parse", "HEAD").strip(), len(run("status", "--porcelain").splitlines())


def check(sets: Iterable[ResultSet], baseline: Baseline, commit: str) -> Check:
    """Heap result sets (one run, or the shards of one) against baseline,
    over the files they covered.  commit is the corpus pin."""
    c = Check()
    if baseline.commit != commit:
        c.problems.append(f"the baseline is for sqllogictest {baseline.commit[:12]}, the pin "
                          f"is {commit[:12]}: regenerate it (prolly_bench cbaseline update)")
    seen: set[tuple[str, int]] = set()
    for rs in sets:
        if rs.suite != "C" or rs.table_am != "heap":
            c.problems.append(f"{rs.path.name}: not a C suite run on heap")
            continue
        # (the result set's own validation; records that didn't run are this
        # check's to report, as incomplete)
        invalid = [p for p in rs.problems if not ccompare.is_status_problem(p)]
        if invalid:
            c.problems += [f"{rs.path.name}: {p}" for p in invalid]
            continue
        run_commit, dirty = _corpus_state(rs)
        if run_commit != commit:
            c.problems.append(f"{rs.path.name}: ran sqllogictest {run_commit[:12] or '?'}, "
                              f"the pin is {commit[:12]}")
        elif dirty != 0:
            c.problems.append(f"{rs.path.name}: ran a corpus checkout with local changes "
                              f"({dirty if dirty > 0 else 'unknown'} files)")
        dirs = ccompare.slt_dirs(rs)
        wanted = ccompare.run_workloads(rs)
        c.workloads += [w for w in wanted if w in dirs]
        for w in wanted:
            if w not in dirs:
                c.problems.append(f"{rs.path.name}: incomplete: no records for {w}")
        for w, d in sorted(dirs.items()):
            errors, corpus = ccompare.evidence(d, "heap", statuses=False)
            c.problems += [f"{rs.path.name}/{w}: {e}" for e in errors]
            if errors:
                continue
            overlap = c.files & corpus.keys()
            if overlap:
                c.problems.append(f"{rs.path.name}/{w}: files run twice, e.g. {min(overlap)}")
            c.files |= corpus.keys()
            for r in _records(d):
                key = (r["file"], r["line"])
                c.records += 1
                listed = baseline.entries.get(key)
                if listed:
                    seen.add(key)
                where = {"file": r["file"], "line": r["line"], "workload": w}
                if r["status"] in INCOMPLETE:
                    c.incomplete.append({**where, "status": r["status"]})
                elif r["status"] in PASSING:
                    if listed:
                        c.fixed.append({**where, "listed": list(listed), "now": r["status"]})
                elif listed is None:
                    c.new.append({**where, "now": list(entry(r)), "msg": r.get("msg", "")})
                elif listed != entry(r):
                    c.changed.append({**where, "listed": list(listed), "now": list(entry(r)),
                                      "msg": r.get("msg", "")})
    if not c.files and not c.problems:
        c.problems.append("no heap records to check")
    gone = sorted(k for k in baseline.entries if k[0] in c.files and k not in seen)
    if gone:
        c.problems.append(f"{len(gone):,} baseline records aren't in the run's files, e.g. "
                          f"{gone[0][0]}:{gone[0][1]}: regenerate the baseline")
    c.workloads = sorted(set(c.workloads), key=lambda w: int(w[1:]))
    return c


def update(sets: list[ResultSet], path: Path, commit: str, corpus: Path) -> list[str]:
    """Rewrite the baseline from heap result sets covering the whole corpus
    at the pin, with every record run to a result.  Returns problems; writes
    nothing if there are any."""
    c = check(sets, Baseline(commit, {}), commit)
    problems = list(c.problems)
    if c.incomplete:
        problems.append(_incomplete(c.incomplete))
    try:
        # (the checkout is listed as it is now: it must still be the pin)
        head, dirty = _checkout_state(corpus)
        if head != commit or dirty:
            problems.append(f"the corpus at {corpus.parent} is at {head[:12]} with {dirty} "
                            f"changed files, not the pin {commit[:12]}")
        full = cshard.workload_files(corpus, {"workloads": {}})
        if {f for fs in full.values() for f in fs} != c.files:
            problems.append("not a full run of the corpus: the baseline is written from "
                            "every file")
    except (OSError, ValueError) as e:
        problems.append(f"cannot list the corpus: {e}")
    if problems:
        return problems
    write(path, commit, {(d["file"], d["line"]): tuple(d["now"]) for d in c.new})
    return []
