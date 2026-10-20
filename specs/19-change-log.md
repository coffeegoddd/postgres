# 19. Change finder "log": a change log per table

**Status: decided by the user 2026-10-19; built as a stack of pull
requests (part 3), `log` the profile's change finder. Code references at
`39de7462e5`.** Folds find a
table's changes from a log of the rows each write changed, instead of
from old versions of pages.

## 1. Why

The change finder `pages` (`changes_pages.c`) finds a fold's changes from
the pages changed since the baseline:

- **Baseline copies.** The first change to a page after a fold copies the
  page, even when no other branch shares it, so the change finder can
  read the old version (`bmapprepareforchange()`, `bmap.c`). That costs up
  to one extra 8 KB page per changed page between folds.
- **Candidates by page.** Every tuple on both versions of a listed page is
  a candidate, looked up in the baseline tree and in the heap. A page of
  100 tuples with one changed row costs about 200 lookups.
- **Full folds.** A table is read in full when a candidate's key can't be
  read from an old version (a key stored out of line that VACUUM removed,
  or any out-of-line value of a keyless table).

## 2. The design

Two interfaces, each selected by the table AM's profile
(`prolly_config.c`):

- **Change log** (`prolly_changelog.h`, new): a table's row changes,
  recorded as they are written. Implementation `relation`.
- **Change finder** (`prolly_changes.h`): implementation `log`, beside
  `pages`. Which one folds is the profile's, overridden by the developer
  setting `debug_prolly_change_finder`, so both can be tested and
  measured.

### 2.1 The change log, `relation`

- **A log relation per prolly table** (or materialized view), made with
  its table at the points that make TOAST tables, named
  `pg_prolly_log_<table oid>` in `pg_toast`; for a table that has none
  (made before this stack, or by `SET ACCESS METHOD prolly`), made by the
  next `ALTER TABLE` that takes a lock writers wait for (ShareLock or
  stronger: under a weaker one a writer whose transaction is open
  wouldn't see the new log, and its rows would be in no entry).  None is
  made as `pg_upgrade` restores a table (it preassigns relation numbers).
  It has the table's access method and is a TOAST-kind relation, so it
  is block-mapped, versioned with its table (its branch's copy shares its
  pages copy-on-write, as the table's does), skipped by the canonical
  form, and dropped with its table. Its columns are a row image
  (`bytea`, never moved out of line) and a count delta (`int4`).
  Logical decoding passes over it (the interface's `is_log()`, asked in
  critical sections, so it neither errors nor allocates): its rows are
  bookkeeping, not data, and a TOAST-kind relation would otherwise be
  read as TOAST chunks.  It takes its table's owner, tablespace and
  autovacuum settings, as a TOAST table does (`relid()`), and a branch's
  making locks it after every table, as a writer does.
- **An entry per tuple version, at the write.** Heap reports each version
  a write made or replaced (`note_branch_tuple()`, or with the versions
  made in hand `note_branch_made()`, `heapam.c`), and an aborted
  speculative insertion the version it takes back. Each is recorded as
  it is reported, hushed writes too (a row applier's for a move): the
  entry is inserted by the same (sub)transaction as the version, so it is
  seen exactly when the version is. A write's entries are inserted
  together (a `COPY` batch is one multi-insert into the log). Aborts,
  savepoints, two-phase commit, branch switches inside a transaction,
  standbys and crash recovery need nothing more. A note's memory is freed
  as it ends. A transient heap a rewrite fills has no log.
- **What an entry holds:** with a primary key, the key columns as a heap
  tuple (other columns null, values out of line inlined, compressed ones
  still compressed), one entry for an update that kept its key; without
  one, the version as stored, with +1 for a version made and −1 for one
  replaced, its values out of line as their hash pointers: by content in
  the chunk store, where nothing deletes them with the row, and a
  collection keeps every tree a pointer on the log's pages names (#508).
  A keyless version with a value out of line otherwise (not a versioned
  table's), or either image too large for the log's page (a key's compressed values needn't be as
  its index holds them), is recorded as "fold in full" instead (no
  image). So is a truncation (made anew or in place) and a rewrite that
  may change rows (`ALTER TABLE`'s heap swap, a materialized view's
  refresh, `REPACK CONCURRENTLY`), which change rows without a write to
  note. `CLUSTER` and `VACUUM FULL` change none: entries name rows by
  key or image, so they still hold (#485).
- **Reset:** a branch's first use that gives its copy of the table new
  storage (repeatable read, a commit its snapshot doesn't see,
  `empty_copy()`) gives the log new storage too, with one "fold in full"
  entry, under a writer's lock (the log's OID is every branch's).
- **Read:** the entries a snapshot sees. **Consume:** delete them. The
  canonicalizer consumes a table's log once a fold that begins a new
  baseline has read it, whichever finder folded: what that fold saw is
  in the baseline, and what it didn't see (transactions it didn't see as
  committed) stays for the next.
- **Identity:** the log relation's OID, so a baseline knows the log it
  was folded with. A branch's copy of the log keeps the OID, as it keeps
  the entries; a log made later (for a table that had none) doesn't.

### 2.2 The change finder, `log`

- **Its state** records the snapshot of its fold (xmax, running
  transactions, its own transaction, as `pages` does), the log's identity
  and flags, and is tagged so `pages` and `log` never read each other's.
  Reclamation walks each state with every change finder (each passes over
  states it didn't write), not the one configured now. A branch's copy
  of a table has its source's baseline and a copy of its log, so it folds
  from them.
- **A fold by an older snapshot.** Consuming entries means a fold's
  snapshot must see its baseline's fold: a snapshot that doesn't (a
  repeatable-read transaction's, or a statement's taken before it waited
  behind another fold) may miss rows committed meanwhile whose entries
  that fold consumed. Such a fold reads the table in full by its snapshot,
  and its baseline is marked unsettled, so the next fold reads the table
  in full again, by its own.
- **A fold from a baseline** of its own reads the log entries its
  snapshot sees:
  - with a primary key, each entry's key is a candidate, looked up in the
    baseline tree and, by the primary key index, in the heap, as `pages`
    looks up its candidates;
  - without one, each entry's row is counted by its delta against the
    baseline's count.

  The entries read are kept once per row (a key's, or a keyless row's
  with its count summed), so a fold's memory is of the rows changed, not
  of the writes.
- **It folds in full** when:
  - there is no baseline of its own, or one folded with another log (or
    none: the table has no log);
  - the snapshot doesn't see the baseline's fold as committed, or the
    baseline is unsettled;
  - the table's catalog row was changed by a transaction the snapshot
    doesn't see (a truncation or rewrite whose entry the snapshot can't
    see, though it reads the new storage);
  - an entry says so;
  - a keyless table meets a key's entry (delta 0), or a row's count would
    go below none (the keying changed meanwhile);
  - an entry's key column is null (it was written under another key);
  - the primary key has no equality operator to look up by;
  - a keyless table's entries name more rows than the table has;
  - what it holds of the changes passes `maintenance_work_mem`.

  A keyed table's many changes (past a tenth of the baseline's rows) are
  merged with the baseline in one pass, not applied one by one (#509).
  A full fold sorts the rows in `maintenance_work_mem`, spilling past it
  (#484). Each is counted in `pg_stat_prolly` by why (#486). A hot
  standby folds from the baseline too, its log and baseline replayed, the
  trees it builds kept in the transaction's memory (#506).

  A change of what rows' bytes depend on is caught by the baseline's
  shape hash (`table_root()`, `canon_folded.c`) before the finder is
  asked, which is what makes it safe to read old images by the table's
  descriptor now: the keying's `row_shape()` (keyed or not, the key's
  columns and order, the live columns' numbers, types and missing
  values), the encoders and the columns' type definitions. DDL that
  changes none of those (an index, a constraint, a default, a trigger, a
  policy, reloptions, statistics, storage) keeps the baseline (#485).
  Adding a column without a default, and an enum's label added, change
  no row's bytes but do change the shape: still a full fold.
- **A verifier** (`fold_verify.c`, #513) folds tables from their
  baselines and in full by one snapshot, at `prolly.fold_verify_rate` in
  autovacuum or by `dolt_verify_folds()`: a mismatch is warned of,
  counted, and its table read in full at its next fold.
- **No baseline copies:** it never begins a fold in block-mapped storage,
  so only sharing between branches copies pages.
- **The transaction's own changes** (`fold_rows()`, spec 15): from the
  TIDs it is given, as `pages` does.

## 3. The stack

1. This spec.
2. **Counters**: pages copied for a fold's baseline and for sharing, in
   `pg_stat_prolly`.
3. **The change log interface**: its header, a stub, the registry and the
   profile; the change finder chosen by configuration, not named in code.
4. **The change log `relation`**: the log relation, entries at each write,
   consumption at baseline folds.
5. **The change finder `log`**, with its tests next to `pages`'s.
6. **`log` by default**; `pages` kept for comparison through the setting.

The performance cost to measure (spec 17's S suite, with the counters of
2): one log insert per changed tuple version, against the page copies
and candidate lookups it removes. Layer 4 adds that cost before layer 6
removes the page copies; its S suite counters (WAL records and bytes,
buffer hits and extends per write) are accepted in `perf_accept.toml`
with this reason, after a write's entries were batched and the versions
made passed in hand.

## 4. Known limits

- **Bulk writes to keyless tables** log each row whole until the next
  fold: a load of N rows writes N entries of the row's size.
- **Keyless rows with values out of line** other than by content (not a
  versioned table's) are entries to fold in full; `pages` reads values
  stored by content again.
- **The log grows between baselines**, as the changed-page list did: with
  `dolt.transaction_commit` and no other fold (spec 15 doesn't advance
  the baseline), until the next commit, checkpoint or move.
- **Tables `pages` folded before** keep their fold epoch: each page born
  before it is copied and listed once more, and the list isn't pruned.
- **Tables made before this stack have no log** until an `ALTER TABLE`
  under a strong enough lock makes one (likewise after `pg_upgrade`):
  under `log` they fold in full on every commit, which is correct but a
  performance cliff after an upgrade.
- **Consumed entries are freed only by vacuuming the log**, which sets
  no options of its own: autovacuum gives it its table's, and where the
  table leaves them unset, a vacuum threshold of 1000 dead entries and no
  scale factor, so it's vacuumed soon after a fold consumes a load's
  entries; `VACUUM` of the table
  vacuums its log too, as its TOAST table (#480). Until then each fold
  walks the dead entries (`pg_stat_prolly`'s `log_pages_read`), and a
  long-running transaction holding back the horizon keeps them.
- **An update's or delete's old version** is read again by its TID to
  make its entry (heap has let go of its page by then).
