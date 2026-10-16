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
