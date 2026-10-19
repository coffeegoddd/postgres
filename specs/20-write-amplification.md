# 20. Write amplification in the S and V suites

**Status: decided by the user 2026-10-19, as recommended, on `39de7462e5`.
Built in the `s-suite/wa-*` stack (#417–#421), which is in review. Part 3
describes what the stack builds. Where that differs from the first
proposal, the difference is noted with its reason.**

Every benchmark result reports write amplification for both engines:
how many bytes the server wrote for each byte of row data the workload
asked it to write. Heap and prolly are split the same way: WAL, data
pages, and (for prolly) the chunk store. Bytes are counted when written,
not when synced.

## 1. What existed

| Recorded per cell (`results.jsonl`) | Collector | Shown by `table` |
|---|---|---|
| `wal_bytes`, `wal_bytes_per_operation` (LSN difference), `wal_records`, `wal_fpi`, `wal_fpi_bytes` | `wal.py` | WAL per unit only |
| `device_write_bytes(_per_operation)`: the whole data device | `diskstats.py` | yes |
| `io_writes`, `io_write_bytes` (relation objects, `pg_stat_io`) | `pg_stat_io.py` | no |
| `engine.chunk_bytes_appended`, `engine.nodes_written`, ... (`pg_stat_prolly`) | `engine_counters.py` | no |
| `relation_bytes`, `chunk_store_bytes`, `row_representation_bytes` (after the window) | `space.py` | relation bytes only |

`06-benchmarking.md` defined write amplification as device bytes per
transaction. No measurement here could compute it as a ratio:

1. **No logical denominator.** Every write metric was bytes per
   transaction, and nothing measured how many bytes of row data were
   written.
2. **Deferred writes fell outside the window.** The window closes when the
   clients exit. Heap's dirty pages reach disk at a later checkpoint.
   Prolly's tree writes are deferred too: plain SQL `COMMIT` writes no
   chunks. Chunks are written only by Dolt commits, `prolly_checkpoint()`,
   checkout, `dolt.transaction_commit = on`, and GC/move. Measured on S1,
   the window itself shows 0 chunk bytes on prolly, and `io_write_bytes`
   is 0 for both engines.
3. **WAL was not split by source.** For prolly, every chunk byte is
   written twice: once in full into WAL (`ProllyChunks` APPEND/FILE
   records, in `logged_write`, `append_reserve` and `table_file_write`),
   then into the journal or a table file.
4. **Chunk-store file writes were counted nowhere.** The journal, table
   files and manifests are written with `pg_pwrite`/`FileWrite`, outside
   bufmgr. `CHUNK_BYTES_APPENDED` counts only payload, at put time.
5. **The block map writes part of its page store itself**, with
   `mdwritev`/`mdzeroextend`, outside bufmgr. `pg_stat_io.py` also left out
   `extend_bytes`.
6. **Reporting** showed only WAL per unit and device writes per unit.

## 2. Definitions

All amounts are summed over the collector's **write-accounting
interval**:

- **Start.** As the window opens. Before it, the collector waits for
  autovacuum workers to exit, has prolly fold its working set, and
  checkpoints.
- **End, for WAL.** After the post-window flush (part 3.2), before its
  final `CHECKPOINT`.
- **End, for pages and the chunk store.** After that checkpoint, and after
  autovacuum workers have exited.

### Logical bytes, L

L is the new row versions the SQL wrote, defined the same way for every
engine:

```
L = Σ over workload tables t:  (Δ tuples_inserted(t) + Δ tuples_updated(t)) × avg_row_bytes(t)
```

- **Row counts.** The Δ counts come from `pg_stat_get_tuples_*()`. Prolly
  tables run heap's code and are counted the same way: on S1, both engines
  count exactly 4 rows written per transaction. The counts include rows
  of aborted transactions, which inflates L where transactions abort (TPC-C
  deadlocks, failed transactions).
- **Row size.** `avg_row_bytes` is `avg(pg_column_size(t.*))` after the
  interval, which is the expression `space.py` uses.
- **Deletes** add no bytes. They are reported as `wa.logical_rows_deleted`.
- **Indexes.** Index bytes are not logical bytes: index maintenance counts
  as amplification.

### Physical components

Each component is in bytes over the interval, and is also reported
`_per_operation`.

| Metric | Heap | Prolly | Source |
|---|---|---|---|
| `wa.wal.row_bytes` | Heap/Heap2 record bytes, without their full-page images | same (shared heap code) | `pg_get_wal_stats` `record_size`, by rmgr |
| `wa.wal.index_bytes` | Btree/Hash/Gin/Gist/SPGist/BRIN | same | same |
| `wa.wal.fpi_bytes` | full-page images, all rmgrs | same, including the block map's copy-on-write images | `fpi_size` |
| `wa.wal.blockmap_bytes` | 0 | `BlockMap` | `record_size` |
| `wa.wal.chunk_bytes` | 0 | `ProllyChunks` | `record_size` |
| `wa.wal.other_bytes` | everything else (Transaction, XLOG, Standby, ...) | same | `record_size` |
| `wa.wal.unattributed_bytes`, `_fraction` | LSN difference no record holds: page headers and alignment, about 0.5%; a segment switch's padding if more | same | |
| `wa.wal.total_bytes` | WAL generated: the LSN difference | same | |
| `wa.pages.io_bytes` | `pg_stat_io` relation `write_bytes + extend_bytes`, **all backend types** | same | `pg_stat_io` |
| `wa.pages.bmap_store_bytes` | n/a | the block map's copy-on-write remaps, which no other counter sees | `pg_stat_prolly` |
| `wa.pages.write_bytes` | `io_bytes` | `io_bytes + bmap_store_bytes` | |
| `wa.pages.bmap_extend_bytes` | n/a | the block map's extends for shared buffers: reported only, because `pg_stat_io` already counts them as extends | `pg_stat_prolly` |
| `wa.pages.bmap_bulk_bytes` | n/a | the block map's bulk writes and copies (`smgrextend`, `smgrpreextend`): reported only, because heap's equivalent writes count nowhere | `pg_stat_prolly` |
| `wa.tree.chunk_payload_bytes` | n/a | `chunk_bytes_appended` | `pg_stat_prolly` |
| `wa.tree.file_bytes` | n/a | bytes of journal, table files and metadata (manifests and collections' dropped-chunk lists) | `pg_stat_prolly` |
| `wa.tree.gc_copied_bytes`, `wa.tree.conjoin_copied_bytes` | n/a | of the table-file bytes, the copies (breakdowns, not additions) | `pg_stat_prolly` |
| `wa.tree.materialize_*`, `wa.wal.materialize_bytes` | n/a | the share the post-window fold wrote | |

### Ratios

The ratios are `None` when L is under 64 KiB or the outcome is read-only.
`table` says which applies.

```
wa_wal          = wa.wal.total_bytes / L
wa_pages        = wa.pages.write_bytes / L
wa_tree         = wa.tree.file_bytes / L              # prolly only
wa_tree_payload = wa.tree.chunk_payload_bytes / L     # tree shape alone
wa_server       = (wa.wal.total_bytes + wa.pages.write_bytes + wa.tree.file_bytes) / L
```

`wa_server` is WAL generated, plus relation pages and chunk-store bytes
written, per logical byte. It is the headline number: the same
accounting for heap and prolly.

It is not every byte the server hands the kernel. It leaves out:

- WAL's partial-page rewrites and segments zero-filled ahead;
- SLRU writes;
- bulk writes (index builds, `CREATE DATABASE` copies), for both engines;
- the block map's extends beyond the blocks `pg_stat_io` counts.

The first proposal claimed "every byte handed to the kernel"; a reviewer
showed it doesn't hold. In the proposal, `wa_tree` also added GC copies,
but copies are part of the table-file bytes. The floor was 1 MiB, but a
counter run's S1 writes 470 kB.

### Measured

S1, scale 5, 4 clients, a 5 s window, one repetition, as built. This is a
smoke run, not a ranking:

| | heap | prolly |
|---|---:|---:|
| logical bytes per transaction | 236 | 236 |
| `wa_wal` | 23.8 | 36.2 |
| `wa_pages` | 23.1 | 38.6 |
| `wa_tree` | n/a | 13.0 |
| `wa_server` | 47.0 | 87.8 |

Prolly's extra amplification has two sources:

- **Chunk WAL.** The `ProllyChunks` records hold the 36 MB of chunks the
  window's fold wrote, and the journal then holds the same bytes again.
- **Copy-on-write remaps.** These are 47 MB: after a fold, a page's first
  write copies it into a new block of the page store.

Read-only S2 writes 8 kB of WAL on heap and 75 kB on prolly, and no
ratios.

The first version folded only after the window. Nothing folds between
the load and the window, so that fold wrote the trees of the whole load:
59 MB of chunks charged to S1's 5 s window, and 23 MB of WAL to read-only
S2.

## 3. What is built

### 3.1 The `write_amp` collector (`pbench/collectors/write_amp.py`)

The collector owns the interval's boundaries through two hooks:

- **`Collector.before_window(ctx)`.** `CollectorWindow.__enter__` calls it
  on every collector before any `start`.
- **`Collector.after_window(ctx, outcome)`.** The runner calls it after
  every `stop`, and before any `results` and before the post-run checks.
  The proposal had the flush in `post`, but `correctness.verify` and
  `workload.verify` run before `post`, and they read the tables. Reading
  can write hint bits, which would be charged to the workload.

What the collector does at each step:

- **`check`.** `CREATE EXTENSION IF NOT EXISTS pg_walinspect`. If that
  fails, the collector is `unavailable`. `build_postgres.py` installs
  `contrib/pg_walinspect` and attests its hash.
- **`before_window`.**
  1. Wait for autovacuum workers to exit.
  2. Fold prolly's working set (`prolly_checkpoint()`, with the same
     setting as part 3.2), so the fold after the window writes only the
     window's changes.
  3. Create a physical slot, if `wal_level` is not `minimal`.
  4. `CHECKPOINT`.

  It doesn't wait for client backends. S6's snapshot holder and the V
  suite's background S1 connect before the window opens. The first
  version waited for them, and every such cell failed.
- **`start`.** Snapshot the insert LSN, `pg_stat_io` (all backend types),
  the tuple counts and `pg_stat_prolly`.
- **`after_window`.**
  1. Wait for the clients and autovacuum workers to exit.
  2. Materialize (part 3.2).
  3. Read the WAL's end LSN and its per-rmgr stats. An interval that wrote
     no WAL is read as no rows, since `pg_get_wal_stats` fails on an empty
     interval.
  4. Drop the slot, in a `finally`, whatever failed. A failure of the
     first checkpoint drops it too.
  5. `CHECKPOINT`, and wait for autovacuum.
  6. Take the end snapshot.
- **`results`.** Compute L, the components and the ratios. Write the rmgr
  rows to `raw/.../write-amp-wal.json`.

**Other instances.** V4 clones, fetches and pushes with a second
instance, and the clone and fetch write there. A workload adds such
instances to `ctx.peers` before it opens the window, and the runner
destroys them after the collectors. Each step above runs on every
instance: the waits, the folds, the slot, the checkpoints, the snapshots
and the WAL read.

- **What's summed.** The WAL, pages and chunk-store bytes of all instances
  are added together. `wa.instances` says how many were counted.
- **Logical bytes** are the origin's: the clone's rows are a copy, not new
  row versions the workload asked for.
- **The first version** read only the origin, and destroyed V4's second
  instance before `after_window` ran. Review measured another 314 kB of
  WAL and 82 kB of chunk-store files on that instance that weren't
  counted.

**Checkpoints are awaited.** `CHECKPOINT` returns before the checkpointer
reports its statistics (`checkpointer.c`). The collector waits for
`pg_stat_checkpointer.num_requested` to move, and then for the
checkpointer's `pg_stat_io` row to stop changing.

**Autovacuum is awaited.** An autovacuum worker's statistics reach shared
memory only when it exits; `autovacuum.c` never reports them sooner. A
worker running across either end of the interval would otherwise be
charged to the interval its exit fell in. That covers its pages, and on
prolly the files its collection or journal move writes. The background
writer reports about every `bgwriter_delay`, so its last writes before
the end snapshot may be missing.

**WAL retention.** The WAL is read before the final checkpoint, which
would recycle it. A slot keeps the window's own checkpoints from
recycling it, where `wal_level` allows one.

- **WAL size.** Over 600 s at 64 clients, the slot can hold tens of GB,
  and `max_wal_size` doesn't bound it. All of it is read in one pass after
  the window.
- **The counter runs keep `minimal`.** `replica` would log standby
  snapshots on a timer and make their WAL counters timing-dependent. At
  `minimal` there is no slot. The config refuses `write_amp` at `minimal`
  unless `checkpoint_timeout` exceeds the longest measured window. The
  first checkpoint restarts the timer.
- **Loud failure.** A checkpoint on WAL volume still recycles the WAL the
  read needs, and the read then fails.
- **Rejected:** the proposal moved the counter runs to `replica`.

`pg_get_wal_stats` stops silently at the flushed position. So if the end
LSN isn't flushed yet, the collector first runs `pg_switch_wal()`. The
switch record lands after the end.

### 3.2 The post-window flush

`[run] write_amp_materialize` takes `auto`, `prolly_checkpoint` or
`none`. `auto` means `prolly_checkpoint` for the S suite and `none` for
V, whose workloads commit themselves. Heap ignores the setting.
`prolly_checkpoint` runs `SELECT prolly_checkpoint()` in the bench
database. That folds the session's branch's working set into trees and
writes the chunks the window's work implies. A Dolt-commit-per-transaction
variant can still set `dolt.transaction_commit = on`.

### 3.3 Effects on the run, and config checks

The hooks move what other collectors see in a write_amp run:

- **Full-page images.** The window opens just after a checkpoint, so the
  full-page images of the pages it first touches are the window's.
  `wa.wal.fpi_bytes` shows their share. This fork's initdb enables
  checksums, so hint-bit images count too. Over a 600 s window with
  2-minute checkpoints, these images are the steady state. In a short
  window they bias WA upward.
- **`space`.** It reads after the fold and the checkpoint, so prolly's
  `chunk_store_bytes` includes the fold. That holds in `s_counters.toml`
  since `write_amp` was added there. Earlier history differs, but base
  and candidate runs share the harness, so the gate compares like with
  like.
- **`vacuum`.** It would start just after the checkpoint and log a
  full-page image of every page it touches. `s_write_amp.toml` leaves it
  out.
- **`recovery`.** The config refuses `write_amp` together with
  `recovery`: the flush's checkpoint leaves recovery nothing to replay,
  and a crash discards the cumulative statistics.
- **Replication slots.** The config refuses `max_replication_slots = 0`
  at a `wal_level` other than `minimal`.

### 3.4 New `pg_stat_prolly` counters

| Counter | Counted at |
|---|---|
| `journal_bytes_written` | `logged_write`, `append_write`, and a ROOT written over its PAD |
| `table_file_bytes_written` | `table_file_write`: the chunks, the file's index and footer |
| `manifest_bytes_written` | `write_manifest_temp`, and a collection's dropped-chunk list (not WAL-logged) |
| `gc_bytes_copied` | `gc_write`: collection, and moving the journal (`copying_move`), which is a collection |
| `conjoin_bytes_copied` | `journal_conjoin`, through `write_table_file(..., copies)` |
| `bmap_store_bytes_written` | `prepare_store` for a copy-on-write remap (`bmapprepareforchange`) |
| `bmap_extend_bytes_written` | `prepare_store` when extending for shared buffers (`smgrzeroextend`) |
| `bmap_bulk_bytes_written` | `prepare_store` and the content write when extending for bulk writes and copies (`smgrextend`, `smgrpreextend`) |

**How the counts are taken:**
- **Copies** are counted once their file is complete. A cancelled
  collection under-reports the breakdown, but not the table-file bytes.
- **Allocation.** Extending by `posix_fallocate` counts the bytes
  allocated, as `pg_stat_io` does.
- **Database.** A process counts in the database it is connected to. A
  `CREATE DATABASE` copying a chunk store counts in the database that
  runs it.

`PGSTAT_FILE_FORMAT_ID` changes to `0x01A5BCC0`, because
`changelog/counters` (#410) changes the struct under `0x01A5BCBF`.
`stat_prolly` prints the block map's bytes in blocks and checks that the
journal holds more than the chunks it frames. `gc` and `table_files`
check the copy, table-file and metadata counters.

**Not counted:**
- **Redo's writes** (`write_at`, `prepare_store` in redo). The startup
  process reports no statistics.
- **Chunk-store fsyncs.** The proposal included a counter, but the
  checkpoint's fsyncs run in the checkpointer, which belongs to no
  database, and counts go into `MyDatabaseId`'s entry. `write_file` and
  `write_manifest_temp` sync in the backend that writes, and are not
  counted either.

### 3.5 Deferred: per-server cgroup I/O

A `cgroup_io` collector, which reads `io.stat` for a delegated cgroup
around the postmaster, would cover what `wa_server` leaves out (part 2).
It is not built. `diskstats` still reports whole-device writes.

### 3.6 Tests

- **`tests/test_write_amp.py`, against a scripted server.** It checks:
  - the checkpoint order;
  - that only the flush waits for clients;
  - the autovacuum waits;
  - that materialization happens before the final checkpoint;
  - the rmgr grouping, the flush-before-read, and an interval with no
    WAL;
  - the slot's lifetime, including when the interval fails;
  - the WAL interval ending before the final checkpoint;
  - L, deletes, per-counter backwards checks, and the floor;
  - the block-map counters, and a build without the file counters;
  - the config checks;
  - the runner's order (`after_window` after every stop, before results
    and the post-run checks);
  - the tables' n/a reasons and the breakdown.
- **The `test_prolly` suite** passes, with the new `stat_prolly`, `gc` and
  `table_files` checks.
- **Measured on `build-rel`.** S1 heap and prolly in smoke and counter
  shapes, at `replica` and `minimal`. The rmgr groups sum to the LSN
  difference within 0.6%. `ProllyChunks` WAL is at least the chunk
  payload, and journal bytes are at least the payload.

### 3.7 Reporting

- **`table`.** Logical bytes per unit, `wa_server`, `wa_wal`, `wa_pages`
  and `wa_tree` (each n/a with its reason), followed by a table of every
  component per engine and cell. The HTML page has the same table.
- **V suite.** The V tables show server and chunk-store bytes per
  operation.
- **`README.md` and `compare`.** Each result set's `README.md` has the
  breakdown, and `compare` includes the ratios.
- **`scounters`.** It gates the logical, WAL and chunk-store totals. It
  doesn't gate the page total, or so the server total: those sum every
  backend type, and how often the background writer and the checkpointer
  write a page dirtied again is timing. That is why `17-performance-on-prs.md`
  reads `pg_stat_io` for client backends only in counter runs.
  `s_counters.toml` requires `write_amp`. The gate also fails, as an
  invariant, on any collector that ran in a cell of both base runs but not
  in that cell of a candidate run. Otherwise a candidate that lost
  `write_amp`, say to a missing `pg_walinspect`, would pass: a counter only
  one build has is skipped. A collector unavailable in every run is no
  loss.

### 3.8 Configs

- **`configs/s_write_amp.toml`.** The standard procedure's sizes and
  workloads, with these changes:
  - collectors `environment, wal, diskstats, pg_stat_io, cpu, space,
    engine_counters, write_amp` (`write_amp` required);
  - 5 repetitions at 1, 16 and 64 clients;
  - `wal_level = replica` and `max_replication_slots = 2`.
- **`configs/s_counters.toml`.** Adds `write_amp`, as a required
  collector, and keeps `minimal`.
- **`standard.toml`.** Unchanged. Its default collectors include
  `recovery`.

## 4. Open questions

- Should L count deleted rows at key width instead of 0? This matters for
  delete-heavy sysbench tests.
- `avg_row_bytes` is an average over the whole table, not over the rows
  written. An exact alternative is a per-table counter of the `t_len`
  written in `heap_insert`/`heap_update`, which would also count prolly
  tables. It is deferred unless the approximation proves misleading
  (S8's large rows).
- Bulk writes are outside `wa_pages` for both engines. Counting
  `bulk_write.c`'s I/O in `pg_stat_io` would cover S4's index builds for
  heap and prolly alike.
- `standard.toml` could include `write_amp` if recovery ran in a separate
  repetition.
