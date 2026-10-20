# 22. The chunk store against Dolt's

**Status: proposed and decided by the user 2026-10-19, as recommended
but for the hash's length (20 bytes, the user's choice; item 1), on
`cf49764a49`. Nothing here is built. Tracked in #442–#447, one issue for
each group of part 4.**

This spec compares the chunk store under spec 11 with Dolt's (NBS, the
reference) and doltlite's, and lists what changes. The decisions are
in part 4.

The format freeze recorded at spec 11's step 4 doesn't bind a prototype
(user, 2026-10-19). Every format change below goes into one clean
break (group A), as spec 11's "Up to step 8" makes each format change
before step 8 a clean break.

Sources:
- Dolt: `~/cursor_src/dolt`, `go/store/nbs`, `go/store/types`,
  `go/libraries/doltcore`.
- doltlite: `~/cursor_src/doltlite` at `b17149d539`, `src/chunk_*`,
  `src/doltlite_gc.c`.
- Ours: `src/backend/access/prolly`.

Line numbers were read on the dates above.

## 1. Side by side

| | Dolt | doltlite | Ours |
|---|---|---|---|
| Hash | SHA-512, first 20 bytes (`store/hash/hash.go`) | BLAKE3, 20 bytes (`prolly_hash.c`) | SHA-256, 32 bytes (`hash_sha256.c`) |
| Journal | Records with a tag, CRC-32C and snappy-compressed chunks; root records with a timestamp (`journal_record.go`) | Chunk and sealed root records appended to the one file (`chunk_store.h`) | Raw chunks, CRC-32C, segments; every byte in WAL first (`chunkstore_journal.c`) |
| Durability | fdatasync at each root commit, 4 MiB of zeros written ahead (`journal_writer.go`, `journal_file*.go`) | One fsync a commit (`chunk_store_commit.c`) | WAL first, group flush, the root's move in the commit record; standbys byte-identical |
| Journal index | `journal.idx`, a batch every 16,384 chunks, rebuildable (`journal_index_record.go`) | Sorted runs (at most 8) of hash-checked index pages in the file (`chunk_wal.c`) | None kept: each backend scans every segment on first use, ~1 s/GB (spec 14 C8) |
| Table file index in memory | ~40 bytes a chunk: 8-byte prefixes, u64 offsets, 12-byte suffixes (`table_index.go`) | Paged on disk, a 200 KiB cache (`chunk_index.c`) | `Entry` (a 65-byte `ProllyHash`, offset, length), ~88 bytes a chunk, in every backend for every file (`tablefile_basic.c`) |
| Lookup across files | A binary search per file, at most ~256 files | One file | One binary search per file, in turn (`tables_get`); no bound on files |
| Conjoin | In the background past 256 files, the smallest first, data copied by range, duplicates kept (`conjoiner.go`, `table_persister.go`) | None (one file) | `journal_conjoin` exists; only `test_chunk_conjoin()` calls it |
| Compression | snappy; a GC writes zstd archives by default (`dolt_gc.go`: `ArchiveLevel = SimpleArchive`) | None | LZ4 a chunk in table files; none in the journal |
| Archives | `.darc`: one 4 KiB zstd dictionary trained on the first 1,000 chunks; interpolation search; a dictionary cache (`archive*.go`) | None | None |
| GC | Online; newgen and oldgen stores; a keeper hook as read and write barrier; session safepoints (`value_store.go`, `store.go`) | Stops the world, copies and renames (`doltlite_gc.c`) | Online, WAL-logged, safe on standbys; default and full by a "settled" flag (`reclaim_copying.c`) |
| Automatic GC | From commits, at 128 MiB of journal or growth, with time and growth rules (`auto_gc.go`) | None | Autovacuum: GC past 128 MiB and 0.2 growth; a move past 256 MiB (`prolly_autovacuum.c`) |
| Batched reads | Sorted by prefix, reads 4 KiB apart coalesced, 16 at a time (`table_reader.go`, `store.go`) | Up to 16 adjacent leaves in one read (`chunk_store.c`) | A pread a chunk; batches only in transfers |
| Transfer | Chunks sent compressed as stored; whole files on clone; files up to 1 GiB (`datas/pull`) | Batches of 256 chunks (`doltlite_remote.c`) | Chunks decoded and written again by the receiver (`remote_file.c`, `remote_serve.c`); clone walks |
| Reflog | The last 5,000 roots, with their times (`journal.go`) | None | Each root's time in its ROOT record; `dolt_reflog` (item 4) |
| Statistics | Latency and size histograms, `StoreSizes` (`stats.go`, `doltdb.go`) | None | `pg_stat_prolly`: appends, bytes hashed, bytes the journal, table files and manifest write and GC and conjoin copy, journal reads to index it, collections, moves and conjoins with what collections kept and freed and their windows' time; `prolly_store_sizes()` (item 13) |

## 2. Where ours is ahead, kept

- **Every byte is in WAL.** That gives point-in-time recovery, base
  backups and standbys identical byte for byte. Dolt's store has none
  of these.
- **Concurrent appends.** Backends reserve ranges under a lock and
  write them outside it. Dolt's journaling store holds an exclusive
  lock for each process.
- **Transactional visibility.** A chunk carries its transaction's id,
  so aborted and running transactions' chunks stay hidden.
- **The append check covers every batch's references.** Dolt checks a
  commit's root and the memtable's references at a flush.
- **Checksums on metadata.** The table files' index and the manifest
  carry CRCs; Dolt's index doesn't (`table_index.go`).
- **Standby conflicts.** A collection's conflicts on a standby are
  resolved as VACUUM's are.

## 3. What isn't taken

- **Hashing each chunk again on every read** (doltlite does). CRCs
  cover reads, as in Dolt.
- **doltlite's single file.** Postgres's WAL already does what its
  sealed roots do.
- **Writing chunks once, outside WAL.** The second write is what makes
  standbys identical and allows point-in-time recovery. Item 2 makes it
  cheaper instead.
- **A second store for oldgen.** The manifest's "settled" flag does the
  same work (spec 11 8.4).

## 4. Decisions (user, 2026-10-19, as recommended)

Each group is an issue. The numbers are the items the issues use.

### A. One clean break of format (#442), first

1. **BLAKE3, its first 20 bytes** (user, 2026-10-19), as Dolt and
   doltlite keep 20.
   - 32 bytes was recommended, for 128-bit collision resistance where
     remotes accept chunks others push. 20 bytes gives 80 bits, and
     saves 12 bytes in every reference a node holds, every index entry,
     every TOAST pointer and every hash a transfer sends.
   - `ProllyHash` keeps its length, so it stays a parameter.
   - Vendor the official C implementation, taken under CC0, in
     `src/common`. The SIMD variants (SSE4.1, AVX2, AVX-512, NEON) are
     built with per-file flags and chosen at run time, as `pg_crc32c`
     is.
   - `hash_blake3.c` implements content_hash as `"blake3"`, the
     default in `prolly_config.c`.
   - The seven direct `prolly_hash_lookup("sha256")` calls
     (`canon_folded.c`, `refs_copies.c`, `transfer_chunks.c`,
     `remote_serve.c`, `prolly_table.c`, and `chunkstore_journal.c`'s
     GC copy) take the configured component instead. They break
     `00-architecture-principles.md`'s first rule.
   - The TOAST pointer's tag `VARTAG_ONDISK_SHA256` (`varatt.h`) becomes
     BLAKE3's, and its `va_hash[32]` (with the `StaticAssertDecl` on its
     size) becomes 20 bytes; spec 14's "the pointer tags its hash
     algorithm" holds.
   - SHA-256 was 22% of CPU at 64 KB values (spec 14 C8); that profile
     and gate 3 are measured again.
2. **Journal chunks compressed with LZ4** before `XLogInsert`, as table
   files' chunks are. Both WAL and the journal shrink. Dolt compresses
   its journal with snappy. A store's files don't say whether they hold
   compressed chunks, so a server built without LZ4 opens a store written
   with it and fails at the first compressed chunk it reads (a read, a
   collection partway through, a standby's replay), as it does for table
   files. A record that passes its CRC but doesn't decode is corrupt
   everywhere, collections included: one skipped could leave what it
   names unmarked.
3. **Table files' index laid out as Dolt's:** 8-byte prefixes sorted
   with ordinals, then lengths, then 12-byte suffixes, as Dolt's. That
   is about 40 bytes a chunk in memory, searched by interpolation.
4. **ROOT records carry a timestamp,** and `dolt_reflog` reads them.
   As built: journal version 4, a ROOT record's payload its hash and then
   the move's time (int64 microseconds, as `timestamptz`), carried in the
   commit's WAL record so redo writes the same bytes. The time is taken
   as the append is reserved, under the journal's lock and never before
   the last append's, so a later place in the journal never has an
   earlier time. `dolt_reflog` reads the ROOT records of the segments
   named, passing over the chunk records between by their length alone
   (Dolt keeps the last 5,000 roots in memory instead), and keeps the
   newest 5,000.
5. **The manifest records the hash's algorithm and length and the chunk
   format's version.** A store that differs is refused.

The expected hashes in `test_prolly` and the vcscenario ports are
written again once. BLAKE3's official test vectors are added as a
conformance test.

### B. The read path and scaling (#443), before step 8

6. **One journal index per database in shared memory**, written at
   checkpoint and rebuilt from that file and the journal's tail (Dolt's
   `journal.idx`; doltlite's runs).
7. **Table files' indexes shared** (DSA, or their index sections
   mapped), not an array in every backend.
8. **Autovacuum conjoins the smallest files first past a count**, as
   Dolt's `chooseConjoinees` does, dropping duplicate chunks while
   merging (Dolt keeps them).
9. **A miss costs O(1) across files:** the database-wide index of 6 and
   7, or a prefix filter for each file.
10. **A checkpoint fsyncs only the files written since the last one,**
    through `RegisterSyncRequest`, as `md.c` does. Today
    `journal_checkpoint` fsyncs every file of every database.
11. **A batched `get_many`:** requests sorted by offset, reads within 4
    KiB coalesced, issued through read streams and AIO. Diff, merge,
    the GC copy and transfer use it.
12. **Puts checked for duplicates in batches at a flush** (Dolt's
    memtable `hasMany`). Lookups one at a time were 15% of CPU (spec 14
    C8).

### C. Observability (#444), early

13. **`pg_stat_prolly` reports the store's sizes and the rest of its
    work.** Bytes written and copied are counted since #420
    (`journal_bytes_written`, `table_file_bytes_written`,
    `manifest_bytes_written`, `gc_bytes_copied`,
    `conjoin_bytes_copied`, which spec 20's write amplification reads).
    What's left:
    - journal bytes and segments, as they stand;
    - table files settled and not, and their bytes;
    - journal index scans and their bytes;
    - collections, moves and conjoins, with chunks kept and freed,
      bytes freed and the window's time.

    A function gives a database's store sizes, as Dolt's `StoreSizes`
    does, and `dolt_gc()` returns its statistics. Groups A, B and D are
    measured with these and spec 20's write amplification, and step 8
    reads them.

### D. Garbage collection (#445)

14. **The swap window locks only the database collected,** not the
    server (`ProllyJournal`).
15. **The mark queue spills to a BufFile** under `maintenance_work_mem`.
16. **A collection's leftovers are removed at the end of recovery.**
17. **Marks follow the references each chunk's format declares**
    (Dolt's `getAddrs`), not every byte pattern shaped like one. Group A
    makes every chunk typed.
18. **A collection writes files of a set size** (Dolt's rotating GC
    files), so they can be sent whole (group E).

Items 14 to 16 are spec 11 8.11's "not as designed".

### E. Remotes (#446)

19. **Chunks are sent as stored,** not decoded and encoded again (spec
    11 7.1).
20. **Whole-file clone into an empty database,** a bounded seen set,
    and resume (7.11, 7.12).

### F. After step 8 (#447)

21. **A prototype archive format for settled files:** zstd with a
    trained dictionary, as Dolt's `.darc`, behind the table-file
    interface. It is adopted only if its size and read costs pay; Dolt
    publishes no numbers.
22. **The hash's length** was to be decided here; the user chose 20
    bytes at once (item 1). Step 8 records what it costs and saves
    against 32.

## 5. Order

1. A.
2. C.
3. B.
4. D and E.
5. Spec 11's step 8.
6. F.

Spec 21 (schema diffs) is also before step 8. The two don't depend on
each other.
