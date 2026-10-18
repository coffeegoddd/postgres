import json
from pathlib import Path
from pbench.core.validation import sha256


def seal(path: Path, suite="S", am="heap", label=None):
    run_path = path / "run.json"
    run = json.loads(run_path.read_text()) if run_path.exists() else {
        "suite": suite, "table_am": am, "label": label or am, "started": "2026-10-16"}
    results = [json.loads(line) for line in (path / "results.jsonl").read_text().splitlines()]
    for r in results:
        r.setdefault("unit", "transaction")
        if r.get("denominator") is None:
            r["denominator"] = 1
        measurement = r.setdefault("measurement", {})
        for name, value in {"seconds": 1, "quality": "smoke", "seed": 1}.items():
            measurement.setdefault(name, value)
    (path / "results.jsonl").write_text("".join(json.dumps(r) + "\n" for r in results))
    for name in ("config.json", "environment.json"):
        if not (path / name).exists():
            (path / name).write_text("{}")
    names = ["config.json", "environment.json", "results.jsonl"]
    names += [str(p.relative_to(path)) for p in (path / "raw").rglob("*") if p.is_file()]
    run.update(schema_version=2, state="complete", comparison_identity="test",
               planned_cells=[r["key"] for r in results],
               artifacts={n: sha256(path / n) for n in names})
    run_path.write_text(json.dumps(run))


def c_result(path: Path, am: str, records: dict, config=None, commit="pin", label=None):
    """A sealed C result set at path: records maps workload -> records
    (file, line, status and any of sqlstate, s, msg), one cell each."""
    import gzip
    from pbench.table import ResultSet
    results = []
    for w, recs in sorted(records.items()):
        raw = path / "raw" / w / "slt"
        raw.mkdir(parents=True)
        recs = sorted(({"version": 2, "o": "o", "s": "s", **r} if r["status"] == "ok"
                       else {"version": 2, **r} for r in recs),
                      key=lambda r: (r["file"], r["line"]))
        with gzip.open(raw / "records.jsonl.gz", "wt") as f:
            f.writelines(json.dumps(r) + "\n" for r in recs)
        with gzip.open(raw / "planned.jsonl.gz", "wt") as f:
            f.writelines(json.dumps({"file": r["file"], "line": r["line"],
                                     "skip": r["status"] == "skipped"}) + "\n" for r in recs)
        files = sorted({r["file"] for r in recs})
        (raw / "corpus.json").write_text(json.dumps({f: "hash-" + f for f in files}))
        (raw / "files.jsonl").write_text("".join(json.dumps({"file": f, "tables": 1}) + "\n"
                                                 for f in files))
        counts = {}
        for r in recs:
            counts[r["status"]] = counts.get(r["status"], 0) + 1
        (raw / "summary.json").write_text(json.dumps({
            "total": len(recs), "files": len(files), "counts": counts, "wrong_am_files": [],
            "unmatched_digests": 0, "unparsed_lines": 0, "am": am}))
        ran = len(recs) - counts.get("skipped", 0)
        results.append({"key": {"workload": w}, "status": "ok", "raw_dir": f"raw/{w}",
                        "metrics": {"records": len(recs), "skipped": counts.get("skipped", 0),
                                    "ok": counts.get("ok", 0), "window_seconds": 1.5,
                                    "pass_percent": 100 * counts.get("ok", 0) / ran if ran else None}})
    (path / "results.jsonl").write_text("".join(json.dumps(r) + "\n" for r in results))
    (path / "config.json").write_text(json.dumps(config or {"run": {}}))
    (path / "environment.json").write_text(json.dumps({"sqllogictest": {"commit": commit,
                                                                     "dirty_files": 0}}))
    label = label or ("B0" if am == "heap" else am)
    seal(path, "C", am, label)
    return ResultSet(path, "C", label, am, "2026-10-18", results)
