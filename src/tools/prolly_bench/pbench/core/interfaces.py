"""The harness's interfaces.

Everything the driver works with is one of these abstract classes, found by
name through a registry (registry.py).  Adding a workload, collector, sink or
cluster provider means writing a new implementation module; the driver does
not change.
"""

from __future__ import annotations

import abc
import enum
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, ClassVar, Optional


class Status(str, enum.Enum):
    """Outcome of one cell, or availability of one metric."""

    OK = "ok"
    # The engine under test raised ERRCODE_FEATURE_NOT_SUPPORTED (0A000).
    NOT_IMPLEMENTED = "not_implemented"
    # A tool or facility the measurement needs is missing (fio, perf, ...).
    UNAVAILABLE = "unavailable"
    # Does not apply to this table AM (e.g. engine counters for heap).
    NOT_APPLICABLE = "not_applicable"
    # Anything else went wrong: a harness or database failure.
    ERROR = "error"


class NotImplementedByEngine(Exception):
    """The engine raised SQLSTATE 0A000; reported as a result, not a failure."""


class Unavailable(Exception):
    """A needed tool is missing; reported as a result, not a failure."""


class NotApplicable(Exception):
    """The measurement does not apply here (e.g. engine counters on heap)."""


# ---------------------------------------------------------------------------
# Cluster
# ---------------------------------------------------------------------------

@dataclass
class SqlResult:
    ok: bool
    stdout: str
    stderr: str
    sqlstate: Optional[str] = None   # SQLSTATE of the first error, if any


class Cluster(abc.ABC):
    """A Postgres instance the harness can create, start, crash and query.

    The local implementation runs initdb/pg_ctl on this machine.  Other
    implementations (another device, a remote host) plug in here, so the
    storage target can vary without changing workloads.
    """

    name: ClassVar[str]

    data_dir: Path
    log_file: Path

    @abc.abstractmethod
    def create(self, settings: dict[str, str]) -> None:
        """initdb a fresh, empty cluster with settings in postgresql.conf."""

    @abc.abstractmethod
    def start(self) -> None: ...

    @abc.abstractmethod
    def stop(self, mode: str = "fast") -> None:
        """Stop; mode "immediate" simulates a crash."""

    @abc.abstractmethod
    def wait_ready(self, timeout: float = 600.0) -> float:
        """Wait until the server accepts connections; returns seconds waited."""

    @abc.abstractmethod
    def destroy(self) -> None: ...

    @abc.abstractmethod
    def sql(self, sql: str, *, dbname: str = "postgres",
            options: Optional[dict[str, str]] = None,
            check: bool = True) -> SqlResult:
        """Run SQL through psql.  With check, raise on error: NotImplemented-
        ByEngine for SQLSTATE 0A000, RuntimeError otherwise."""

    @abc.abstractmethod
    def scalar(self, sql: str, *, dbname: str = "postgres") -> str: ...

    @abc.abstractmethod
    def client_env(self, options: Optional[dict[str, str]] = None) -> dict[str, str]:
        """Environment for client programs (PGHOST, PGPORT, PGOPTIONS...)."""

    @abc.abstractmethod
    def jdbc_url(self, dbname: str, options: Optional[dict[str, str]] = None) -> str: ...

    @abc.abstractmethod
    def libpq_url(self, dbname: str) -> str:
        """A postgresql:// URL other instances can use to reach this one."""

    @abc.abstractmethod
    def conninfo(self) -> str:
        """A libpq keyword connection string for this instance, without a
        database name, over the fastest local path."""

    @abc.abstractmethod
    def bin(self, program: str) -> str:
        """Path of a Postgres client program (psql, pgbench, ...)."""

    @abc.abstractmethod
    def log_position(self) -> int: ...

    @abc.abstractmethod
    def log_since(self, position: int) -> str: ...


# ---------------------------------------------------------------------------
# Context passed to workloads and collectors
# ---------------------------------------------------------------------------

@dataclass
class CellKey:
    suite: str
    workload: str
    size: str
    variant: str          # "" when the workload has one variant
    clients: int
    rep: int
    # The server build the cell ran ("base", "candidate"), when a run
    # compares builds; "" otherwise.
    build: str = ""

    def pair_path(self) -> Path:
        """The cell, whatever build and engine: cells with the same one are
        run as a group, in random order, and compared with each other."""
        parts = [self.workload, self.size]
        if self.variant:
            parts.append(self.variant)
        parts += [f"c{self.clients}", f"r{self.rep}"]
        return Path(*parts)

    def path(self) -> Path:
        return Path(self.build, self.pair_path()) if self.build else self.pair_path()


@dataclass
class Context:
    key: CellKey
    cluster: Cluster
    table_am: str
    size_params: dict[str, Any]      # [sizes.<name>] from the config
    params: dict[str, Any]           # [workloads.<id>] plus the variant
    warmup_seconds: int
    measure_seconds: int
    raw_dir: Path                    # kept with the results
    # Per-cell scratch space in RAM (tmpfs), deleted after the cell: for
    # large intermediate output written during the window (latency logs,
    # perf data), so it neither lands on the device under test nor in the
    # results.
    scratch_dir: Path
    tools: dict[str, str]            # [tools] from the config
    dbname: str = "bench"
    # The workload's tables (Workload.tables), set after prepare.
    tables: list[str] = field(default_factory=list)
    # Filled in by the cluster factory, for workloads needing a second
    # instance (V4).
    make_cluster: Optional[Callable[[str], Cluster]] = None
    # Other instances the measured work writes to (V4's clone), added by
    # the workload before it opens the window.  The runner destroys them
    # after the collectors, which see their writes too (write_amp).
    peers: list[Cluster] = field(default_factory=list)

    def session_options(self) -> dict[str, str]:
        """GUCs every client session of this run uses."""
        return {"default_table_access_method": self.table_am}


@dataclass
class Outcome:
    """What a workload's measured section produced."""

    metrics: dict[str, Optional[float]] = field(default_factory=dict)
    # Number of transactions (or operations) measured, for per-txn metrics.
    transactions: Optional[float] = None
    # What one of those is ("transaction", "row", "operation").
    unit: str = "transaction"
    # CPU used by client programs during the window, to separate server CPU.
    client_cpu_seconds: Optional[float] = None
    # True when the measured work writes nothing, so write-volume metrics
    # (WAL and device bytes per transaction) do not apply.
    read_only: bool = False
    notes: list[str] = field(default_factory=list)


class Window(abc.ABC):
    """The measured section.  Workloads enter it exactly around the work
    being measured; collectors snapshot on entry and exit.  After exit,
    seconds is the time spent inside."""

    seconds: float = 0.0

    @abc.abstractmethod
    def __enter__(self) -> "Window": ...

    @abc.abstractmethod
    def __exit__(self, *exc) -> None: ...


# ---------------------------------------------------------------------------
# Workload
# ---------------------------------------------------------------------------

class Workload(abc.ABC):
    """One S, V or C workload."""

    id: ClassVar[str]               # "S1", "V3", ...
    suite: ClassVar[str]            # "S", "V" or "C"
    title: ClassVar[str]

    def variants(self, params: dict[str, Any]) -> list[tuple[str, dict[str, Any]]]:
        """(label, extra params) per variant; one unnamed variant by default."""
        return [("", {})]

    def client_counts(self, configured: list[int],
                      variant: dict[str, Any]) -> list[int]:
        """Client counts to sweep; single-session workloads return [1]."""
        return configured

    def check(self, ctx: Context) -> None:
        """Raise Unavailable if a needed tool is missing."""

    @abc.abstractmethod
    def prepare(self, ctx: Context) -> None:
        """Create and load data in the fresh cluster.  Not measured."""

    def warmup(self, ctx: Context) -> None:
        """Run unmeasured warm-up work.  Default: nothing."""

    @abc.abstractmethod
    def measure(self, ctx: Context, window: Window) -> Outcome:
        """Do the measured work, entering window around it."""

    def tables(self, ctx: Context) -> list[str]:
        """Tables holding the workload's data, for space measurements."""
        return []

    def verify(self, ctx: Context) -> None:
        """Optional post-measurement answer verification; never timed."""


# ---------------------------------------------------------------------------
# Collector
# ---------------------------------------------------------------------------

class Collector(abc.ABC):
    """One metric source.

    A new instance is made for every cell.  start and stop bracket the
    measured window and should only take snapshots; results then turns the
    snapshots into metrics, once the workload's Outcome (e.g. its
    transaction count) is known.  before_window and after_window bracket
    every collector's start and stop, for a collector whose interval is
    wider than the window.  post runs after the window, in ascending
    post_order, for measurements that act on the cluster (VACUUM, crash
    recovery).  Raising Unavailable, NotImplementedByEngine or NotApplicable
    from any hook marks the collector's metrics with that status instead of
    failing the cell.
    """

    name: ClassVar[str]
    post_order: ClassVar[int] = 0

    def check(self, ctx: Context) -> None:
        """Raise Unavailable or NotApplicable if the collector can't run."""

    def before_window(self, ctx: Context) -> None:
        """Act on the cluster before any collector's start (write_amp's
        checkpoint), so no other collector's interval includes it."""

    def start(self, ctx: Context) -> None: ...

    def stop(self, ctx: Context) -> None: ...

    def after_window(self, ctx: Context, outcome: Outcome) -> None:
        """Act on the cluster after every collector's stop and before any
        results or verification (write_amp's flush): the post-run checks
        read the tables, and reading them can write (hint bits)."""

    def results(self, ctx: Context, outcome: Outcome) -> dict[str, Optional[float]]:
        return {}

    def post(self, ctx: Context, outcome: Outcome) -> dict[str, Optional[float]]:
        return {}


# ---------------------------------------------------------------------------
# Results and sinks
# ---------------------------------------------------------------------------

@dataclass
class CellResult:
    key: CellKey
    status: Status
    message: str = ""
    metrics: dict[str, Optional[float]] = field(default_factory=dict)
    # Per collector: status and reason when not Status.OK.
    collector_status: dict[str, tuple[str, str]] = field(default_factory=dict)
    seconds: float = 0.0
    raw_dir: str = ""
    read_only: bool = False
    notes: list[str] = field(default_factory=list)
    validation_errors: list[str] = field(default_factory=list)
    unit: str = "transaction"
    denominator: Optional[float] = None
    measurement: dict[str, Any] = field(default_factory=dict)


@dataclass
class RunInfo:
    suite: str
    label: str
    table_am: str
    started: str
    command: str
    config: dict[str, Any]
    environment: dict[str, Any]
    warnings: list[str]
    out_dir: Path
    planned_cells: list[CellKey] = field(default_factory=list)
    build: str = ""                  # the build compared, if builds are
    builds: list[str] = field(default_factory=list)   # all of them, the base first


class Sink(abc.ABC):
    """Where results go."""

    name: ClassVar[str]

    @abc.abstractmethod
    def open(self, run: RunInfo) -> None: ...

    @abc.abstractmethod
    def cell(self, result: CellResult) -> None: ...

    @abc.abstractmethod
    def close(self, run: RunInfo, results: list[CellResult]) -> None: ...
