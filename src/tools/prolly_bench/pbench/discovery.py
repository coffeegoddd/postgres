"""Select saved runs from manifest headers before loading execution evidence."""
from pathlib import Path

from pbench.reports import read_json


def header(path: Path) -> dict:
    return read_json(path / "run.json", [])


def latest(root: Path, suite: str) -> list[Path]:
    """Latest per label, retaining failed and incomplete attempts."""
    selected = {}
    for path in sorted(root.glob("*/run.json")):
        run = header(path.parent)
        if run.get("suite") != suite:
            continue
        label = str(run.get("label", path.parent.name))
        started = str(run.get("started", ""))
        if label not in selected or started > selected[label][0]:
            selected[label] = (started, path.parent)
    return [path for _, path in selected.values()]
