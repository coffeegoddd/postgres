# PostgreSQL with prolly version control

This fork develops one hybrid storage engine: PostgreSQL pages and native
indexes for the working state, with canonical prolly trees for committed
history. There is no current plan to build competing storage implementations.
Heap and Dolt remain comparison baselines.

[GitHub issues](https://github.com/coffeegoddd/postgres/issues) are the source
of truth for proposals, decisions, remaining work, and acceptance criteria.
Performance and complexity goals are tracked in
[#749](https://github.com/coffeegoddd/postgres/issues/749).
Update the relevant issue when a decision changes; keep usage and implemented
contracts beside the code and tests:

- [Storage implementation](src/backend/access/prolly/README)
- [Requirements and tests](src/test/prolly/README)
- [Benchmark usage and evidence contracts](src/tools/prolly_bench/README.md)
- [Dolt test ports](src/tools/prolly_bench/corpus/dolt/README.md) and
  [compatibility catalog](src/tools/prolly_bench/corpus/dolt/divergences.md)

Preserve PostgreSQL semantics and document necessary Dolt adaptations. Canonical
state roots must depend on logical content, not local OIDs, physical placement,
or the sequence of writes; commit IDs additionally include history and metadata.
Requirements belong in executable tests, and bug fixes need regression coverage
that fails without the fix. Dolt and doltlite are references for analysis, not
sources from which to copy code or formats.

The retired design documents remain available in
[Git history](https://github.com/coffeegoddd/postgres/tree/28cca9345d/specs).
They are historical evidence, not current instructions or a parallel roadmap.

PostgreSQL Database Management System
=====================================

This directory contains the source code distribution of the PostgreSQL
database management system.

PostgreSQL is an advanced object-relational database management system
that supports an extended subset of the SQL standard, including
transactions, foreign keys, subqueries, triggers, user-defined types
and functions.  This distribution also contains C language bindings.

Copyright and license information can be found in the file COPYRIGHT.

General documentation about this version of PostgreSQL can be found at
<https://www.postgresql.org/docs/devel/>.  In particular, information
about building PostgreSQL from the source code can be found at
<https://www.postgresql.org/docs/devel/installation.html>.

The latest version of this software, and related software, may be
obtained at <https://www.postgresql.org/download/>.  For more information
look at our web site located at <https://www.postgresql.org/>.
