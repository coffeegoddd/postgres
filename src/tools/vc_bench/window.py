"""The history window a run measures in, and the checks that keep it there.

Version-control operations are measured inside the history window, where
every commit the run builds keeps a root of its pages: a move to one gives
the pages back instead of writing rows.  An engine with a window
(Engine.window_settings()) is set up so that the window holds every commit
a run builds, at every size.  Before each operation its target commit is
checked: in the window, and kept with a root (or with one on the nearest
commit before it, within the root interval).  After it, the engine's path
counters must show the root path: no rows written for want of a root, no
restore given up for a lock, no read refused (or rows left out) for
history past the horizon.  A failed check fails the run; it never becomes
a slow sample.

A target outside the window is measured, if at all, in a group of its own
(window "past"), never with the window's.  An engine that keeps all of its
history locally (Dolt, Doltgres) has no window: its targets are all "in".
"""

# The window's settings: a time span much longer than any run, so a fixture
# prepared long before it is measured is still in it, and the newest commits
# a branch keeps however old, at least every commit a fixture builds.
DEFAULT_TIME_WINDOW = "100 years"

# pg_stat_prolly's counters an operation's path is told by.
PATH_COUNTERS = ("moves_root_kept", "moves_root_nearest", "moves_root_missing",
                 "move_tables_written", "restores_lock_busy", "severed_refusals")

# Operations that move the session's branch to their target (its pages
# given back from a root), and those that only read history or name it.
MOVES = ("reset_hard", "checkout")


class WindowError(RuntimeError):
    """A target that isn't where the run measures: outside the window, or
    without a root."""


class PathError(RuntimeError):
    """An operation that didn't take the root path."""


def covered_commits(c):
    """The commits a fixture's history holds: the baseline and one per
    revision, up to the deepest history measured."""
    return max(c["history_depths"]) + 1


def settings(c):
    """The window, as database settings (name: value), that holds all the
    history a run builds: the configured span (config history_window), the
    newest covered_commits() commits kept whatever their age, and a root at
    every commit."""
    w = c.get("history_window", {})
    min_commits = int(w.get("min_commits", covered_commits(c)))
    if min_commits < covered_commits(c):
        raise ValueError(f"history_window.min_commits {min_commits} keeps fewer than the "
                         f"{covered_commits(c)} commits a fixture builds")
    interval_commits = int(w.get("root_interval_commits", 1))
    if interval_commits < 1:
        raise ValueError("history_window.root_interval_commits must be at least 1")
    return {"prolly.history_window": str(w.get("time", DEFAULT_TIME_WINDOW)),
            "prolly.history_min_commits": str(min_commits),
            "prolly.history_root_max_size": "",
            "prolly.root_interval": str(w.get("root_interval_ms", 0)),
            "prolly.root_interval_commits": str(interval_commits)}


def local_target():
    """A target of an engine without a window: all its history is here."""
    return {"window": "in", "root": None, "root_distance": None}


def classify(chain, index, in_window, has_root, interval_commits, allow_past=False):
    """Where chain[index] (chain: a fixture's commits, oldest first, each the
    first parent of the next) is measured: {"window": "in" or "past",
    "root": "kept" (its own), "nearest" (one on a commit before it, within
    the interval) or None, "root_distance": commits back to it}.  An
    in-window target with no root within the interval is an error; so is
    one past the window, unless allow_past (then measured apart)."""
    commit = chain[index]
    if not in_window(commit):
        if not allow_past:
            raise WindowError(f"target {commit} is outside the history window; the run builds "
                              "only history the window must hold (prepare again with its settings)")
        return {"window": "past", "root": None, "root_distance": None}
    for back in range(0, min(interval_commits, index + 1)):
        if has_root(chain[index - back]):
            return {"window": "in", "root": "kept" if back == 0 else "nearest", "root_distance": back}
    raise WindowError(f"target {commit} is in the window but neither it nor the "
                      f"{interval_commits - 1} commits before it keep a root")


def check_path(operation, distance, target, deltas):
    """Raise PathError unless an operation on target (classify()'s), its
    path counters' growth deltas, took the root path.  A target past the
    window is measured apart and isn't held to it."""
    if target is None or target["window"] != "in" or target["root"] is None:
        return
    got = {name: deltas.get(name, 0) for name in PATH_COUNTERS}
    where = f"{operation} D={distance}"
    if got["severed_refusals"]:
        raise PathError(f"{where}: refused, or left rows out, as history past the horizon")
    if got["restores_lock_busy"]:
        raise PathError(f"{where}: gave up a restore for a lock and wrote rows instead")
    if got["moves_root_missing"]:
        raise PathError(f"{where}: moved to a commit without a root and wrote its rows")
    if operation not in MOVES or target["root"] == "kept":
        if got["move_tables_written"]:
            raise PathError(f"{where}: wrote rows ({got['move_tables_written']} tables) instead of "
                            "giving back its root's pages")
    if operation == "reset_hard" and distance:
        need = "moves_root_kept" if target["root"] == "kept" else "moves_root_nearest"
        if not got[need]:
            raise PathError(f"{where}: no move gave back the pages kept as of its target ({need} is 0)")
