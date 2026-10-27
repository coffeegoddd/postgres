-- Retiring generations: a collection of the journal, the files not
-- settled, and the settled files of the generations retired, marking only
-- what it collects, from the roots and what the other settled files'
-- sections list in those generations.  The files retired lose their
-- garbage; the others are neither read nor rewritten.
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;
CREATE DATABASE generations_retire_db;
\c generations_retire_db
CREATE EXTENSION test_prolly;

-- Generation A: a node of three leaves.
SELECT test_chunk_put('ret a1') AS a1 \gset
SELECT test_chunk_put('ret a2') AS a2 \gset
SELECT test_chunk_put('ret a3') AS a3 \gset
SELECT test_chunk_put_node(1, ARRAY[:'a1', :'a2', :'a3']) AS na \gset
SELECT test_chunk_move_root(NULL, :'na');
SELECT kept, freed FROM test_chunk_gc(false);
SELECT test_chunk_generation() AS ga \gset

-- Generation B: a node of a1 and a leaf of its own.  Only B's section
-- names a1 now; the rest of A is garbage.
SELECT test_chunk_put('ret b1') AS b1 \gset
SELECT test_chunk_put_node(1, ARRAY[:'a1', :'b1']) AS nb \gset
SELECT test_chunk_move_root(:'na', :'nb');
SELECT kept, freed FROM test_chunk_gc(false);
SELECT test_chunk_generation() AS gb \gset
SELECT name AS b_file FROM test_chunk_generations() WHERE epoch_lo = :gb \gset
SELECT chunks, epoch_lo = :ga AS a, epoch_lo = :gb AS b, xrefs
	FROM test_chunk_generations() ORDER BY epoch_lo;

-- Retiring A frees its garbage and keeps a1, which only B names.
SELECT kept, freed FROM test_chunk_retire(:ga, :ga);
SELECT test_chunk_has(:'a1') AS a1, test_chunk_has(:'a2') AS a2,
	test_chunk_has(:'a3') AS a3, test_chunk_has(:'na') AS na,
	test_chunk_has(:'nb') AS nb, test_chunk_has(:'b1') AS b1;
SELECT chunks, epoch_lo = :ga AS a, epoch_lo = :gb AS b, name = :'b_file' AS b_kept
	FROM test_chunk_generations() ORDER BY epoch_lo;
SELECT test_chunk_verify_generations() AS checked;

-- What came since is collected with it, into a generation of its own.
SELECT test_chunk_put('ret c1') AS c1 \gset
SELECT test_chunk_put('ret dead') AS dead \gset
SELECT test_chunk_put_node(2, ARRAY[:'nb', :'c1']) AS nc \gset
SELECT test_chunk_move_root(:'nb', :'nc');
SELECT kept, freed FROM test_chunk_retire(:ga, :ga);
SELECT test_chunk_has(:'c1') AS c1, test_chunk_has(:'dead') AS dead,
	test_chunk_has(:'a1') AS a1;
SELECT count(*) AS files, count(*) FILTER (WHERE settled) AS settled
	FROM test_chunk_generations();
SELECT test_chunk_verify_generations() AS checked;

-- Retiring B too: a1 is named from the newest file, b1 from B's node.
SELECT kept, freed FROM test_chunk_retire(:ga, :gb);
SELECT test_chunk_has(:'a1') AS a1, test_chunk_has(:'b1') AS b1,
	test_chunk_has(:'nb') AS nb, test_chunk_has(:'nc') AS nc;
SELECT test_chunk_verify_generations() AS checked;

-- A file holding generations retired and others is refused.
SELECT test_chunk_conjoin();
SELECT epoch_lo = :ga AS lo, epoch_hi > :ga AS spans FROM test_chunk_generations();
SELECT test_chunk_retire(:ga, :ga);

\c postgres
DROP DATABASE generations_retire_db;
