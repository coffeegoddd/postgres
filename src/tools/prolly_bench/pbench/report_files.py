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
SOURCE_FILES = ("run.json", "results.jsonl", "config.json", "environment.json")


def fingerprint(path: Path) -> dict[str, str]:
    """Fingerprint the small source files, never the raw artifact tree."""
    return {name: sha256(path / name) for name in SOURCE_FILES if (path / name).is_file()}


def result_set(source: reports.Source, formats: str = "both") -> tuple[list[Path], bool]:
    document = reports.build([source], source.path)
    paths = presentation.write(document, source.path / "index", formats,
                               markdown_path=source.path / "README.md")
    # Derived cache for the root catalog. Full evidence validation belongs to
    # explicit reports, not to scanning every historical raw log after a run.
    atomic_json(source.path / "report-status.json", {
        "schema_version": SCHEMA, "hashes": fingerprint(source.path),
        "finished": len(source.results),
        "successful": sum(not reports.cell_problems(r) for r in source.results),
        "problems": source.problems})
    return paths, document.passed


def begin(runners, formats: str) -> Path:
    root = runners[0].out_dir.parent
    identity = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ-") + uuid.uuid4().hex[:10]
    path = root / "reports" / identity / "report.json"
    path.parent.mkdir(parents=True)
    inputs = [{"path": os.path.relpath(r.out_dir, path.parent),
               "run": {"suite": r.suite, "label": r.label, "table_am": r.table_am,
                       "build": r.build, "builds": list(r.builds), "state": "not_started",
                       "planned_cells": [dataclasses.asdict(k) for k, _ in r._cells()]}}
              for r in runners]
    atomic_json(path, {"schema_version": SCHEMA, "id": identity, "inputs": inputs,
                       "execution": "running", "rendering": "pending", "formats": formats})
    return path


def invocation(path: Path, *, execution: str | None = None,
               formats: str | None = None, execution_error: str | None = None) -> tuple[list[Path], bool]:
    manifest = json.loads(path.read_text())
    if manifest.get("schema_version") != SCHEMA:
        raise ValueError("unknown report manifest schema")
    if execution:
        manifest["execution"] = execution
    if execution_error is not None:
        manifest["execution_error"] = execution_error
    formats = formats or manifest.get("formats", "both")
    manifest.update(rendering="running", formats=formats, passed=False, errors=[])
    atomic_json(path, manifest)
    outputs, sources, errors = [], [], []
    try:
        for entry in manifest["inputs"]:
            source_path = (path.parent / entry["path"]).resolve()
            try:
                source = reports.load(source_path)
                source.run = {**entry.get("run", {}), **source.run}
                observed = fingerprint(source.path)
                if "hashes" in entry and entry["hashes"] != observed:
                    source.problems.append("Source evidence changed since this invocation report was recorded")
                else:
                    entry["hashes"] = observed
                sources.append(source)
                # Keep missing inputs visible without manufacturing evidence.
                if (source.path / "run.json").exists():
                    paths, _ = result_set(source, formats)
                    outputs += paths
            except Exception as error:
                errors.append(f"{source_path.name}: {type(error).__name__}: {error}")
                if not any(s.path == source_path for s in sources):
                    sources.append(reports.Source(source_path, entry.get("run", {}), {}, {}, [],
                                                  [errors[-1]]))
        document = reports.build(sources, path.parent)
        document.notes.insert(0, f"Invocation {manifest['id']}; execution: {manifest['execution']}.")
        if manifest.get("execution_error"):
            document.notes.append(manifest["execution_error"])
        document.notes += errors
        document.passed &= not errors and manifest.get("execution") == "complete"
        outputs += presentation.write(document, path.parent / "index", formats,
                                       markdown_path=path.parent / "README.md")
        manifest.update(rendering="failed" if errors else "complete", renderer_version=SCHEMA,
                        passed=document.passed, errors=errors)
    except BaseException as error:
        manifest.update(rendering="interrupted" if isinstance(error, KeyboardInterrupt) else "failed",
                        passed=False, errors=[*errors, f"{type(error).__name__}: {error}"])
        if isinstance(error, Exception):
            raise RuntimeError("report rendering failed: " + "; ".join(manifest["errors"])) from error
        raise
    finally:
        manifest["outputs"] = [os.path.relpath(p, path.parent) for p in outputs]
        atomic_json(path, manifest)
    if errors:
        raise RuntimeError("report rendering failed: " + "; ".join(errors))
    return outputs, manifest["passed"]


def catalog(root: Path, formats: str = "both") -> list[Path]:
    document = Document("Benchmark results", [
        "All saved result sets, including incomplete and invalid evidence. "
        "Individual runs and invocations are listed without merging retries or experiments. "
        "Evidence status and counts are cached from the last report; raw evidence is not rechecked."])
    runs = Section("Result sets", ["Result set", "Suite", "Engine / build", "Started", "Quality",
                                  "Execution", "Planned / finished / successful", "Evidence", "HTML", "Markdown"])
    for path in sorted(root.glob("*/run.json"), reverse=True):
        problems = []
        run = reports.read_json(path, problems)
        config = reports.read_json(path.parent / "config.json", problems)
        cache_problems = []
        cache = reports.read_json(path.parent / "report-status.json", cache_problems)
        counts = "? / ?"
        status = "not validated; regenerate report"
        valid_cache = (cache.get("schema_version") == SCHEMA and
                       isinstance(cache.get("hashes"), dict) and
                       type(cache.get("finished")) is int and type(cache.get("successful")) is int and
                       isinstance(cache.get("problems"), list) and
                       all(isinstance(p, str) for p in cache["problems"]))
        if not problems and not cache_problems and valid_cache:
            try:
                current = cache.get("hashes") == fingerprint(path.parent)
            except OSError:
                current = False
            if current:
                counts = f"{cache.get('finished', '?')} / {cache.get('successful', '?')}"
                status = ("; ".join(cache.get("problems", [])) or "valid at last report")
                status += "; raw evidence not rechecked"
            else:
                status = "source files changed; regenerate report"
        if problems:
            status = "invalid result evidence: " + "; ".join(problems)
        planned = run.get("planned_cells", [])
        planned = len(planned) if isinstance(planned, list) else "?"
        cfg_run = config.get("run", {})
        quality = cfg_run.get("quality", "legacy/unverified") if isinstance(cfg_run, dict) else "invalid"
        runs.rows.append([link(path, root, path.parent.name), run.get("suite", "?"),
                          f"{run.get('table_am', '?')} / {run.get('build') or 'default'}", run.get("started", "?"),
                          quality, run.get("state", "unverified"), f"{planned} / {counts}", status,
                          link(path.parent / "index.html", root, "HTML") if (path.parent / "index.html").exists() else "not generated",
                          link(path.parent / "README.md", root, "Markdown") if (path.parent / "README.md").exists() else "not generated"])
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
