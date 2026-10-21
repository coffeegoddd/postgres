# 22. The chunk store against Dolt's

**Status: proposed and decided by the user 2026-10-19, as recommended
but for the hash's length (20 bytes, the user's choice; item 1), on
`cf49764a49`. Groups A–E built 2026-10-19 as one stack of pull requests
(#460–#465, #467–#479, #502–#503, and this spec as built, #504; stack
#466), not merged;
part 6 says what each built and where it departs. F (#447) waits for
step 8. Tracked in #442–#447, one issue for each group of part 4.**

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
| Conjoin | In the background past 256 files, the smallest first, data copied by range, duplicates kept (`conjoiner.go`, `table_persister.go`) | None (one file) | Autovacuum conjoins the smallest past `prolly.conjoin_max_files` (256), Dolt's rule, bounded per pass by `maintenance_work_mem` and 1 GB, paced, each chunk's hash checked, duplicates dropped |
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
  backups and standbys identical byte for byte. (Bytes copied from the
  store's own files are logged as where they come from: part 6, G.) Dolt's store has none
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
   is about 40 bytes a chunk in memory, searched by interpolation (as
   built, 32 bytes, then 34 with #469's offsets and filter: item 3 of
   part 6).
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
   7, or a prefix filter for each file. As built: a blocked Bloom filter
   in each file (10 bits a chunk, its probes in one cache line), so a
   miss costs one line per file: O(files), which conjoining keeps at no
   more than `prolly.conjoin_max_files`, not O(1). A table file's index
   is mapped where the OS can; an I/O error reading a mapping is a
   SIGBUS (the server restarts) where `read()` would raise an error.
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

## 6. As built (2026-10-19)

One linear stack, a pull request a layer, in part 5's order. Part 1's
"Ours" column is the store before group A, but for a few rows rewritten
as decided (conjoin, reflog, statistics); this part says what was built.

### A. One clean break of format

- **1. BLAKE3, its first 20 bytes** (#460, #461). The official C
  implementation (1.8.2) is vendored in `src/common`, its SIMD variants
  chosen at run time; `hash_blake3.c` is the default content hash, and
  every direct SHA-256 lookup takes the configured one. Commit ids are
  40 hex digits. The TOAST pointer's tag is `VARTAG_ONDISK_BLAKE3` (6),
  its hash 20 bytes. Not yet measured again (spec 14's C8 profile, gate
  3): step 8.
- **2. Journal chunks compressed with LZ4** (#462), when that makes them
  smaller; a codec byte in each record. Journal v3.
- **3. Table files' index laid out as Dolt's** (#463): prefixes with
  ordinals, lengths, suffixes; interpolation search, every other step a
  bisection; about 32 bytes a chunk. B's #469 replaces the lengths with
  u64 offsets (nothing is computed from them) and adds a blocked Bloom
  filter (format v3): about 34 bytes a chunk for 20-byte hashes.
- **4. ROOT records carry a timestamp; `dolt_reflog`** (#464). Dolt's
  reflog scripts ported (one GC case partial: it tags a commit, and
  there are no tags).
- **5. The store records its format** (#461): hash name and length and
  the chunk format's version, in each segment's header and the
  manifest (v4); a store of another format is refused.

### C. Observability

- **13.** (#465) `pg_stat_prolly` counts journal scans and bytes,
  collections, moves, chunks kept and freed, bytes freed and conjoins;
  `prolly_store_sizes()` gives the store's sizes, `prolly_gc([full])` a
  collection's statistics. **Departure:** `dolt_gc()` still returns
  `{0}`, as Dolt's does and the Dolt ports check; its statistics are
  `prolly_gc()`'s.

### B. The read path and scaling

- **10. A checkpoint fsyncs only what was written since** (#467),
  through sync requests (`SYNC_HANDLER_PROLLY_CHUNKS`).
- **8. Autovacuum conjoins the smallest files past a count** (#468),
  `prolly.conjoin_max_files` (256), Dolt's chooser; settled files kept
  apart.
- **7, 9. Table files' indexes mapped and shared**, each with a Bloom
  filter for misses (#469).
- **6. One journal index per database in shared memory** (#470), in
  DSA, and **saved at checkpoints** as `prolly_index`, read back at
  start and brought up to date from the journal's tail (#471).
- **11. `get_many()`, read-ahead and ordered copies** (#472).
  **Departures:** no read streams or AIO (this tree's AIO reads only
  into shared buffers, through smgr); `FilePrefetch` gives the overlap.
  Diffs and merges still read a node at a time: reading a node's
  children ahead would break their bound of reading only what changed.
- **12. Puts checked for duplicates all at once, as they are appended**
  (#473).

### D. Garbage collection

- **14. The window takes the database's append lock** (#474), in its
  shared journal index, not the server's.
- **15. The mark queue spills to a temporary file** past
  `maintenance_work_mem` (#475).
- **16. A collection's leftovers are removed as crash recovery ends**
  (#476), from `StartupXLOG()` before connections are let in, each
  removal WAL-logged. **Departure:** not in the end-of-recovery
  checkpoint, which may write no WAL but its own; and not after archive
  recovery, where a promoted standby's readers may still read what is
  retiring.
- **17. Marks follow the references each chunk declares** (#477). A
  tree's leaves record the chunks their values name
  (`PROLLY_ITEM_CHUNK_REFS`, the environment's `chunk_refs()`), which
  `references()` reports and walks don't follow: commits, the refs,
  canonical roots and baselines declare theirs. Assert-enabled builds
  warn of a chunk a chunk's values spell and don't declare.
- **18. A collection writes files of a set size**,
  `prolly.gc_file_size` (64 MB) (#478).

### E. Remotes

- **19. Chunks are sent as stored** (#479): framed (codec, length,
  bytes), as a table file's chunk is less its CRC, read and written so
  by the store, table files and both remotes; the receiver decodes them
  only to check them.
- **20. Whole-file clone, resume, a bounded seen set** (#502, #503). A
  clone copies the remote's files whole (`dolt_serve_files()`,
  `dolt_serve_file_chunks()`), each kept out of what the store reads
  until all are checked, then adopted at once; a clone that fails keeps
  what it copied, and one tried again copies only what it lacks. A
  remote that can't list its files, or whose journal is a fifth of it or
  more, is walked. **Departure:** only a clone resumes; a fetch or push
  that fails starts over, as Dolt's do. What a transfer has seen (and
  `dolt_serve_since()` has sent) is a map held to
  `maintenance_work_mem`, past it sorted runs in a temporary file with
  a Bloom filter each.


### G. Copies WAL-logged as where they come from (#564, 2026-10-20)

A write that copies chunks the store already holds logged their bytes,
then wrote them: each copied byte twice, though the source is durable on
the primary and, byte for byte, on every standby. Now it logs where they
come from, as Postgres's `CREATE DATABASE ... STRATEGY FILE_COPY` does,
and redo copies them again from its own files. Chunks received from a
remote are new to the store and stay logged as bytes.

- **The record.** `XLOG_CHUNKS_COPY`: the destination (a table file, or
  a segment) and offset, the sources' database (the same, or `CREATE
  DATABASE`'s template), the length and CRC-32C of the bytes, then 21
  bytes a source: kind, file id, offset, length. A source is a table
  file's bytes as they are, a segment's bytes as they are, or a
  segment's `CHUNK`/`XCHUNK` record, which redo frames (no decoding) and
  adds as the table file format adds a stored chunk. Byte sources that
  run on are merged. The destination's index and footer stay literal
  bytes in `XLOG_CHUNKS_FILE`. `XLOG_PAGE_MAGIC` 0xD127.
- **Copies as stored.** A collection (and so a journal move) copies each
  chunk as stored: a journal record's frame, a table file's chunk bytes
  (which the format gives back unchanged: a stored frame added again is
  the bytes it was read from, now in the table file interface's
  contract). Each is still decoded and checked against its hash. Redo
  doesn't depend on LZ4 giving the same bytes on primary and standby: it
  never compresses. A chunk is described only if the bytes written are
  what redo will make: a table file's must be the source's bytes; a
  format without `write_add_stored()` gets literal bytes.
- **Redo.** Reads the sources, checks length and CRC, writes the
  destination. Measured (`143_copy_records.pl`): a collection of the
  journal, 33,856 bytes of WAL for 6,568,381 copied (0.5%, most of it the
  index); a full collection of a table file, 4,648 bytes for 2,627,431
  (0.2%); with `prolly.log_copied_bytes = on`, 989,000 for 985,360.

**Why redo finds the sources.** The invariants, in the file header of
`chunkstore_journal.c` ("Copies in redo"):

1. *A source holds its bytes when the copy is replayed, or is gone.* A
   copy's sources are named in the manifest, so a table file source was
   written (and synced, and named) before the copy; table file ids are
   never given twice, and a table file is never written again. A
   segment range was written by appends before the copy, and segments
   are only appended to (the cut of an unwritten tail is past every
   whole record). Redo runs forward, so it finds those bytes, or finds
   the file removed by a record after the copy.
2. *A source is removed only after the copy's file is durable.* A
   collection's sources go at its retire, after the swap; conjoin's
   after its manifest; both sync their new file before the manifest
   naming it. `CREATE DATABASE` syncs each copied file before it
   commits; its template can be dropped only after. In redo, every
   removal (unlink, retire, a database dropped) first syncs the files
   copies wrote since the last removal. So crash recovery, or a
   standby's restart from a restartpoint before the copy, finds a missing
   source's copy already on disk: redo checks the destination's bytes
   against the copy's CRC, and leaves them.
3. *A removal moves the minimum recovery point past itself* (as a
   truncation's redo does). A copy whose sources fail it and whose file
   doesn't hold its bytes (both were removed later) waits for that
   removal; it can only before recovery is consistent. One still waiting
   at the consistent point (a new optional chunk store callback,
   `redo_consistent()`, beside `XLogCheckInvalidPages()`; and at the end
   of crash recovery) is a PANIC; so is a copy that fails once
   consistent. Either means the server's files aren't those the WAL was
   written against, which the byte-identical standby invariant (part 2)
   rules out; the message names the source that failed.
4. *No store file is removed while an online backup runs* (the
   full-page-write analogue: what keeps a backup's redo whole). A backup
   lists files and reads them later; a source removed in between and a
   destination made after the listing would leave its redo neither. So a
   removal checks `XLogBackupInProgress()` (a new export from `xlog.c`,
   read under a WAL insertion lock, as `do_pg_backup_start()` changes
   the count under all of them) after logging itself: a backup starting
   later has its start point after the removal and every copy from the
   file. A file kept is unnamed: the next writer's `remove_unnamed()`
   takes it on a primary. On a standby (its own backups: the primary
   can't know of them) redo keeps a list and removes them at the first
   record after the backups end; a standby restarted meanwhile leaves
   them until its promotion's leftovers pass or the next writer after
   it.
5. *PITR.* A source written before the backup's start checkpoint was
   synced by it (a checkpoint waits for ranges in flight, and copies
   hold `DELAY_CHKPT_START` from their record to their write), so it is
   in the backup whole; one written after is rewritten by records in the
   range replayed. Recovery stopping between a copy and its source's
   removal has both; the copy's file is unnamed there, and the next
   writer removes it.

- **Conjoin** copies its files' chunks as stored too, each a table file
  byte range (in hash order, so few run on), checked against its hash.
  Measured (`144_conjoin_copies.pl`): 23,528 bytes of WAL for 3,937,304
  copied (0.6%); bytes logged, 6,911,408 for 6,890,242. Its sources go
  after its manifest, which follows its file's sync (invariant 2).
- **`CREATE DATABASE`** copies the template's table files and segments
  as copies of their bytes, a megabyte a record, whatever its strategy
  (decided, user, 2026-10-20): the file-copy strategy's files are
  written over with the same bytes, so the store's durability is its own
  WAL's, not the strategy's checkpoints. A segment is described up to
  its last whole record; past that (a range a crash left reserved, its
  zeros never in WAL, so on no standby) its bytes are logged. Each file
  is synced before the command commits, so before its template can be
  dropped (invariant 2; a dropped database's redo syncs copies first,
  as a removal's does). **Not safe as copies:** a template dropped while
  an online backup runs, which may be missing from the backup and can't
  be kept for it (Postgres removes the directory). So while a backup runs
  on the primary the copies log their bytes (`copy_write()`'s
  `in_backup`, asked under `DELAY_CHKPT_START`: a backup starting later
  has its start checkpoint after the record and waits for the write).
  A backup taken from a standby can't tell the primary; there a template
  dropped during the backup, after a `CREATE DATABASE` from it that the
  backup's redo replays, fails the restore at its consistent point
  (invariant 3's PANIC), not silently, as Postgres's own file-copy
  strategy would leave the copy empty. Measured
  (`145_createdb_copies.pl`, a 4.9 MB store): the file-copy strategy
  logs 1,672 bytes, against 4,938,504 with the bytes; the WAL-log
  strategy, 4,879,728 (its catalogs' pages) against 9,814,832.
- **Checksums.** The record's CRC covers the bytes the sources make; each
  table file chunk keeps its own CRC and each journal record its own,
  which redo checks when it frames one.
- **`prolly.log_copied_bytes`** (off; superuser): log the bytes, as
  before, for debugging and to compare.
