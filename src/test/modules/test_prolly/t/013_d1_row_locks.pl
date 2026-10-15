# Copyright (c) 2026, PostgreSQL Global Development Group

# Engine D1 with coordination K1: row locks by key, so writers to different
# rows of one table run at once and their commits are replayed onto each
# other; writers of one row, of one primary key and of one unique value
# take turns, as on heap; an update that keeps the key columns doesn't
# block foreign-key checks; a transaction locking many rows of a table
# locks the whole table instead; tables whose key bytes don't name their
# rows (numeric keys) lock whole tables; deadlocks are found; a uniqueness
# check isn't held up by other rows' writers; and many concurrent transfers
# keep their total.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('row_locks');
$node->init;
$node->append_conf('postgresql.conf', qq{
default_table_access_method = prolly_d1
deadlock_timeout = 100ms
});
$node->start;

$node->safe_psql('postgres', q{
	CREATE TABLE t (id int PRIMARY KEY, v int, u int);
	CREATE UNIQUE INDEX t_u ON t USING btree (u);
	INSERT INTO t SELECT g, 0, g FROM generate_series(1, 10) g;
	CREATE TABLE p (id int PRIMARY KEY, w text);
	CREATE UNIQUE INDEX p_w ON p USING prolly (w);
	CREATE TABLE n (id numeric PRIMARY KEY, v int);
	INSERT INTO n VALUES (1, 0), (2, 0);
	CREATE TABLE parent (id int PRIMARY KEY, note text);
	CREATE TABLE child (id int PRIMARY KEY, parent_id int REFERENCES parent);
	INSERT INTO parent VALUES (1, 'one'), (2, 'two');
});

my $s1 = $node->background_psql('postgres', on_error_stop => 0);
my $s2 = $node->background_psql('postgres', on_error_stop => 0);

# Run sql in s2, expecting it to wait for a lock: it times out.
sub waits
{
	my ($sql) = @_;
	$s2->query("SET lock_timeout = '300ms'; $sql");
	my $waited = $s2->{stderr} =~ /lock timeout/;
	$s2->{stderr} = '';
	$s2->query_safe('RESET lock_timeout');
	return $waited;
}

# Different rows: neither waits, and both commits are kept.
$s1->query_safe('BEGIN; UPDATE t SET v = 1 WHERE id = 1;');
$s2->query_safe(q{BEGIN; SET LOCAL lock_timeout = '5s';
	UPDATE t SET v = 2 WHERE id = 2; INSERT INTO t VALUES (11, 0, 11);});
$s2->query_safe('COMMIT');
$s1->query_safe('INSERT INTO t VALUES (12, 0, 12); COMMIT;');
is($node->safe_psql('postgres',
	q{SELECT array_agg(id || ':' || v ORDER BY id) FROM t WHERE id IN (1, 2, 11, 12)}),
	'{1:1,2:2,11:0,12:0}',
	'writers of different rows run at once, and both commits are kept');
is($node->safe_psql('postgres', q{SET enable_seqscan = off;
	SELECT count(*) FROM t WHERE u BETWEEN 1 AND 12}), '12',
	'index entries of both commits are kept');

# A new table's first writers don't wait for each other either.
$node->safe_psql('postgres', 'CREATE TABLE fresh (id int PRIMARY KEY)');
$s1->query_safe('BEGIN; INSERT INTO fresh VALUES (1);');
ok(!waits('INSERT INTO fresh VALUES (2)'),
	'a new table\'s first writers run at once');
$s1->query_safe('COMMIT');
is($node->safe_psql('postgres', 'SELECT count(*) FROM fresh'), '2',
	'and both commits are kept');

# One row: the second writer waits, then updates the committed version.
$s1->query_safe('BEGIN; UPDATE t SET v = v + 10 WHERE id = 3;');
ok(waits('UPDATE t SET v = v + 1 WHERE id = 3'),
	'a writer of a locked row waits');
is($node->safe_psql('postgres', q{
	SELECT count(*) FROM pg_locks WHERE locktype = 'prollyrow' AND granted
	AND relation = 't'::regclass}),
	'1', 'pg_locks shows the row lock');
is($node->safe_psql('postgres', q{
	SELECT count(*) FROM pg_locks WHERE locktype = 'prollyrow' AND granted
	AND relation = 't_u'::regclass}),
	'1', 'and the value it has in a btree unique index');
$s2->query_until(qr/go/, "\\echo go\nUPDATE t SET v = v + 1 WHERE id = 3;\n");
$s1->query_safe('COMMIT');
$s2->query_safe('SELECT 1');
is($node->safe_psql('postgres', 'SELECT v FROM t WHERE id = 3'), '11',
	'the waiting writer updates the committed version');

# One primary key: the second inserter waits and then finds a duplicate,
# or inserts if the first rolls back.
$s1->query_safe('BEGIN; INSERT INTO t VALUES (20, 0, 20);');
ok(waits('INSERT INTO t VALUES (20, 0, 21)'),
	'an inserter of a key being inserted waits');
$s2->query_until(qr/go/, "\\echo go\nINSERT INTO t VALUES (20, 0, 21);\n");
$s1->query_safe('COMMIT');
$s2->query('SELECT 1');
like($s2->{stderr}, qr/duplicate key value/,
	'it finds the committed key a duplicate');
$s2->{stderr} = '';
$s1->query_safe('BEGIN; INSERT INTO t VALUES (21, 0, 22);');
$s2->query_until(qr/go/, "\\echo go\nINSERT INTO t VALUES (21, 0, 23);\n");
$s1->query_safe('ROLLBACK');
$s2->query_safe('SELECT 1');
is($node->safe_psql('postgres', 'SELECT u FROM t WHERE id = 21'), '23',
	'it inserts once the first inserter rolls back');

# A btree unique value being deleted: an inserter of it waits, then
# inserts once the delete commits.
$s1->query_safe('BEGIN; DELETE FROM t WHERE id = 4;');
ok(waits('INSERT INTO t VALUES (30, 0, 4)'),
	'an inserter of a unique value being deleted waits');
$s2->query_until(qr/go/, "\\echo go\nINSERT INTO t VALUES (30, 0, 4);\n");
$s1->query_safe('COMMIT');
$s2->query_safe('SELECT 1');
is($node->safe_psql('postgres', 'SELECT id FROM t WHERE u = 4'), '30',
	'it inserts the value once the delete commits');

# A prolly unique index: one value at a time.
$s1->query_safe(q{BEGIN; INSERT INTO p VALUES (1, 'x');});
ok(waits(q{INSERT INTO p VALUES (2, 'x')}),
	'an inserter of a value a prolly unique index is being given waits');
$s2->query_until(qr/go/, "\\echo go\nINSERT INTO p VALUES (2, 'x');\n");
$s1->query_safe('COMMIT');
$s2->query('SELECT 1');
like($s2->{stderr}, qr/duplicate key value violates unique constraint "p_w"/,
	'it finds the committed value a duplicate');
$s2->{stderr} = '';

# Updating a non-key column doesn't block a foreign-key check; updating
# the key does.
$s1->query_safe(q{BEGIN; UPDATE parent SET note = 'changed' WHERE id = 1;});
ok(!waits('INSERT INTO child VALUES (10, 1)'),
	'a foreign-key check isn\'t blocked by an update of other columns');
$s1->query_safe('COMMIT');
$s1->query_safe('BEGIN; UPDATE parent SET id = 3 WHERE id = 2;');
ok(waits('INSERT INTO child VALUES (11, 2)'),
	'a foreign-key check waits for an update of the key');
$s1->query_safe('ROLLBACK');

# Past prolly_max_row_locks_per_table rows, the whole table is locked.
$s1->query_safe(q{BEGIN; SET LOCAL prolly_max_row_locks_per_table = 3;
	UPDATE t SET v = v + 1 WHERE id BETWEEN 5 AND 9;});
is($node->safe_psql('postgres', q{
	SELECT mode FROM pg_locks WHERE locktype = 'prollytable' AND granted
	AND mode = 'ExclusiveLock' AND relation = 't'::regclass}),
	'ExclusiveLock', 'a transaction locking many rows locks the table');
ok(waits('UPDATE t SET v = 0 WHERE id = 10'),
	'then writers of other rows wait for it');
$s1->query_safe('COMMIT');

# Numeric keys: 1.0 and 1.00 are one key, so their bytes can't name it,
# and the whole table is locked.
$s1->query_safe('BEGIN; UPDATE n SET v = 1 WHERE id = 1;');
ok(waits('UPDATE n SET v = 1 WHERE id = 2'),
	'a table whose key bytes don\'t name its rows is locked whole');
$s1->query_safe('COMMIT');

# Deadlocks are found.
$s1->query_safe('BEGIN; UPDATE t SET v = 100 WHERE id = 1;');
$s2->query_safe('BEGIN; UPDATE t SET v = 200 WHERE id = 2;');
$s1->query_until(qr/go/, "\\echo go\nUPDATE t SET v = 100 WHERE id = 2;\n");
$s2->query('UPDATE t SET v = 200 WHERE id = 1');
$s1->query('SELECT 1');
like($s1->{stderr} . $s2->{stderr}, qr/deadlock detected/,
	'a deadlock between row locks is found');
$s1->{stderr} = '';
$s2->{stderr} = '';
$s1->query('ROLLBACK');
$s2->query('ROLLBACK');
$s1->{stderr} = '';
$s2->{stderr} = '';

# An aborted insert leaves btree entries whose TID names no row.  A later
# uniqueness check for the same value meets them and knows their writer is
# gone, without waiting for the table's other writers.
$node->safe_psql('postgres', q{
	BEGIN; INSERT INTO t VALUES (40, 0, 40); ROLLBACK;
});
$s1->query_safe('BEGIN; UPDATE t SET v = 1 WHERE id = 3;');
$s2->query_safe('SET lock_timeout = 2000');
$s2->query('INSERT INTO t VALUES (40, 0, 40)');
is($s2->{stderr}, '',
	'a uniqueness check doesn\'t wait for other rows\' writers over an aborted insert\'s entry');
$s2->{stderr} = '';
$s1->query_safe('ROLLBACK');
$s2->query_safe('RESET lock_timeout');

# An update whose row was updated while it waited follows the newer
# version in the mode its own change needs: here FOR NO KEY UPDATE, which a
# FOR KEY SHARE lock on the newer version doesn't block (as Postgres's
# tuplelock-update test expects).
$node->safe_psql('postgres', q{CREATE TABLE ks (id int PRIMARY KEY, data int);
	INSERT INTO ks VALUES (1, 0);});
$s1->query_safe('SELECT pg_advisory_lock(4242)');
$s2->query_safe('SET lock_timeout = 3000');
$s2->query_until(qr/go/, "\\echo go\nUPDATE ks SET data = data + 1 WHERE pg_advisory_lock_shared(4242) IS NOT NULL;\n");
$node->poll_query_until('postgres', q{SELECT count(*) > 0 FROM pg_locks WHERE locktype = 'advisory' AND NOT granted})
  or die 'the update never waited';
$s1->query_safe('UPDATE ks SET data = 100');
$s1->query_safe('BEGIN; SELECT * FROM ks FOR KEY SHARE;');
$s1->query_safe('SELECT pg_advisory_unlock(4242)');
$s2->query('SELECT 1');
is($s2->{stderr}, '', 'an update of a row updated meanwhile isn\'t blocked by a key share lock');
$s2->{stderr} = '';
$s1->query_safe('COMMIT');
$s2->query_safe('RESET lock_timeout');
is($node->safe_psql('postgres', 'SELECT data FROM ks'), '101', 'and it updates the newer version');

# A row locker waits only for the holders it conflicts with, as on heap:
# FOR KEY SHARE gets a row that FOR UPDATE is waiting for (Postgres's
# tuplelock-upgrade-no-deadlock test).
my $s3 = $node->background_psql('postgres', on_error_stop => 0);
$s1->query_safe('BEGIN; SELECT id FROM ks WHERE id = 1 FOR KEY SHARE;');
$s2->query_until(qr/go/, "\\echo go\nBEGIN; SELECT id FROM ks WHERE id = 1 FOR UPDATE;\n");
$node->poll_query_until('postgres', q{SELECT count(*) > 0 FROM pg_locks WHERE locktype = 'prollyrow' AND NOT granted})
  or die 'FOR UPDATE never waited';
$s3->query_safe('SET lock_timeout = 3000');
$s3->query('BEGIN; SELECT id FROM ks WHERE id = 1 FOR KEY SHARE;');
is($s3->{stderr}, '', 'FOR KEY SHARE doesn\'t wait behind a waiting FOR UPDATE');
$s3->{stderr} = '';
$s3->query_safe('ROLLBACK');
$s1->query_safe('ROLLBACK');
$s2->query_safe('ROLLBACK');
$s3->quit;

# FOR KEY SHARE doesn't conflict with an update that keeps the key: a
# REPEATABLE READ foreign-key check of a parent updated meanwhile (another
# column) succeeds, as on heap (Postgres's lock-committed-update test).
$s2->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM parent;');
$node->safe_psql('postgres', q{UPDATE parent SET note = 'changed' WHERE id = 1});
$s2->query('INSERT INTO child VALUES (100, 1)');
is($s2->{stderr}, '', 'a foreign-key check of a parent whose other columns changed meanwhile succeeds');
$s2->{stderr} = '';
$s2->query_safe('COMMIT');
is($node->safe_psql('postgres', 'SELECT parent_id FROM child WHERE id = 100'), '1', 'and the row is inserted');

# An update that waited, and whose row's newest version this command wrote
# itself (a data-modifying WITH), skips it as heap does, rather than fail
# with an unexpected lock status (Postgres's eval-plan-qual test).
$node->safe_psql('postgres', q{CREATE TABLE cte_acc (id text PRIMARY KEY, bal int);
	INSERT INTO cte_acc VALUES ('a', 100), ('b', 200);});
$s1->query_safe(q{BEGIN; UPDATE cte_acc SET bal = bal - 10 WHERE id = 'a';});
$s2->query_until(qr/go/, "\\echo go\nWITH doup AS (UPDATE cte_acc SET bal = bal + 1000 WHERE id = 'a' RETURNING *) UPDATE cte_acc t SET bal = doup.bal + 1 FROM doup;\n");
$node->poll_query_until('postgres', q{SELECT count(*) > 0 FROM pg_locks WHERE NOT granted})
  or die 'the update never waited';
$s1->query_safe('COMMIT');
$s2->query('SELECT 1');
is($s2->{stderr}, '', 'an update meeting its own command\'s newer version skips it');
$s2->{stderr} = '';
is($node->safe_psql('postgres', q{SELECT string_agg(id || ':' || bal, ',' ORDER BY id) FROM cte_acc}),
	'a:1090,b:1091', 'and the rows are as on heap');

# Concurrent transfers between accounts keep the total.
$node->safe_psql('postgres', q{
	CREATE TABLE acct (id int PRIMARY KEY, bal int);
	INSERT INTO acct SELECT g, 1000 FROM generate_series(1, 50) g;
});
$node->pgbench(
	'--no-vacuum --client=6 --transactions=100',
	0,
	[qr/processed: 600\/600/],
	[qr/^$/],
	'concurrent transfers',
	{
		'013_transfer' => q{
\set a random(1, 50)
\set b :a % 50 + 1
\set x random(1, 10)
UPDATE acct SET bal = bal + CASE WHEN id = :a THEN -:x ELSE :x END
  WHERE id IN (:a, :b);
}
	});
is($node->safe_psql('postgres', 'SELECT count(*), sum(bal) FROM acct'),
	'50|50000', 'concurrent transfers keep every row and the total');

$s1->quit;
$s2->quit;
$node->stop;
done_testing();
