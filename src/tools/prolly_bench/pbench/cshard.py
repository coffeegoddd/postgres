"""Shards of the C suite: the corpus's files split across N runs, so the
full corpus can run as parallel jobs.

  prolly_bench run C --shard I/N ...     run shard I of N
  prolly_bench cshard --shards N         print the plan

Files are assigned largest first, each to the shard with the fewest bytes so
far (the lowest-numbered on a tie), so the assignment depends only on the
corpus's file names and sizes.  C1 and C2 are small and go whole to shard 1,
ahead of the rest.  A shard's result set records the shard and its files in
config.json, so a report can tell a file outside the shard from a file that
is missing.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable

from pbench.core import registry

# Workloads that run whole on shard 1.
FIRST_SHARD = ("C1", "C2")


@dataclass
class Shard:
    index: int                   # 1-based
    count: int
    files: dict[str, list[str]] = field(default_factory=dict)   # workload -> files
    bytes: int = 0

    @property
    def spec(self) -> str:
        return f"{self.index}/{self.count}"

    @property
    def workloads(self) -> list[str]:
        return sorted((w for w, fs in self.files.items() if fs), key=lambda w: int(w[1:]))


def parse(spec: str) -> tuple[int, int]:
    """'I/N' -> (I, N), with 1 <= I <= N."""
    m = re.fullmatch(r"(\d+)/(\d+)", spec or "")
    if not m or not 1 <= int(m[1]) <= int(m[2]):
        raise ValueError(f"invalid shard {spec!r}: expected I/N with 1 <= I <= N")
    return int(m[1]), int(m[2])


def workload_files(corpus: Path, cfg: dict[str, Any],
                   workloads: Iterable[str] = ()) -> dict[str, list[str]]:
    """Workload -> its test files, as paths below the corpus's test/
    directory, with the config's sampling applied."""
    registry.load_all()
    wanted = list(workloads) or [n for n, c in registry.workloads.items() if c.suite == "C"]
    out = {}
    for name in wanted:
        cls = registry.workloads.get(name)
        sample = cfg.get("workloads", {}).get(name, {}).get("sample", "all")
        out[name] = [str(f.relative_to(corpus)) for f in cls.select_files(corpus, sample)]
    return out


def assign(files: dict[str, list[str]], sizes: dict[str, int], count: int) -> list[Shard]:
    """Split files (workload -> files) into count shards; sizes gives each
    file's bytes."""
    if count < 1:
        raise ValueError("at least one shard")
    shards = [Shard(i + 1, count, {w: [] for w in files}) for i in range(count)]
    owner = {f: w for w, fs in files.items() for f in fs}
    if len(owner) != sum(len(fs) for fs in files.values()):
        raise ValueError("a file belongs to two workloads")
    rest = []
    for f, w in owner.items():
        if w in FIRST_SHARD:
            shards[0].files[w].append(f)
            shards[0].bytes += sizes[f]
        else:
            rest.append(f)
    for f in sorted(rest, key=lambda f: (-sizes[f], f)):
        s = min(shards, key=lambda s: (s.bytes, s.index))
        s.files[owner[f]].append(f)
        s.bytes += sizes[f]
    for s in shards:
        for fs in s.files.values():
            fs.sort()
    return shards


def plan(corpus: Path, cfg: dict[str, Any], count: int,
         workloads: Iterable[str] = ()) -> list[Shard]:
    files = workload_files(corpus, cfg, workloads)
    sizes = {f: (corpus / f).stat().st_size for fs in files.values() for f in fs}
    return assign(files, sizes, count)


def recorded(config: dict[str, Any]) -> tuple[str, dict[str, list[str]]]:
    """A result set's shard ('' when it ran unsharded) and, if sharded, its
    files per workload, from its config.json."""
    spec = config.get("run", {}).get("shard", "")
    if not spec:
        return "", {}
    return spec, {w: list(p.get("shard_files", []))
                  for w, p in config.get("workloads", {}).items() if p.get("shard_files")}


def apply(cfg: dict[str, Any], corpus: Path, spec: str) -> Shard:
    """Restrict a C run's configuration to shard spec: its workloads, each
    with the files of the shard."""
    index, count = parse(spec)
    shard = plan(corpus, cfg, count, cfg["run"]["workloads"])[index - 1]
    cfg["run"]["shard"] = shard.spec
    cfg["run"]["workloads"] = shard.workloads
    for w in shard.workloads:
        cfg["workloads"].setdefault(w, {})["shard_files"] = shard.files[w]
    return shard


def overlap_and_gaps(shard_files: list[dict[str, list[str]]],
                     full: dict[str, list[str]]) -> list[str]:
    """Problems with a set of shards as a cover of the full corpus: a file
    in two shards, a file in none, or a file that isn't in the corpus."""
    problems = []
    seen: dict[tuple[str, str], int] = {}
    for i, files in enumerate(shard_files):
        for w, fs in files.items():
            for f in fs:
                if (w, f) in seen:
                    problems.append(f"{w} {f}: in two shards")
                seen[(w, f)] = i
    want = {(w, f) for w, fs in full.items() for f in fs}
    missing = sorted(want - set(seen))
    extra = sorted(set(seen) - want)
    problems += [f"{w} {f}: in no shard" for w, f in missing[:20]]
    if len(missing) > 20:
        problems.append(f"... {len(missing) - 20} more files in no shard")
    problems += [f"{w} {f}: not in the corpus" for w, f in extra[:20]]
    if len(extra) > 20:
        problems.append(f"... {len(extra) - 20} more files not in the corpus")
    return problems


def to_text(shards: list[Shard]) -> str:
    out = []
    for s in shards:
        counts = ", ".join(f"{w} {len(s.files[w])}" for w in s.workloads)
        out.append(f"shard {s.spec}: {s.bytes / 1e6:.1f} MB; --workloads "
                   f"{','.join(s.workloads)}; files: {counts}")
    return "\n".join(out)
