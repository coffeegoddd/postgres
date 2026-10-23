"""Suites: what each suite letter means to the harness.

A suite registers itself once, in a file of pbench/suites/, and everything
that depends on its letter reads the registration: the command line's
choices, the default configuration, the [sizes] and per-workload keys a
configuration may use, whether the durability settings are required,
which engines it accepts, which extensions its build and databases need,
and how its reports are laid out.  Adding a suite is adding a file.
"""

from __future__ import annotations

import importlib
import pkgutil
import re
from dataclasses import dataclass, field

# Report layouts.  A storage suite is any table AM against the same build's
# heap (B0); a version-control suite measures engines with history, against
# bounds rather than heap; a correctness suite compares answers with heap.
STORAGE, VERSION_CONTROL, CORRECTNESS = "storage", "version_control", "correctness"
LAYOUTS = (STORAGE, VERSION_CONTROL, CORRECTNESS)


@dataclass(frozen=True)
class Suite:
    letter: str
    title: str
    layout: str
    # The configuration a run uses without --config, in configs/.
    default_config: str
    # Only engines with version control (targets.VERSIONED_HISTORY).
    history_only: bool = False
    # Runs must have fsync and synchronous_commit on.  A suite that
    # measures answers rather than speed, and never crashes the server on
    # purpose, may run without.
    durable: bool = True
    # The environment's performance warnings (CPU governor, build flags,
    # filesystem) apply.
    measures_performance: bool = True
    # The [sizes] keys its workloads read.
    size_keys: frozenset[str] = frozenset()
    # Per-workload parameter keys, beyond the ones every workload accepts.
    workload_params: dict[str, frozenset[str]] = field(default_factory=dict)
    # Workloads that accept a fixed transaction count, and an offered rate.
    transaction_workloads: frozenset[str] = frozenset()
    rate_workloads: frozenset[str] = frozenset()
    # Extensions its builds must carry and its databases install
    # (build_postgres.py --with-NAME, targets.Target).
    extensions: tuple[str, ...] = ()
    # Data manifests (files under configs/) whose hashes every run records.
    data_manifests: tuple[str, ...] = ()
    # Where it is listed among the suites (help, `list`).
    order: int = 100

    def __post_init__(self):
        if not re.fullmatch(r"[A-Z]{1,3}", self.letter):
            raise ValueError(f"suite letter {self.letter!r} must be one to three capitals")
        if self.layout not in LAYOUTS:
            raise ValueError(f"suite {self.letter}: unknown layout {self.layout!r}")
        for wid in self.workload_params:
            if workload_number(wid, self.letter) is None:
                raise ValueError(f"suite {self.letter}: {wid!r} is not one of its workloads")

    def owns(self, wid: str) -> bool:
        """Whether wid names one of this suite's workloads ("G3" is G's,
        not GV's)."""
        return workload_number(wid, self.letter) is not None


def workload_number(wid: str, letter: str) -> int | None:
    m = re.fullmatch(re.escape(letter) + r"([1-9][0-9]*)", wid)
    return int(m.group(1)) if m else None


_suites: dict[str, Suite] = {}
_loaded = False


def register(suite: Suite) -> Suite:
    if suite.letter in _suites:
        raise ValueError(f"duplicate suite {suite.letter!r}")
    _suites[suite.letter] = suite
    for letter in sorted(_suites, key=lambda k: (_suites[k].order, k)):
        _suites[letter] = _suites.pop(letter)
    return suite


def load() -> None:
    """Import every file of pbench/suites/, each registering its suites."""
    global _loaded
    if _loaded:
        return
    _loaded = True
    pkg = importlib.import_module("pbench.suites")
    for mod in pkgutil.iter_modules(pkg.__path__):
        if not mod.name.startswith("_"):
            importlib.import_module(f"pbench.suites.{mod.name}")


def letters() -> list[str]:
    load()
    return list(_suites)


def all_suites() -> list[Suite]:
    load()
    return list(_suites.values())


def get(letter: str) -> Suite:
    load()
    try:
        return _suites[letter]
    except KeyError:
        raise KeyError(f"unknown suite {letter!r} (known: {', '.join(_suites)})") from None


def layout(letter: str | None) -> str | None:
    """A suite's report layout, or None for an unknown or missing letter
    (evidence from elsewhere)."""
    load()
    suite = _suites.get(letter or "")
    return suite.layout if suite else None


def of_workload(wid: str) -> Suite | None:
    """The suite a workload ID belongs to."""
    load()
    return next((s for s in _suites.values() if s.owns(wid)), None)


def workload_order(wid: str) -> tuple:
    """Sort key for workload IDs: by suite registration, then number."""
    load()
    for i, s in enumerate(_suites.values()):
        n = workload_number(wid, s.letter)
        if n is not None:
            return (i, n, "")
    return (len(_suites), 0, wid)


def is_workload_id(wid: object) -> bool:
    return isinstance(wid, str) and of_workload(wid) is not None


def size_keys() -> frozenset[str]:
    """Every registered suite's [sizes] keys: one configuration may serve
    several suites."""
    load()
    return frozenset().union(*(s.size_keys for s in _suites.values()))


def workload_params() -> dict[str, frozenset[str]]:
    load()
    out: dict[str, frozenset[str]] = {}
    for s in _suites.values():
        out.update(s.workload_params)
    return out


def transaction_workloads() -> frozenset[str]:
    load()
    return frozenset().union(*(s.transaction_workloads for s in _suites.values()))


def rate_workloads() -> frozenset[str]:
    load()
    return frozenset().union(*(s.rate_workloads for s in _suites.values()))
