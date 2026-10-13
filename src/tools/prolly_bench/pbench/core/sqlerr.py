"""Classifying SQL errors reported by psql (with VERBOSITY=verbose)."""

from __future__ import annotations

import re

from pbench.core.interfaces import NotImplementedByEngine, SqlResult

# psql prefixes errors with "psql:<file>:<line>: " when reading a script.
SQLSTATE_RE = re.compile(r"(?:^|: )(?:ERROR|FATAL|PANIC):\s+([0-9A-Z]{5}):\s*(.*)$",
                         re.M)

# ERRCODE_FEATURE_NOT_SUPPORTED: what every prolly stub raises.
FEATURE_NOT_SUPPORTED = "0A000"


def sqlstate_of(stderr: str) -> str | None:
    m = SQLSTATE_RE.search(stderr)
    return m.group(1) if m else None


def raise_for_sql_error(res: SqlResult, sql: str) -> None:
    if res.ok:
        return
    m = SQLSTATE_RE.search(res.stderr)
    msg = m.group(2) if m else res.stderr.strip()
    if res.sqlstate == FEATURE_NOT_SUPPORTED:
        raise NotImplementedByEngine(msg)
    raise RuntimeError(f"SQL failed ({res.sqlstate or 'no SQLSTATE'}): {msg}\n"
                       f"statement: {sql.strip()[:500]}")
