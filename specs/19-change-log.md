# 19. Change finder "log": a change log per table

**Status: decided by the user 2026-10-19; built as a stack of pull
requests (part 3). Code references at `9ac631831e`.** Folds find a
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

- **A log relation per prolly table**, made with its table at the points
  that make TOAST tables, named `pg_prolly_log_<table oid>` in `pg_toast`.
  It has the table's access method and is a TOAST-kind relation, so it
  is block-mapped, versioned with its table (its branch's copy shares its
  pages copy-on-write, as the table's does), skipped by the canonical
  form, and dropped with its table. Its columns are a row image
  (`bytea`, never moved out of line) and a count delta (`int4`).
  Logical decoding passes over it (the interface's `is_log()`): its rows
  are bookkeeping, not data, and a TOAST-kind relation would otherwise be
  read as TOAST chunks.
- **An entry per tuple version, at the write.** Heap reports each version
  a write made or replaced (`note_branch_tuple()`, `heapam.c`), and an
  aborted speculative insertion the version it takes back. Each is
  recorded as it is reported, hushed writes too (a row applier's for a
  move): the entry is inserted by the same (sub)transaction as the
  version, so it is seen exactly when the version is. Aborts,
  savepoints, two-phase commit, branch switches inside a transaction,
  standbys and crash recovery need nothing more.
- **What an entry holds:** with a primary key, the key columns as a heap
  tuple (other columns null, values out of line inlined); without one,
  the version as stored, with +1 for a version made and −1 for one
  replaced. A keyless version with a value out of line, or an image too
  large for the log's page, is recorded as "fold in full" instead (no
  image). So is a truncation (made anew or in place) and a rewrite (any
  heap swap: `ALTER TABLE`'s, `CLUSTER`, `VACUUM FULL`, a materialized
  view's refresh), which change rows without a write to note.
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
  transactions, its own transaction, as `pages` does) and the log's
  identity, and is tagged so `pages` and `log` never read each other's.
  A branch's copy of a table has its source's baseline and a copy of its
  log, so it folds from them.
- **A fold from a baseline** of its own reads the log entries its
  snapshot sees:
  - with a primary key, each entry's key is a candidate, looked up in the
    baseline tree and, by the primary key index, in the heap, as `pages`
    looks up its candidates;
  - without one, each entry's row is counted by its delta against the
    baseline's count.
- **It folds in full** when there is no baseline of its own (or one
  folded with another log), the snapshot doesn't see the baseline's fold
  as committed, an entry says so, the table has no log, or during
  recovery.
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
and candidate lookups it removes.

### Known limits

- **Bulk writes to keyless tables** log each row whole until the next
  fold: a load of N rows writes N entries of the row's size.
- **Keyless rows with values out of line** (by content too) are entries
  to fold in full; `pages` reads values stored by content again.
- **The log grows between baselines**, as the changed-page list did: with
  `dolt.transaction_commit` and no other fold (spec 15 doesn't advance
  the baseline), until the next commit, checkpoint or move.
- **Tables `pages` folded before** keep their fold epoch: each page born
  before it is copied and listed once more, and the list isn't pruned.
