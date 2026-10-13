# 08. Open questions

Nothing below is decided. Update this file when something is.

## Status

| Topic | Status |
|---|---|
| Overall concurrency design | Open. Option 8 (coordination in the tree) was the user's stated favorite on 2026-10-12; the current plan is to measure candidates instead of choosing up front |
| Candidate set (D1–D5, K1–K3, pairings) | Proposed 2026-10-13; agreed as the working plan, not as final designs |
| Engine per storage type | Open; hypotheses in `05-storage-targets.md` |

## Questions

1. **Node I/O (B4).** Starting assumption: nodes are `BLCKSZ` buffer-manager
   pages reached through smgr. Still to confirm: whether the forced split at a
   full page costs too much sharing, and whether D1/D2's hash → block map
   performs well enough, or a separate store with a checkpoint hook is needed.
2. **Row identity (B7).** Synthetic content-derived TIDs with a map, or patch
   core for wide row identifiers?
3. **Chunker boundary rule.** Boundaries that depend on keys only stay put when
   values change, so K2's lock entries and SSI page mappings stay stable. Rules
   that also count bytes control node size better. Page-bounded nodes need a
   forced split either way. Which rule, and what size targets?
4. **Canonical format.** Our own Postgres-native format (node layout, key and
   value encoding, content hash), or Dolt byte compatibility for DoltHub
   interop? Dolt compatibility would constrain nearly every foundation choice,
   so the default is our own format.
5. **Catalogs.** Are system catalogs versioned per branch? Large and separate.
6. **Reference counting (D3b).** Can decrements be propagated lazily without
   unbounded backlog after a branch delete? What bounds the background work?
7. **Cross-branch coordination (axis 4b).** Is per-chunk latching across
   branches needed for in-place engines, and what does it cost?
8. **D4 canonicalization.** How is a committed-only view of an in-place page
   computed cheaply and consistently for hashing?
9. **Commit markers (K2).** Memory bound under long-running snapshots; what to
   do at the limit (spill, or error like `max_locks_per_transaction`)?
10. **Decision rule.** What margin versus B0 on the S suite is acceptable in
    exchange for version control? (For after the roadmap's step 6.)
11. **Storage targets in scope.** Is cloud/tiered storage (D5) a goal, or is
    local SSD/NVMe enough for now?
12. **Value encoding.** Reuse Postgres's tuple format so executor deforming
    works unchanged, or define a portable canonical row encoding (fixed
    endianness, no alignment padding) and deform from it? Canonical chunks must
    be identical across platforms for clone/push/pull.
13. **First engine.** D1+K1 is proposed for roadmap step 4 because it is the
    simplest and the reference point for the others. Confirm before starting.
14. **Version-control scope for the first engine.** Which of V1–V4 must D1+K1
    support in step 4? Clone/push/fetch (V4) needs a transfer protocol, which
    may be more than the first engine needs.
15. **Choosing a pairing per table.** Today the table AM name picks a fixed
    profile (`prolly_d2` → D2+K1). D2+K2 needs another way to pick K2: another
    AM name, or a setting read only at table creation and recorded in the
    tree (so a session can never read a table with different components than
    wrote it). Needed before D2+K2 is built.
16. **WAL description in `pg_waldump`.** The log interface's `desc` and
    `identify` must also run in `pg_waldump`, which is a frontend program
    without the registry.
17. **Index trees on non-prolly tables.** `CREATE INDEX … USING prolly` on a
    heap table is accepted up to `ambuild` today. Should prolly indexes be
    refused on heap tables, or supported?
18. **Loading the larger-than-RAM size.** A fresh ~300 GB load per cell is
    impractical (5 repetitions x 5 client counts x 9 workloads). Options: load
    once per workload and size and reuse it across cells (results then depend
    on cell order), or keep a loaded copy and restore it per cell (a 300 GB
    copy each time, since the data directory is copy-on-write-free). Needed
    before step 3 runs that size.
19. **Indexes on prolly tables in the S suite.** pgbench and BenchBase create
    their primary keys and indexes as btree. On a prolly table that means a
    btree index over a prolly table, not a prolly index tree. Should the S
    suite make prolly the default index AM for prolly runs (no such setting
    exists in Postgres; it would need a small core change or rewriting the
    workloads' DDL)?
20. **Device bytes on this machine are dominated by btrfs fsync.** In the
    step 2 smoke run every write workload wrote 45-77 KB to the device per
    transaction, whatever its WAL volume (S5: 127 bytes of WAL, 46 KB on the
    device; read-only workloads: ~0). That is a fixed cost per commit fsync
    from the filesystem (most likely btrfs's fsync log tree), not from
    Postgres, and it would swamp the engine differences the metric is meant
    to show. Options for step 3: run the data directory on a dedicated
    ext4/XFS partition or device, or report device bytes with this caveat.
    Not yet confirmed with a filesystem-level trace.

