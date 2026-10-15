# Copyright (c) 2026, PostgreSQL Global Development Group

# Engine D1 with coordination K1: row locks by key, so writers to different
# rows of one table run at once and their commits are replayed onto each
# other; writers of one row, of one primary key and of one unique value
# take turns, as on heap; an update that keeps the key columns doesn't
# block foreign-key checks; a transaction locking many rows of a table
# locks the whole table instead; tables whose key bytes don't name their
# rows (numeric keys) lock whole tables; deadlocks are found; and many
# concurrent transfers keep their total.

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

# One row: the second writer waits, then updates the committed version.
$s1->query_safe('BEGIN; UPDATE t SET v = v + 10 WHERE id = 3;');
ok(waits('UPDATE t SET v = v + 1 WHERE id = 3'),
	'a writer of a locked row waits');
is($node->safe_psql('postgres', q{
	SELECT count(*) FROM pg_locks WHERE locktype = 'prollyrow' AND granted}),
	'1', 'pg_locks shows the row lock');
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
	AND mode = 'ExclusiveLock'}),
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
