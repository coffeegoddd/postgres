"""Derived reports, exact invocation manifests, and a local result catalog."""
from __future__ import annotations

import dataclasses
import datetime as dt
import json
import os
import uuid
from pathlib import Path

from pbench import presentation, reports
from pbench.core.validation import atomic_json, sha256
from pbench.presentation import Document, Section, link

SCHEMA = 1


def result_set(source: reports.Source, formats: str = "both") -> tuple[list[Path], bool]:
    document = reports.build([source], source.path)
    paths = presentation.write(document, source.path / "index", formats,
                               markdown_path=source.path / "README.md")
    return paths, document.passed


def begin(runners, formats: str) -> Path:
    root = runners[0].out_dir.parent
    identity = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ-") + uuid.uuid4().hex[:10]
    path = root / "reports" / identity / "report.json"
    path.parent.mkdir(parents=True)
    inputs = [{"path": os.path.relpath(r.out_dir, path.parent),
               "run": {"suite": r.suite, "label": r.label, "table_am": r.table_am,
                       "build": r.build, "builds": list(r.builds),
                       "planned_cells": [dataclasses.asdict(k) for k, _ in r._cells()]}}
              for r in runners]
    atomic_json(path, {"schema_version": SCHEMA, "id": identity, "inputs": inputs,
                       "execution": "running", "rendering": "pending", "formats": formats})
    return path


def invocation(path: Path, *, execution: str | None = None,
               formats: str | None = None) -> tuple[list[Path], bool]:
    manifest = json.loads(path.read_text())
    if manifest.get("schema_version") != SCHEMA:
        raise ValueError("unknown report manifest schema")
    if execution:
        manifest["execution"] = execution
    formats = formats or manifest.get("formats", "both")
    manifest.update(rendering="running", formats=formats)
    atomic_json(path, manifest)
    outputs, sources, errors = [], [], []
    try:
        for entry in manifest["inputs"]:
            source = reports.load((path.parent / entry["path"]).resolve())
            source.run = {**entry.get("run", {}), **source.run}
            observed = {name: sha256(source.path / name) for name in
                        ("run.json", "results.jsonl", "config.json", "environment.json")
                        if (source.path / name).is_file()}
            if entry.get("hashes") and entry["hashes"] != observed:
                source.problems.append("Source evidence changed since this invocation report was recorded")
            else:
                entry["hashes"] = observed
            sources.append(source)
            # A missing result set stays in the invocation report; do not
            # manufacture an evidence directory for a runner that never opened.
            if (source.path / "run.json").exists():
                try:
                    paths, _ = result_set(source, formats)
                    outputs += paths
                except (OSError, ValueError, RuntimeError) as e:
                    errors.append(f"{source.label}: {e}")
        document = reports.build(sources, path.parent)
        document.notes.insert(0, f"Invocation {manifest['id']}; execution: {manifest['execution']}.")
        document.notes += errors
        outputs += presentation.write(document, path.parent / "index", formats,
                                       markdown_path=path.parent / "README.md")
        manifest.update(rendering="failed" if errors else "complete", renderer_version=SCHEMA,
                        passed=document.passed and not errors,
                        outputs=[os.path.relpath(p, path.parent) for p in outputs], errors=errors)
    except (OSError, ValueError, RuntimeError) as e:
        manifest.update(rendering="failed", errors=[*errors, str(e)])
        raise
    finally:
        atomic_json(path, manifest)
    if errors:
        raise RuntimeError("report rendering failed: " + "; ".join(errors))
    return outputs, document.passed


def catalog(root: Path, formats: str = "both") -> list[Path]:
    document = Document("Benchmark results", [
        "All saved result sets, including incomplete and invalid evidence. "
        "Individual runs and invocations are listed without merging retries or experiments."])
    runs = Section("Result sets", ["Result set", "Suite", "Engine / build", "Started", "Quality",
                                  "Execution", "Planned / finished / successful", "Evidence", "HTML", "Markdown"])
    for path in sorted(root.glob("*/run.json"), reverse=True):
        source = reports.load(path.parent)
        run = source.run
        runs.rows.append([link(path, root, source.path.name), run.get("suite", "?"),
                          f"{run.get('table_am', '?')} / {run.get('build') or 'default'}", run.get("started", "?"),
                          source.config.get("run", {}).get("quality", "legacy/unverified"), run.get("state", "unverified"),
                          f"{len(run.get('planned_cells', []))} / {len(source.results)} / "
                          f"{sum(not reports.cell_problems(r) for r in source.results)}",
                          "; ".join(source.problems) or "valid",
                          link(source.path / "index.html", root, "HTML") if (source.path / "index.html").exists() else "not generated",
                          link(source.path / "README.md", root, "Markdown") if (source.path / "README.md").exists() else "not generated"])
    document.sections.append(runs)
    invocations = Section("Invocations", ["Invocation", "Execution", "Rendering", "Evidence", "HTML", "Markdown"])
    for path in sorted((root / "reports").glob("*/report.json"), reverse=True):
        problems = []
        data = reports.read_json(path, problems)
        invocations.rows.append([link(path, root, data.get("id", path.parent.name)),
                                 data.get("execution", "unknown"), data.get("rendering", "unknown"),
                                 "; ".join(problems) or ("valid" if data.get("passed") else "invalid or not yet evaluated"),
                                 link(path.parent / "index.html", root, "HTML") if (path.parent / "index.html").exists() else "not generated",
                                 link(path.parent / "README.md", root, "Markdown") if (path.parent / "README.md").exists() else "not generated"])
    document.sections.append(invocations)
    return presentation.write(document, root / "index", formats)
