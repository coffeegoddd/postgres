# Working set ⇄ prolly tree lifecycle

How a schema's Postgres working set and its prolly-tree history stay in step,
which artifacts exist, and when.  The aim: cheap version-control operations
for any commit inside an arbitrarily sized window.

## Pieces

| Piece | What it is | Lifetime |
|---|---|---|
| Working set | The schema's heap and indexes, mutable, plain SQL | Always |
| Change stream | One logical-decoding slot per versioned schema, read by one persistent streaming decoder | Always |
| Prolly tree | Commits, plus a *working root* per branch | Commits: per the window; working roots: always |
| Keyframes | Commit images (page images + page map) at selected commits | Local only, inside the window, never exported |
| Window | Incremental head coverage, sever into segments, export or discard | Always |

Only keyframes are optional.  Everything else is required.

## Lifecycle

1. **Writes.** SQL changes pages as usual.  Version control adds no work to
   individual writes; the standing costs are logical-level WAL, plus hint
   logging (data checksums or `wal_log_hints`) when keyframes are used.

2. **Checkpoint.**  The streaming decoder parses committed changes as they
   arrive and applies whole transactions to the branch's working root in
   batches, by row count or time (background checkpoints), confirming its
   position so the slot advances.  An on-demand checkpoint (commit, diff,
   status, checkout) is a *barrier*: a non-transactional logical message
   with a fresh token; when the stream reaches it, every transaction whose
   commit record precedes it has been applied.  Cost: O(rows since the last
   background apply).  No WAL is decoded twice.

3. **Dolt commit.**  No lock, and writers are never blocked.  The caller
   commits its own SQL transaction, then: barrier, commit chunk, return the
   hash.  "Synchronous" only means the caller waits for its commit's
   contents to reach the prolly tree.
   - The commit holds exactly the working root as of the barrier: every
     transaction whose commit record precedes the barrier in the WAL, each
     all or nothing.  Transactions committing later land in the working
     root afterwards, like uncommitted Dolt working-set changes, and the
     branch's working root carries on.
   - That cut follows WAL commit order: exactly what a replica replaying the
     same WAL shows.  In rare corner cases it can differ from what an MVCC
     snapshot taken at the same instant sees, because a commit record is
     written slightly before the transaction becomes visible to new
     snapshots.
   - Then the keyframe decision (below).  A keyframe is captured under an
     MVCC snapshot taken at the barrier, so it does not block writers
     either; if none is cut, commit does nothing physical.

4. **diff, status, log, merge base.**  Checkpoint, then prolly tree only:
   O(d log n) against any commit in the window.  Diff sees committed SQL
   state; a session's own uncommitted changes appear once it commits.

5. **Checkout, reset, branch switch** (move the working set to commit X).
   A planner picks the cheapest of:
   - **Row replay** (default): apply the prolly diff working root → X as DML,
     O(d × (1 + indexes)), under a replication origin so the change stream
     skips it.
   - **Keyframe + replay**: restore the nearest keyframe K, O(pages differing
     between the working set and K), then replay K → X.  For diffs that are
     large relative to the pages they touch: bulk changes, many indexes,
     distant targets.
   - **Full rebuild** from the prolly tree: for very large diffs or targets
     outside the window.

   Afterwards the working root is X's root and the slot is advanced.  The
   working set's physical base becomes K if a keyframe was used; otherwise
   it keeps its base plus what replay dirtied.

6. **Merge, cherry-pick, revert.**  Computed in the prolly tree, conflicts
   recorded there, result applied to the working set by row replay.

7. **Window.**  Commits and keyframes are held inside it.  Severing detaches
   commits into segments and releases their keyframes; segments are exported
   to a remote or archive, or discarded.

8. **Remotes.**  Prolly tree only: push, fetch, clone, attachment rules,
   boundaries and hints.

9. **Outside the window.**  Attach or deepen first, then rebuild from the
   prolly tree, or keyframe + replay if a keyframe is near.

10. **Crash recovery.**  The working set is durable through Postgres; the slot
    position and *working root = working set as of L* are durable; decoding
    replays from the slot.

## Keyframe policy

- **Default: none.**  Prolly tree + change stream covers commit, diff, merge,
  remotes, and most checkouts by row replay.
- **Keyframes where measurement says they pay:** at branch heads (the latest
  per branch: switching heads is the most common move), and along history
  each time *R* rows have changed since the last keyframe, where *R* is the
  measured point at which restoring pages beats replaying rows.  If replay
  always wins once restore's constant factor is fixed, *R* = ∞ and there are
  no keyframes.
- **Restoring a keyframe is exact only if** every page change after its base
  point reaches the WAL and is read: hint changes logged, a checkpoint at the
  base point, and the WAL flushed before the changed pages are read
  (`bench/lp_dead_repro.sql` shows each failure).
- **Longer term:** copy-on-write page storage, where the working set's pages
  live in the page store, makes every commit a free keyframe and checkout
  O(1), at the price of a custom storage manager.

## Evidence so far

From `bench/winbench` against a real server (2,000 random row changes per
commit, two indexes) and `bench/branchbench` at the engine level.

| Measure | Result |
|---|---|
| Keyframe checkout, one commit away | 4,193 / 4,355 / 4,471 pages at 196 MB / 1.7 GB / 17 GB: follows the change, not the size |
| Keyframe restore cost (harness, via libpq, full-page WAL) | ~150 µs per page |
| Row replay, ~9,000 rows | 114 / 153 / 221 ms at the same sizes: currently ~12× cheaper than restore |
| Branches, 1k → 1M | create 2–8 µs, commit ~108 µs, image ~130 µs, sever 0.2 µs: flat |
| Switch between random heads | grows with the heads' distance, not with branch count |
| Correctness | every checkout verified row by row against the prolly tree, with amcheck and index-driven counts |

### Step 1: the lifecycle without keyframes

`bench/winbench --lifecycle` on 10M rows (1.8 GB), 2,000 random row changes
per commit, four branches; medians, every checkout verified row by row.

| Operation | Work | Time |
|---|---|---|
| Dolt commit | 2,000-row tail | 123 ms (checkpoint 114 ms) |
| diff WORKING X | 2,000-row tail, ~36k rows differ | 220 ms (checkpoint 152 ms, prolly diff 69 ms) |
| Branch switch | ~16k rows replayed | 233 ms (replay 166 ms) |
| Reset to a commit in the window | ~8k rows replayed | 162 ms (replay 70 ms) |
| Background checkpoints | 34 during the run | mean 61 ms, max 131 ms |

At 10× the data (100M rows, 17 GB, past the 16 GB buffer pool), same
workload, every checkout verified:

| Operation | 10M rows (1.8 GB) | 100M rows (17 GB) | Growth for 10× data |
|---|---|---|---|
| Dolt commit | 123 ms | 180 ms | 1.5× |
| diff WORKING X (~36k rows) | 220 ms (prolly diff 69 ms) | 298 ms (prolly diff 123 ms) | 1.4× (diff 1.8×) |
| Branch switch (~16k rows) | 233 ms (8.1 µs/row) | 502 ms (10.6 µs/row) | 2.2× |
| Reset (~8k rows) | 162 ms (8.6 µs/row) | 212 ms (11.4 µs/row) | 1.3× |
| Background checkpoint | mean 61 ms | mean 136 ms | 2.2× |

Costs follow the change, with sublinear growth from the taller trees and
from the table no longer fitting in the buffer pool (random row replay
starts to miss), not O(data).

With the persistent streaming decoder in place of the SQL function (same
workload; every checkout verified):

| | Commit | diff WORKING X | Branch switch | Reset |
|---|---|---|---|---|
| 10M, SQL decoding | 123 ms | 220 ms | 233 ms | 162 ms |
| 10M, stream | 71 ms | 138 ms | 171 ms | 75 ms |
| 100M, SQL decoding | 180 ms | 298 ms | 502 ms | 212 ms |
| 100M, stream | 74 ms | 207 ms | 431 ms | 99 ms |

Commit no longer grows with the data.  Background applies absorb most of
the tail (700–800 rows left for the commit's barrier instead of 2,000), and
an empty checkpoint costs 1–2 ms.  What still grows is the prolly diff
(taller tree) and replay once the table outgrows the buffer pool.

**Commits under concurrent writers.**  `winbench --lifecycle --concurrent`
runs a writer committing 50-row transactions nonstop while 40 Dolt commits
are taken, holding each commit back 150 ms after its barrier so later
transactions land in the working root first.  Every commit held exactly a
prefix of the writer's transactions: whole transactions only, everything
committed before the commit began, nothing from after its barrier
(8,248 writer transactions).  Committing the working root at lock time,
instead of the root as of the barrier (`vc_commit_root`), fails the last
check.

**Inside vs outside the window.**  `winbench --lifecycle --outside` on 10M
rows, 2,000 row changes per commit, 45 commits; the same targets with a
10-commit window (severed history exported to an archive store, a local
file here) and with a 60-commit window (everything inside); every checkout
verified.

| Commits back | diff, inside | diff, outside | checkout, inside | checkout, outside | Brought back from the archive |
|---|---|---|---|---|---|
| 5 | 28 ms | (inside) | 162 ms | (inside) | — |
| 15 | 63 ms | 555 ms | 343 ms | 835 ms | 11.6k chunks, 64 MB, 492 ms |
| 20 | 78 ms | 484 ms | 352 ms | 788 ms | 10.0k chunks, 54 MB, 406 ms |
| 30 | 109 ms | 820 ms | 530 ms | 1,196 ms | 17.8k chunks, 98 MB, 711 ms |
| 40 | 137 ms | 823 ms | 644 ms | 1,298 ms | 17.8k chunks, 98 MB, 693 ms |
| Full rebuild of the table from the tree | | | | 30.9 s | |

- Outside the window the only extra is bringing the target back from the
  archive: the chunks that version does not share with what is still
  local.  Diff and replay then cost what they do inside.
- That makes diff ~6–9× slower and checkout ~2× (replay is a larger share
  of a checkout).  The cost follows how far the old version has drifted
  from local history, not the table's size, and bringing one old commit
  back makes nearby ones cheaper (they share chunks).
- A full rebuild, what is left without a diff path, is 40–100× the
  inside-the-window cost.
- With a remote archive, only the bring-back part changes: it becomes
  bandwidth and round trips, ~100 MB per bring-back at these distances.

- Replay costs ~8 µs per row against ~150 µs per page for keyframe restore
  as built (~2 pages per row here): ~30× cheaper per change on this
  workload.
- Checkpoints dominate.  The same 2,000-row tail took ~45 ms at 200k rows
  and ~114 ms at 10M: partly the taller tree, mostly the SQL decoding
  function re-reading WAL from the slot's restart point on every call.  A
  persistent streaming decoder should remove most of it.

## Next

1. **The lifecycle without keyframes:** background and on-demand checkpoints,
   Dolt commit, checkpoint-then-diff, row-replay checkout under a replication
   origin; measured in the harness.
2. **Keyframes and the planner,** with a server-side restore for a fair
   cost, and a workload matrix (locality, index count, diff size, distance)
   to find *R*, or show keyframes are not worth keeping.
3. **Decide on copy-on-write page storage.**
