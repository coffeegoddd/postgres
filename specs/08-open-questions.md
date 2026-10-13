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
