# 05. Storage targets and the logical/physical split

The engine should be selectable per storage type. That is safe only because
of one rule.

## The rule: one logical format, many physical layers

Branch, diff, merge, clone, push and pull all depend on the same data producing
the same chunks and the same root hash, and on chunks being portable between
databases. So these are **fixed for the whole format** and never vary by device
or engine:

- chunk boundaries: the chunker algorithm, its parameters and target size;
- node encoding, key encoding and row (value) encoding;
- the hash function;
- the canonical schema and object identity (by name, never by OID).

The same rule holds across **history**, not only across engines and
devices: a table's canonical form depends only on its current logical
content, never on the DDL or DML that produced it
(`09-versioned-catalogs.md` part 1). Local, physical details (OIDs,
dropped-column slots, statistics, block locations) never enter it.

If a table on NVMe and its clone on cloud storage chunked differently, their
roots would differ and every diff would show the whole table changed.

Everything else is physical and may vary: how chunks are stored, located,
written, logged, cached and allocated. An engine only has to produce
**canonical chunks** when a hash is needed (Dolt commit, diff, push). D3 and D4
already compute hashes only then.

Consequence: a merge between a table stored by D2 and one stored by D4 works on
canonical chunks either way.

**Required test (write it first):** load the same logical data through every
engine, with different write orders and update histories, and assert identical
root hashes and identical exported chunk sets. Its history half is
correctness gate 6 in `06-benchmarking.md`.

## Granularity of the choice

| Level | Existing mechanism | Chooses |
|---|---|---|
| Per tablespace (≈ per device) | Tablespaces already take per-device options: `random_page_cost`, `effective_io_concurrency` in `TableSpaceOpts` (`src/backend/access/common/reloptions.c`) | Engine, block size, flush policy, read-ahead |
| Per table | `CREATE TABLE … USING <am>` | Override the tablespace's engine |
| Per cluster | `initdb`, GUCs; `io_method` (sync / worker / io_uring) already exists in this checkout | WAL form, group commit, I/O issue mechanism |

Attach the main choice to the tablespace, with an explicit default per storage
class, overridable per table. Do not auto-detect devices.

## Expected fit (hypotheses to test)

| Storage | Likely winner | Reasoning |
|---|---|---|
| SATA SSD | D2 | Sequential appends + small synchronous logical WAL writes suit devices with costly random writes and internal GC |
| Local NVMe | D3 or D4 | Cheap random writes and deep queues favor in-place; with io_uring, small blocks (512 B–4 KB) are practical |
| Tiered / cloud object storage | D2 now, D5 later | Objects are immutable once written, so in-place engines don't apply; chunks map directly onto objects |
| Disaggregated storage/compute | D5 | Logical WAL as source of truth, page servers building chunks |

## Plan impact

- The benchmark becomes an **engine × storage** grid (`06-benchmarking.md`).
- The foundation needs canonical export in every engine plus the
  hash-equivalence test.
- Supporting many engines for experiments is not the same as shipping them.
  Expect to keep two.
