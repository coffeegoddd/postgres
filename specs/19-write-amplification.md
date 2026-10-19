# 19. Write amplification in the S and V suites

**Status: decided 2026-10-19, as recommended, on `39de7462e5`; built in
the `s-suite/wa-*` stack. Part 3 describes what was built. Where that
differs from the first proposal, the difference is noted with its
reason.**

Every benchmark result reports write amplification for both engines:
how many bytes the server wrote for each byte of row data the workload
asked it to write. Heap and prolly are split the same way: WAL, data
pages, and (for prolly) the chunk store.

## 1. What existed

| Recorded per cell (`results.jsonl`) | Collector | Shown by `table` |
|---|---|---|
| `wal_bytes`, `wal_bytes_per_operation` (LSN difference), `wal_records`, `wal_fpi`, `wal_fpi_bytes` | `wal.py` | WAL per unit only |
| `device_write_bytes(_per_operation)`: the whole data device | `diskstats.py` | yes |
| `io_writes`, `io_write_bytes` (relation objects, `pg_stat_io`) | `pg_stat_io.py` | no |
| `engine.chunk_bytes_appended`, `engine.nodes_written`, ... (`pg_stat_prolly`) | `engine_counters.py` | no |
| `relation_bytes`, `chunk_store_bytes`, `row_representation_bytes` (after the window) | `space.py` | relation bytes only |

None of this measured write amplification:

1. **No logical denominator.** Every write metric was bytes per
   transaction, and nothing measured how many bytes of row data were
   written.
2. **Deferred writes fell outside the window.** The window closes when
   the clients exit. Heap's dirty pages reach disk at a later checkpoint.
   Prolly's tree writes are deferred too: plain SQL `COMMIT` writes no
   chunks. Chunks are written only by Dolt commits, `prolly_checkpoint()`,
   checkout, `dolt.transaction_commit = on`, and GC/move. Measured on S1,
   the window itself shows 0 chunk bytes on prolly, and `io_write_bytes`
   is 0 for both engines.
3. **WAL was not split by source.** For prolly, every chunk byte is
   written twice: once in full into WAL (`ProllyChunks` APPEND/FILE
   records, in `logged_write`, `append_reserve` and `table_file_write`),
   and then into the journal or a table file.
4. **Chunk-store file writes were counted nowhere.** The journal, table
   files and manifests are written with `pg_pwrite`/`FileWrite`, outside
   bufmgr. `CHUNK_BYTES_APPENDED` counts only payload, at put time.
5. **Some page-store writes bypass `pg_stat_io`.** bmap's `prepare_store`
   zeroes and extends the page store with direct `mdwritev`/`mdzeroextend`
   calls, and `pg_stat_io.py` omitted `extend_bytes`.
6. **Reporting** showed only WAL per unit and device writes per unit.

## 2. Definitions

All amounts are summed over the collector's **write-accounting
interval**:

- **Start.** After a `CHECKPOINT`, as the window opens.
- **End, for WAL.** After the post-window flush (part 3.2), just before
  its final `CHECKPOINT`.
- **End, for pages and the chunk store.** After that `CHECKPOINT`.

**Logical bytes, L.** The new row versions the SQL wrote, defined the same
way for every engine:

```
L = Σ over workload tables t:  (Δ tuples_inserted(t) + Δ tuples_updated(t)) × avg_row_bytes(t)
```

- **Row counts.** The Δ counts come from `pg_stat_get_tuples_*()` at the
  interval's boundaries. Prolly tables run heap's code and are counted the
  same way: on S1, both engines count exactly 4 rows written per
  transaction.
- **Row size.** `avg_row_bytes` is `avg(pg_column_size(t.*))` after the
  interval, the expression `space.py` uses.
- **Deletes** add no bytes. They are reported as `wa.logical_rows_deleted`.
- **Indexes.** Index bytes are not logical bytes: index maintenance is
  amplification.

**Physical components** (bytes over the interval; each is also reported
`_per_operation`):

| Metric | Heap | Prolly | Source |
|---|---|---|---|
| `wa.wal.row_bytes` | Heap/Heap2 record bytes | same (shared heap code) | `pg_get_wal_stats`, by rmgr |
| `wa.wal.index_bytes` | Btree/Hash/Gin/Gist/SPGist/BRIN | same | same |
| `wa.wal.fpi_bytes` | full-page images, all rmgrs | same, including bmap CoW images | same |
| `wa.wal.blockmap_bytes` | 0 | `BlockMap` | same |
| `wa.wal.chunk_bytes` | 0 | `ProllyChunks` | same |
| `wa.wal.other_bytes` | everything else (Transaction, XLOG, Standby, ...) | same | same |
| `wa.wal.unattributed_bytes` | the part of the LSN difference that is in no record (page headers, alignment): about 0.5% | same | |
| `wa.wal.total_bytes` | LSN difference | LSN difference | |
| `wa.pages.write_bytes` | `pg_stat_io` relation `write_bytes + extend_bytes`, **all backend types** | same | `pg_stat_io` |
| `wa.pages.bmap_store_bytes` | n/a | the block map's own page-store writes (reported, not added: shared buffers count the same extends) | `pg_stat_prolly` |
| `wa.tree.chunk_payload_bytes` | n/a | `chunk_bytes_appended` | `pg_stat_prolly` |
| `wa.tree.file_bytes` | n/a | journal + table-file + manifest bytes written | `pg_stat_prolly` |
| `wa.tree.gc_copied_bytes`, `wa.tree.conjoin_copied_bytes` | n/a | of the table-file bytes, the copies (breakdowns, not additions) | `pg_stat_prolly` |
| `wa.tree.materialize_*`, `wa.wal.materialize_bytes` | n/a | the share the post-window fold wrote | |

**Ratios.** These are `None` when L is under 64 KiB or the outcome is
read-only:

```
wa_wal          = wa.wal.total_bytes / L
wa_pages        = wa.pages.write_bytes / L
wa_tree         = wa.tree.file_bytes / L              # prolly only
wa_tree_payload = wa.tree.chunk_payload_bytes / L     # tree shape alone
wa_server       = (wa.wal.total_bytes + wa.pages.write_bytes + wa.tree.file_bytes) / L
```

`wa_server` is the headline number: every byte the server handed to the
kernel, so heap and prolly compare like for like. (In the proposal,
`wa_tree` also added GC copies. Copies are part of the table-file bytes,
so they are breakdowns instead. The floor was 1 MiB; a counter run's S1
writes 470 kB.)

**Measured** (S1, scale 5, 4 clients, 5 s window, one repetition; a smoke
run, not a ranking):

| | heap | prolly |
|---|---:|---:|
| logical bytes per transaction | 236 | 236 |
| `wa_wal` | 23.3 | 44.8 |
| `wa_pages` | 22.5 | 23.1 |
| `wa_tree` | n/a | 20.9 |
| `wa_server` | 45.8 | 88.8 |

Prolly's extra WAL is the `ProllyChunks` records: 4,935 bytes per
transaction, the same bytes its journal then holds. Its journal adds 1.1%
framing to the 59 MB of chunks the fold wrote.

## 3. What was built

### 3.1 The `write_amp` collector (`pbench/collectors/write_amp.py`)

The collector owns the interval's boundaries through two new hooks, so the
other collectors keep their semantics:

- **`Collector.before_window(ctx)`.** `CollectorWindow.__enter__` calls
  it on every collector before any `start`.
- **`Collector.after_window(ctx, outcome)`.** The runner calls it after
  every `stop`, and before any `results` and before the post-run checks.
  The proposal had the flush in `post`, but `correctness.verify` and
  `workload.verify` run before `post`. They read the tables, and reading
  can write hint bits, which would be charged to the workload.

What the collector does at each step:

- **`check`.** `CREATE EXTENSION IF NOT EXISTS pg_walinspect`; the
  collector is `unavailable` if that fails. `build_postgres.py` installs
  `contrib/pg_walinspect`.
- **`before_window`.**
  1. Wait for client backends to exit.
  2. Create a physical slot, if `wal_level` is not `minimal`.
  3. `CHECKPOINT`.
- **`start`.** Snapshot the insert LSN, `pg_stat_io` (all backend
  types), the tuple counts, and `pg_stat_prolly`.
- **`after_window`.**
  1. Wait for the clients to exit.
  2. Materialize (part 3.2).
  3. Read the WAL's end LSN and its per-rmgr stats, and drop the slot.
  4. `CHECKPOINT`.
  5. Take the end snapshot.
- **`results`.** Compute L, the components and the ratios. Write the rmgr
  rows to `raw/.../write-amp-wal.json`.

**Checkpoints are awaited.** `CHECKPOINT` returns before the checkpointer
reports its statistics (`checkpointer.c`). The collector waits for
`pg_stat_checkpointer.num_requested` to move, and then for the
checkpointer's `pg_stat_io` row to stop changing.

**WAL retention.** The WAL is read before the final checkpoint, because
that checkpoint would recycle it. A slot keeps the window's own timed
checkpoints from recycling it where `wal_level` allows one.

At `wal_level = minimal`, there is no slot. The counter runs keep
`minimal` because `replica` would log standby snapshots on a timer and
make their WAL counters timing-dependent. Those runs have no timed
checkpoints. If a checkpoint in the window did recycle the WAL, the read
fails instead of undercounting. The proposal moved the counter runs to
`replica`; that was rejected for the timing dependence.

`pg_get_wal_stats` stops silently at the flushed position. So if the end
LSN isn't flushed yet, the collector first runs `pg_switch_wal()`; the
switch record lands after the end.

### 3.2 The post-window flush

`[run] write_amp_materialize` takes `auto`, `prolly_checkpoint` or
`none`. `auto` means `prolly_checkpoint` for the S suite and `none` for
V, whose workloads commit themselves. Heap ignores the setting.
`prolly_checkpoint` runs `SELECT prolly_checkpoint()` in the bench
database. That folds the session's branch's working set into trees, and
writes the chunks the window's work implies. A Dolt-commit-per-transaction
variant can still set `dolt.transaction_commit = on`.

S6 needs no special case. Its snapshot holder commits and exits before
`measure` returns, so the wait finds no clients.

### 3.3 Config checks (`core/config.py`)

- `write_amp` together with `recovery` is refused. The flush's checkpoint
  leaves recovery nothing to replay, and a crash discards the cumulative
  statistics.
- `max_replication_slots = 0` is refused at a `wal_level` other than
  `minimal`.

### 3.4 New `pg_stat_prolly` counters

| Counter | Counted at |
|---|---|
| `journal_bytes_written` | `logged_write`, `append_write`, and a ROOT written over its PAD |
| `table_file_bytes_written` | `table_file_write` |
| `manifest_bytes_written` | `write_manifest_temp` |
| `gc_bytes_copied` | `gc_write`: collection, and moving the journal (`copying_move`), which is a collection |
| `conjoin_bytes_copied` | `journal_conjoin` (through `write_table_file(..., copies)`) |
| `bmap_store_bytes_written` | `prepare_store`'s zeroing and extending, and an extend's content write |

`PGSTAT_FILE_FORMAT_ID` changes with the struct. `stat_prolly` checks
that the journal holds more than the chunks it frames.

**Not counted:**
- **Redo's writes** (`write_at`). The startup process flushes no
  statistics.
- **Chunk-store fsyncs.** The proposal included a counter for them, but
  the checkpointer does them, and it belongs to no database. Counts go
  into `MyDatabaseId`'s entry.

### 3.5 Deferred: per-server cgroup I/O

A `cgroup_io` collector, which reads `io.stat` for a delegated cgroup
around the postmaster, was optional and is not built. `wa_server` does
not need it. `diskstats` still reports whole-device writes.

### 3.6 Tests

- **`tests/test_write_amp.py`, against a scripted server.** It checks:
  - the checkpoint order (warm-up pages excluded, the window's included);
  - that materialization happens before the final checkpoint;
  - the rmgr grouping, the flush-before-read, and the slot's lifetime;
  - the WAL interval ending before the final checkpoint;
  - L, deletes, the ratios' floor, and counters going backwards;
  - a prolly build without the file counters (no total);
  - the config checks;
  - the hooks' order;
  - the tables' n/a cells and the breakdown.
- **Measured on `build-rel`.** S1 heap and prolly in smoke and counter
  shapes, at `replica` and `minimal`. The rmgr groups sum to the LSN
  difference within 0.6%. `ProllyChunks` WAL is at least the chunk
  payload, and journal bytes are at least the payload.

### 3.7 Reporting

- **`table`.** Logical bytes per unit, `wa_server`, `wa_wal`, `wa_pages`
  and `wa_tree` (n/a for heap, and under the floor), followed by a table
  of every component per engine and cell. The HTML page has the same
  table.
- **V suite.** The V tables show server and chunk-store bytes per
  operation.
- **`README.md` and `compare`.** Each result set's `README.md` has the
  breakdown, and `compare` includes the ratios.
- **`scounters`.** It counts the totals (logical, WAL, pages, chunk
  store, server) as byte counters.

### 3.8 Configs

- **`configs/s_write_amp.toml`.** The standard procedure's sizes and
  workloads, with these changes:
  - collectors `environment, wal, diskstats, pg_stat_io, cpu, space,
    engine_counters, write_amp, vacuum` (`write_amp` required);
  - 5 repetitions at 1, 16 and 64 clients;
  - `wal_level = replica` and `max_replication_slots = 2`.
- **`configs/s_counters.toml`.** Adds `write_amp`, and keeps `minimal`.
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
- `standard.toml` could include `write_amp` if recovery ran in a
  separate repetition.
