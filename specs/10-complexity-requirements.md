# 10. Complexity requirements for version control

**Requirement (user, 2026-10-14):** every version-control operation is, at
worst, as cheap as Dolt's in time and in space, and ideally cheaper. Being
true to Postgres must not cost an asymptotic step: Postgres-only machinery
(TID maps, non-prolly indexes, local catalogs, WAL) can add constant factors,
never a larger bound. An operation that can't yet meet its bound says so in
the status column below, with the roadmap step that brings it there.

The Dolt bounds come from a source-level analysis of Dolt at `51cc4458b740`
(2026-10-14, kept outside this repository): source-derived estimates, not
measurements. They count tree and store operations, treating a chunk lookup
as one operation, and assume bounded entries and ordinary hash
distributions.

## Symbols

| Symbol | Meaning |
|---|---|
| N | Entries in the relevant tree (rows of a table with a primary key) |
| Δ | Changed entries; for a merge, the changes on both sides since the base |
| T | Tables; s: tables being staged; d: tables changed |
| R | Refs (branches, tags, working sets) |
| H | Commits in the relevant history; a: ancestry entries examined; h: commits visited |
| I | Indexes per affected table |
| M | Table and schema metadata work |
| B | Bytes transferred; E: chunk references examined |

## Time

"Ours (required)" is the bound we must meet; "aim" is where the design can
do better than Dolt. Two status columns: D1's, as of 2026-10-14, kept as
the record of what that design met (D1 is removed by #183; its last state is
`cff08be5c3`); and the page design's
(`11-pages-and-dolt-snapshots.md`), as of 2026-10-17, from the PRs that
built it and the measurements in `13-keyless-tables.md`. For the
page design (`11-pages-and-dolt-snapshots.md`), built through its step 5 on
2026-10-17: status, diff and commit fold only the pages changed since the
branch's last fold (counted by the change finder), tables without a
primary key included (#136; a fold in the transaction that wrote a keyless
table's rows still leaves the next fold to read it in full);
creating a branch writes only its head until it is first used; merges,
cherry-picks and reverts apply the tree diff as rows; fetch, push, clone
and pull are not built (step 6). Step 8 rechecks every row here against
that design.

| Operation | Dolt | Ours (required; aim) | How the design meets it | Status (D1, 2026-10-14) | Page design (2026-10-17) |
|---|---|---|---|---|---|
| Compare two known root hashes | O(1) | O(1) | Roots are content hashes; "did the schema change?" is one comparison of catalog sub-roots (question 33) | Built (root layout) | Built: canonical roots in the chunk store, compared by hash |
| Create a branch | O(R) (a case-insensitive name scan, then an O(log R) ref update) | O(R); aim O(log R) | A new ref names an existing root; no rows are copied. Aim: refs kept so a name lookup is O(log R) (question 32's refs file is rewritten whole, O(R), until then) | 4g | Built (#124): only the head is written, a refs-tree put, O(log R); the copies are made on first use, O(versioned objects), pages shared copy-on-write, no rows copied (3.5–4.8 ms measured, spec 13). A first use under repeatable read or serializable, once a transaction its snapshot doesn't see has committed, asks each table's change log whether such a transaction wrote it, O(log entries) (a table without a log has every page read, O(N)), and fails with a serialization failure if one did, to be retried (#567; before, it rewrote every row of those tables, #330). The copies are of the branch nearest the head (#491): one whose working set is known to be the head's root writes and folds nothing (5,000 of 20,000 rows apart: none written, against 5,000 from the session's branch) |
| Switch to a branch | O(log R) root switch; carrying uncommitted changes O(T log T) | O(log R) plus cache reset; carrying changes O(d log T) | Installing a working-set root; caches reset as after an invalidation overflow (`09-versioned-catalogs.md`); non-prolly indexes serve every branch, so nothing is rebuilt (question 37) | 4g | Built: a switch of the session branch with cache resets; leaving a dirty branch checkpoints it, a fold of its changed pages (#113). Clean, 0.45–0.6 ms; with 10,000 rows changed, 45–63 ms (spec 13, part 7) |
| Status | O(M), M = O(T + d log d) by name; O(T²c) with unmatched names (rename matching); O(TF) for F foreign keys | O(M); aim O(1) when nothing changed, O((d + 1) log T) otherwise | Sub-root hashes answer "anything changed?" in O(1); a diff of the directory trees finds the d changed relations in O((d + 1) log T); renames matched by content hash in a hash table, O(u + v), not O(uvc); foreign keys found through `pg_depend`'s index, not a scan per table | 4g (directory diff needs 4e's subtree-skipping diff) | Built (#98, #112, #136, #137): each table folds its pages changed since the baseline, O(changed pages + Δ); "anything changed?" isn't O(1) yet (a fold over every table, cheap when nothing changed). 10,000 rows changed, 22–34 ms |
| Stage s tables | O(M + s log T + log R) | O(s log T + log R); aim without the database-wide O(M) term | Staging updates s directory entries; only the staged tables' metadata is examined | 4g | Not applicable: no staging area (user, 2026-10-15) |
| Commit (staged data materialized, one parent) | O(M + log H + log R) | O(M + log H + log R) | A commit's canonical root refers to the working set's trees themselves (structural sharing, `09-versioned-catalogs.md`); the canonicalizer runs over changed catalog rows only; ancestry as a persistent map updated in O(log H) | 4g. Tables with rows not yet canonical (written before an ADD COLUMN with a default or a DROP COLUMN) are rewritten at commit, Ω(N) per such table, as a Dolt schema rewrite is | Built (#95, #112): the fold of changed pages, O(changed pages + Δ log N), plus the canonical description of every versioned object, O(M); a table's first commit reads it in full, and so does one after DDL that changes its rows' bytes; DDL that changes none (an index, a constraint, a nullable column added, an enum's label added, `VACUUM FULL`) keeps the baseline (#485). 10,000 rows, 22–37 ms. Descriptions, the dependency tree and the top-level trees cached per backend (#483): an untouched table costs its change finder's open and a cache probe; 1000 untouched tables add 18 ms to a one-row commit (145–150 ms before), `--enable-cassert` build |
| SQL transaction commit (publishing the working set) | Concurrent transactions: O((I + 1)(Δ + 1) log N) working-set merge | O((I + 1)(Δ + 1) log N) | Changed trees materialized before the commit lock; over a newer head, the write set is replayed (row locks keep writers apart), O((I + 1) Δ log N) | Built for one writer per table (4d); concurrent writers and replay in 4e | Met: the working set is Postgres pages, committed as heap's are (MVCC and row locks), with no merge at commit |
| Working set vs head (diff, status) | O((Δ + 1) log N): the working set is a tree | O(changed pages + Δ); "anything changed?" O(1) a table | Pages changed since the head are copied and listed per relation; their old and new versions give candidate keys, compared by row (`11-pages-and-dolt-snapshots.md`, part 2). Changed pages are a small multiple of Δ | Designed 2026-10-16 (step 2 lists, step 4 diff) | Built (#111, #112, #136): as designed, counted by the change finder's pages and candidates |
| Row diff | Sparse: O((Δ + 1) log N); full traversal O(N) | O((Δ + 1) log N) | Skipping matching internal subtrees, not only leaves; streaming | Built (4e.1): subtrees with equal hashes are skipped unread; a conformance check counts the nodes read (one change in a 4-level tree of 837 nodes: 8, two root-to-leaf paths; eight changes: 40; the leaf walk read 1,674 for one) | Built (#93): trees as chunks, subtrees with equal hashes skipped unread. Two commits 10,000 rows apart, 2.8–3.4 ms |
| Find merge base, test fast-forward | O(log H + a), worst O(H) | O(log H + a); aim O(log H) | Ancestry closure as a persistent map, with seeks rather than a walk (Dolt notes seeking would improve its own path); fast-forward by closure membership, O(log H) | 4g | Built: a walk of the commit graph with a visited set, O(a) (#116 removed an exponential walk); the persistent closure isn't built |
| Fast-forward merge | Ancestry check + metadata + O(log R) | The same; aim O(log H + log R) | As above, then publishing the destination root | 4g | Not met: moving a branch to another commit writes the difference into its pages as rows, O(Δ) row writes (a fast-forward of 10,000 rows, 50–68 ms, against Dolt's under a millisecond; spec 13, part 5). To a root another branch's working set is known to be, the tables whose rows differ share that branch's pages where that costs less than writing the rows (#491): O(blocks of the changed tables), no rows written (10,000 of 100,000 rows: 87–94 ms writing rows, 25 ms shared, `--enable-cassert` build); O(1) isn't a goal (user, 2026-10-20) |
| Three-way merge | O(M + a + log H + (I + 1)(Δ + 1) log N) plus commit work; schema rewrites Ω(N), index rebuilds O(N log N) each | The same bound | Diff both sides with subtree skipping, apply patches reusing unchanged subtrees (handling the last node of a level that ends with its input, not at a boundary); prolly indexes merged as trees; non-prolly indexes updated with the Δ merged rows, O(Δ log N), never rebuilt for a data-only merge | 4g | Built (#104, #109, #116, #119): diffs of both sides, the result applied as rows; 10,000 rows on each side, 53–71 ms |
| Cherry-pick, revert | One three-way merge plus commit work per commit | The same | As three-way merge | 4g (V3) | Built (#106, #117): as three-way merge; 10,000 rows, 67–104 ms |
| Rebase k commits | Sum of k replays and commits | The same | As three-way merge, per commit | 4g (V3) | Not built |
| Log, history | O(h log(w + 1)) (priority queue of width w); linear history O(h); worst O(H log H) | The same | Commit graph walked with a priority queue by commit order | 4g | Built (`dolt_log`) |
| Fetch, push | O(E + B) plus storage and ref costs | O(E + B) | Chunks sent by content hash; descent stops at chunks the other side has; out-of-line values found through the node format's reference contract (question 48) | Not built (V4) | Built for file remotes (#172): the refs' walk from each head, declining what the other side has (#171), O(E + B); not batched yet |
| Full clone | O(B) plus file and ref metadata | O(B) | Canonical chunks copied; TID maps not built (implicit maps, question 41); local catalogs rebuilt from canonical schema, O(catalog rows) | Not built (V4). Exception: a non-prolly index on a cloned table is rebuilt, O(N log N), as in a restore (question 41); tables whose indexes are prolly indexes meet O(B) | Built for file remotes (#174): a fetch of every branch, O(B), each branch head-only until first used, the default one's copies made then |
| Pull | Fetch + merge | Fetch + merge | As above | Not built (V4) | Built for file remotes (#172) |

## Space

| Operation | Dolt | Ours (required) | Notes | Page design (2026-10-17) |
|---|---|---|---|---|
| Streaming tree diff | O(log N) cursor state | O(log N) | Excluding caches and collected output | Built |
| Status, table deltas | O(Tc) metadata | O(Tc); aim O(dc) | Only changed relations' schemas are loaded | Built |
| One-parent commit | O(log H + log R) new nodes; row trees shared | O(log H + log R + d log T) new nodes | d log T: the canonical directory's changed paths; row trees shared | Built: changed chunks only; the journal keeps every commit's (no GC until step 7) |
| Sparse merge | O((I + 1)(Δ + 1) log N) new nodes, plus conflict records | The same | | Built |
| History traversal | O(h) | O(h) | | Built |
| Fetch, push | O(E) seen addresses | O(E) | Plus buffers; disk growth follows missing chunks | Built, not bounded: the seen sets in memory, growing with the chunks moved, and a temporary file of what was received (`11-pages-and-dolt-snapshots.md` 7.11) |
| SQL transaction (pending writes) | Bounded by flushing (about 64K entries) | Bounded (question 47) | **Not met:** D1 holds a transaction's writes in memory without a limit. Fixed in 4e | Met: writes are heap-like pages, bounded as heap's are |
| Per-branch local state | None | O(rows written on the branch) of TID-map entries | Postgres-only: TID maps are local and per working set; clone avoids building them (question 41) | Pages a branch changed (copy-on-write) and their changed-page lists; no TID maps |

## How it is checked

- **Gate 7** (`06-benchmarking.md`): the V suite sweeps N with Δ fixed and Δ
  with N fixed, and checks each operation's growth against its bound.
- Work is counted, not timed, where it can be: engines expose counters of
  nodes read and written (built in 4e.1: the tree interface counts into
  the caller's counters, and `prolly_node_counters(access_method)` returns
  a backend's for an engine), so a requirements test can assert, for example,
  that a one-row diff in a table of N rows reads O(log N) nodes, and a
  branch creation reads none of any table's nodes. Timing is reported beside
  the counts.
- Row-tree work, metadata work, ancestry work and storage work are measured
  separately, as the Dolt analysis recommends, so a bound met by one part
  isn't hidden by another.
