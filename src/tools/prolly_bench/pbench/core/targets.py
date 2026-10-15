"""Targets: how a run puts its tables on the table AM under test.

Setting default_table_access_method is enough for every AM except prolly,
whose tables exist only in versioned schemas: with prolly as the default, a
table created anywhere else is quietly a heap table.  A prolly target
therefore creates its tables in a versioned schema.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Optional

# Table AMs whose tables must be in a versioned schema.
VERSIONED_ONLY = {"prolly"}

# The versioned schema a target creates its tables in.
VERSIONED_SCHEMA = "versioned"


@dataclass(frozen=True)
class Target:
    table_am: str
    # A versioned schema to create the tables in, or None for the default
    # (public).
    schema: Optional[str] = None


def for_am(table_am: str) -> Target:
    return Target(table_am, VERSIONED_SCHEMA if table_am in VERSIONED_ONLY else None)
