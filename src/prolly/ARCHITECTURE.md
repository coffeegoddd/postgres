# Architecture

The layers the prolly engine is built from, bottom to top: what each owns,
its interface, what the prototype has, and what is missing.  The rule
throughout: every layer sits behind an interface, and a layer talks only to
the ones below it, or to the side through a hook.  `LIFECYCLE.md` holds the
measurements and decisions; this file is the map.

```
 9  SQL surface            functions, views, configuration
 8  Operations             the verbs: commit, checkout, merge, diff, sever
 7  Keyframes              physical cache: images, planner, manager
 6  Apply                  tree -> Postgres: moves, replay, restore, rebuild
 5  Capture                Postgres -> tree: stream decoder, barrier, DDL
 4  Working-set model      schema <-> tree: layouts, tags, codec, diff, merge
 3  Version control core   commits, branches, window, segments, remotes
 2  Prolly tree            ordered maps, diff, merge, value trees
 1  Chunk store            content-addressed chunks, refs, durability
```

Layers 1 to 3 know nothing of Postgres.  Layers 5 and 6 are the only ones
that talk to the server.  Layer 4 is the contract between the two halves:
how a schema's contents look as trees.

## The layers

### 1. Chunk store (`chunkstore.h`)

- **Owns:** chunks by hash; refs, pins and roots; orphan collection;
  compaction; the durable state root.
- **Interface:** `ChunkStore` with ops; memory and directory stores.
- **Status:** built and tested.
- **Open:** durability through Postgres's WAL, so the store's root moves
  with a commit record (see the decisions below).

### 2. Prolly tree (`prolly.h`, `chunkfmt.h`)

- **Owns:** ordered maps; the chunking policy; diff and three-way merge;
  value trees; trees that can be severed and repaired.
- **Status:** built and tested; asymptotic costs checked by chunk counts.

### 3. Version control core (`vc.h`)

- **Owns:** commits (with generation, change bytes and time), branches,
  working roots, merge base and log; the window (floors, ceilings, low
  marks), severing into segments, export and discard; history modes KEEP
  and REWRITE; remotes; the saved repo state.
- **Keyframes here are only holds:** a commit in the window may hold an
  image; a sever releases it; REWRITE moves it.  vc never decides which
  commits get one.
- **Status:** built and tested, flat to 1M commits and 1M branches.

### 4. Working-set model (`bench/ws_codec.c`, `ws_diff.c`, `ws_merge.c`, `ws_tags.c`, `ws_viol.c`)

- **Owns:** how a versioned schema maps into trees: table layouts (stored,
  generated, virtual columns), column tags, the row codec, schema objects,
  sequences, stored constraint violations; schema-aware diff and merge,
  including the evaluators a merge needs (`WsEval`).
- **Status:** works, but lives in the harness.  It must become a library
  behind an interface: layers 5 to 8 all depend on it.

### 5. Capture: Postgres to tree (`bench/ws_stream.c`, `ws_logical.c`, `pgoutput.c`)

- **Owns:** the streaming decoder on a logical slot; the barrier that makes
  a commit exactly "every transaction before it"; DDL carried in the
  stream by an event trigger; COPY capture as the slow reference.
- **Status:** works in the harness, as a libpq client.
- **Missing:** a home inside the server (see the decisions).

### 6. Apply: tree to Postgres (`bench/ws_move.c`, `contrib/prolly_pages`, `bench/prolly_tags.sql`)

- **Owns:** moving the working set to a commit: schema objects, rows by
  replay, sequences, the objects that need rows; full rebuild; page
  restore; the rule that version control runs no user code (row apply
  below triggers and rules, `prolly.quiet()`, copied generated values).
- **Status:** the C pieces are a real extension; the orchestration is
  harness code.

### 7. Keyframes (`image.h`, `kfplan.h`, `prolly_get_pages_frozen`)

- **Owns:** commit images (frozen pages in a page map, shared between
  images); the planner that prices replay against restore and learns its
  costs.
- **Missing: the manager.**  It decides when to capture, keeps the images
  within a budget inside the window, sweeps images of another format, and
  orders an image's durability against the state that names it.  It talks
  to layer 3 only through holds, restores through layer 6, and captures
  under layer 5's snapshot.

### 8. Operations (`bench/rebuild.c`, `bench/winbench.c` today)

- **Owns:** the verbs, each a sequence over the layers below.  A checkout
  is barrier (5), plan (7), move (6), advance the working root (3).  Also
  when to sever, and backups.
- **Status:** harness programs only.  This is why the prototype has felt
  scattered: each piece was wired into a harness, not into this layer.

### 9. SQL surface

- **Owns:** the functions users call (commit, checkout, branch, merge,
  reset), the system views (log, diff, status, conflicts), configuration
  (window policy, history mode, keyframe budget), permissions.
- **Status:** not started.

## Across the layers

- **Durability and crash order:** layers 1, 3 and 7.  What is durable is
  what a saved state names; everything else is collected.
- **No user code:** layers 6 and 8; merge's generated columns are the one
  documented exception.
- **Security, grants, branch permissions:** to prototype.
- **Concurrent branches:** copy-on-write working sets would replace the
  working-set half of layers 5 and 6.  Layers 1 to 4 and 7 to 9 stay.

## Order

1. This map.
2. Lift layers 4, 5, 6 and 8 out of `bench/` behind interfaces; the
   harness programs become thin drivers over them, and every existing
   check keeps passing.
3. The keyframe manager in layer 7.

## Decisions open

Listed with a recommendation each; until decided, the lift keeps both
options possible.

1. **Where layers 5 and 8 run:** inside the server (an extension: the
   decoder as a background worker, the verbs as backend functions) or
   outside it (a client process over libpq, as now).
2. **How layers 4 to 6 reach Postgres:** through an interface for running
   SQL and reading results, implemented over libpq and over SPI, so the
   same code runs in the harness and in the server.
3. **Where the chunk store lives:** files beside the cluster with their own
   fsync (as now), or under Postgres's WAL so the store's root and a
   commit record are one atomic step.
4. **Concurrent branches in the first build:** copy-on-write working sets
   now, or one working set per versioned schema first.
5. **Keyframe defaults:** off unless configured, captured in the
   background after a commit (not on the commit's path), within a byte
   budget.
