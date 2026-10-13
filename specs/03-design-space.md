# 03. Design space

These axes come from a coworker's review of the coordination-in-the-tree idea.
They are physical-design questions that option 8 left open. Each candidate
design in `04-candidate-designs.md` is one answer per axis. Items marked
*(inferred)* were not in the coworker's notes but follow from them.

## Axis 1: copy-on-write or in-place update

- **1a. Always copy-on-write.** Every edit copies the leaf-to-root path. Dolt
  today.
- **1b. Reference counts decide.** A node referenced once may be edited in
  place; a shared node is copied first. Open question from the coworker: can
  counts be maintained efficiently? A branch delete decrements at the top of a
  tree, and propagating that down is expensive. Options:
  - eager propagation (cost proportional to the released tree);
  - lazy propagation in a background VACUUM-like process (coworker's
    suggestion);
  - pending decrements logged and applied only when an allocation needs a
    count *(inferred)*.
- **1c. Ownership instead of counts** *(inferred)*. A node created after the
  newest pinned snapshot is private to its writer and can be edited in place
  (the birth-time rule used by ZFS and btrfs). No counts to maintain.

**Constraint:** an in-place edit must not be visible to a reader holding an
older root. So an in-place engine either edits only nodes no snapshot can
reach, or keeps version information inside the page (axis 3d).

## Axis 2: when nodes are written, and what the WAL contains

- **2a. Physical WAL, full chunks.** Every update writes the rebuilt chunk.
- **2b. Physical WAL, deltas.** Every update writes a byte-level change.
- **2c. Logical WAL.** Every update writes row mutations; nodes are built and
  flushed at checkpoint.

The coworker's point: a chunk's content address requires the whole chunk, so
it must be materialized at some point anyway. Does a smaller synchronous write
(2b, 2c) buy much? Sub-question *(inferred)*:

- **2-i.** Hash at every update.
- **2-ii.** Hash lazily: dirty nodes stay unhashed and are identified by
  location until a checkpoint, Dolt commit, diff or push needs the hash.

## Axis 3: unhashed metadata attached to nodes

- **3a.** None.
- **3b. Read hints.** Internal nodes store each child's physical location next
  to its hash, skipping the chunk-index lookup (the coworker notes that lookup
  is expensive relative to a direct jump).
- **3c.** Lock information on the node.
- **3d.** MVCC data (xmin/xmax-style) on the node.

Second choice: where it lives. An unhashed section of the persisted page, or
a separate in-memory structure.

## Axis 4: how far coordination reaches

- **4a. Per working set / branch.** Each branch head has its own lock state.
- **4b. Per chunk, across working sets.** Transactions on different branches
  coordinate when they touch the same chunk. The coworker suspects coordination
  cannot be isolated per working set.
- **4c. Split by layer** *(inferred)*. Logical row locks per branch; physical
  latches per chunk. Cross-branch latching is needed only when shared chunks
  are edited in place (1b/1c). Pure copy-on-write never writes a shared chunk.

## Axis 5: layout in the file

- **5a. Append-only content-addressed store + GC.** As in Dolt's journal and
  table files.
- **5b. Fixed-size blocks with in-place updates.** E.g. 512-byte runs, chunks
  spanning several. Average waste ≈ half a run per chunk (~256 B), plus a few
  allocation bits per block.
- **5c. Variable-size allocation.** Tighter, but a general allocator with
  fragmentation.

## Axis 6: target storage

- **6a.** Traditional SSD / block device (Dolt's target today).
- **6b.** Local NVMe.
- **6c.** Tiered or cloud object storage.
- **6d.** Disaggregated storage and compute.

The coworker's point: the best answer to the other axes depends strongly on
this one. Handled in `05-storage-targets.md`.

## Couplings between axes

- In-place updates (1b/1c) require 5b or 5c.
- **In-place edits still change hashes.** Editing a leaf changes its hash, so
  the parent's pointer changes, up to the root. In-place avoids *allocating*
  a new path, not *rewriting* it, unless parents point to children by location
  (3b becomes the real pointer) and hashing is lazy (2-ii). Then hashes are a
  derived index computed on demand. That is a large architectural shift.
- In-place page overwrites risk torn pages, so 1b/1c + 5b need full-page images
  in the WAL (like `full_page_writes`) or a double-write scheme. A pure logical
  WAL (2c) alone does not protect against them.
- 4b matters only with in-place updates.
- In-page lock/MVCC data (3c/3d) fits in-place pages (5b) naturally; under
  copy-on-write it would have to be carried across every node copy.
