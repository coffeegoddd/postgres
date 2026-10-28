# Working set ⇄ prolly tree lifecycle

How a schema's Postgres working set and its prolly-tree history stay in step,
which artifacts exist, and when.  The aim: cheap version-control operations
for any commit inside an arbitrarily sized window.

## Pieces

| Piece | What it is | Lifetime |
|---|---|---|
| Working set | The schema's heap and indexes, mutable, plain SQL | Always |
| Change stream | One logical-decoding slot per versioned schema | Always |
| Prolly tree | Commits, plus a *working root* per branch | Commits: per the window; working roots: always |
| Keyframes | Commit images (page images + page map) at selected commits | Local only, inside the window, never exported |
| Window | Incremental head coverage, sever into segments, export or discard | Always |

Only keyframes are optional.  Everything else is required.

## Lifecycle

1. **Writes.** SQL changes pages as usual.  Version control adds no work to
   individual writes; the standing costs are logical-level WAL, plus hint
   logging (data checksums or `wal_log_hints`) when keyframes are used.

2. **Checkpoint.**  Triggered in the background by time or by the size of the
   undecoded tail, and on demand by commit, diff, status and checkout.
   Decodes committed changes up to WAL position L, applies them to the
   branch's working root, and records *working root = working set as of L*.
   Cost: O(rows since the last checkpoint), kept bounded by the triggers.

3. **Dolt commit.**  A synchronous checkpoint, then the commit chunk; the
   hash is known on return.  Then the keyframe decision (below): if one is
   cut, the pages changed since the previous keyframe's base point are
   captured on top of it, O(changed pages).  Otherwise commit does nothing
   physical.

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
