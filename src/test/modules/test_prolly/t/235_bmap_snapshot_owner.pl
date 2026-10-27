# Copyright (c) 2026, PostgreSQL Global Development Group

# A captured root's slot names the transaction that captured it, so a crash
# between a keep's captures and its commit leaks no root: the root is held
# by nobody after the restart, and a page sweep reclaims its slot.  A root
# is held while its transaction runs, and one captured by a transaction
# that rolls back is held by nobody the same way.  A standby replays what
# is let go of.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('bmap_snapshot_owner');
$node->init(allows_streaming => 1);
$node->append_conf(
	'postgresql.conf', q{
autovacuum = off
prolly_fold_worker = off
checkpoint_timeout = 1h
wal_keep_size = '256MB'
wal_consistency_checking = 'all'
max_prepared_transactions = 2
});
$node->start;
$node->safe_psql('postgres',
	'CREATE EXTENSION test_prolly; CREATE EXTENSION injection_points');
$node->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('bmap_snapshot_owner_standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{
	CREATE SCHEMA ko VERSIONED;
	CREATE TABLE ko.t (id int PRIMARY KEY, v int);
	INSERT INTO ko.t SELECT g, 0 FROM generate_series(1, 200) g;
	SELECT dolt_commit('-Am', 'c1');
	CREATE TABLE sub (i int);
});
my $rel = sql(q{SELECT pg_relation_filenode('ko.t')});

# the roots held captured of the table
sub kept
{
	my ($n) = @_;
	$n //= $node;
	return $n->safe_psql('postgres', qq{SELECT test_bmap_roots_of($rel)});
}

# the slots of roots whose capturers never committed, reclaimed
sub reclaimed
{
	return sql(q{SELECT test_bmap_roots_reclaim()});
}

is(kept(), '1', 'c1 is kept by one root');
is(reclaimed(), '0', 'no slot to reclaim');

# A keep stopped after its roots are captured, before its commit: the root
# is held while its transaction runs, a sweep included; then a crash.
sub keep_then_crash
{
	my ($msg) = @_;
	my $s = $node->background_psql('postgres');
	$s->query_safe(q{SELECT injection_points_set_local();
		SELECT injection_points_attach('bmap-roots-captured', 'wait')});
	$s->query_until(qr/keeping/, qq{
		\\echo keeping
		UPDATE ko.t SET v = v + 1 WHERE id = 1;
		SELECT dolt_commit('-am', '$msg');
	});
	$node->wait_for_event('client backend', 'bmap-roots-captured');
	my $during = kept();
	sql(q{SELECT freed FROM prolly_page_sweep()});
	my $swept = kept();
	# (a commit flushes the capture's record)
	sql('SELECT txid_current()');
	$node->stop('immediate');
	eval { $s->quit };
	$node->start;
	return ($during, $swept);
}

my ($during, $swept) = keep_then_crash('c2');
is($during, '2', 'a root is held while its transaction runs');
is($swept, '2', 'and a sweep meanwhile leaves it');
is(kept(), '1', 'after the crash, nobody holds the root of c2');
sql(q{SELECT freed FROM prolly_page_sweep()});
is(reclaimed(), '0', 'a sweep reclaimed its slot');

# Not swept, the slot the crash left is reclaimed by the store's reclaim.
sql(q{UPDATE ko.t SET v = 2 WHERE id = 2; SELECT dolt_commit('-am', 'c3')});
is(kept(), '2', 'c1 and c3 are kept');
keep_then_crash('c4');
sql(q{UPDATE ko.t SET v = 3 WHERE id = 3; SELECT dolt_commit('-am', 'c5')});
# (c1 is retired by c5's keep, let go of by the next)
is(kept(), '3', 'c1, c3 and c5 are kept after a crash in c4\'s keep');
cmp_ok(reclaimed(), '>', 0, 'the slots the crash left (one a relation) are reclaimed');
is(reclaimed(), '0', 'once');
sql(q{UPDATE ko.t SET v = 4 WHERE id = 4; SELECT dolt_commit('-am', 'c6')});

# Roots captured by a transaction that rolls back, and by a subtransaction
# rolled back in one that commits, are held by nobody, and reclaimed.
sql(qq{BEGIN; SELECT test_bmap_root_capture($rel); ROLLBACK;
	BEGIN; SAVEPOINT s; SELECT test_bmap_root_capture($rel);
	ROLLBACK TO s; COMMIT;});
is(kept(), '3', 'roots of transactions rolled back are held by nobody');
is(reclaimed(), '2', 'and their slots are reclaimed');

# A prepared transaction's root is held while it is prepared, a sweep
# included: held once it commits, by nobody if it is rolled back.
sql(qq{BEGIN; SELECT test_bmap_root_capture($rel); PREPARE TRANSACTION 'p1'});
is(kept(), '4', 'a prepared transaction\'s root is held');
sql(q{SELECT freed FROM prolly_page_sweep()});
is(kept(), '4', 'and a sweep leaves it');
sql(q{COMMIT PREPARED 'p1'});
is(kept(), '4', 'committed, it is held');
is(reclaimed(), '0', 'and not reclaimed');
sql(qq{BEGIN; SELECT test_bmap_root_capture($rel) AS id \\gset
	SELECT test_bmap_root_release(:id); COMMIT;});
sql(qq{BEGIN; SELECT test_bmap_root_capture($rel); PREPARE TRANSACTION 'p2'});
sql(q{ROLLBACK PREPARED 'p2'});
is(kept(), '4', 'rolled back, it is held by nobody');
is(reclaimed(), '1', 'and reclaimed');

# A root captured in a subtransaction past the ones a backend keeps track
# of (its subtransactions overflowed): held while the transaction runs, a
# sweep included, and by nobody once that subtransaction rolls back.
my $s = $node->background_psql('postgres');
$s->query_safe('BEGIN');
$s->query_safe(join('', map { "SAVEPOINT s$_; INSERT INTO sub VALUES ($_);" } 1 .. 80));
$s->query_safe(qq{SELECT test_bmap_root_capture($rel)});
is(kept(), '5', 'a root of an overflowed subtransaction is held');
sql(q{SELECT freed FROM prolly_page_sweep()});
is(kept(), '5', 'and a sweep leaves it');
$s->query_safe('ROLLBACK TO s40');
is(kept(), '4', 'rolled back, it is held by nobody');
$s->query_safe('COMMIT');
$s->quit;
is(reclaimed(), '1', 'and reclaimed');

# A sweep passes over maps a crash left unfinished: one with no pages, and
# one whose pages are all zeroes.
my $dir = $node->data_dir . '/'
  . sql(q{SELECT 'base/' || oid FROM pg_database WHERE datname = 'postgres'});
for my $f (["999990_bmap", 0, 'no pages'], ["999991_bmap", 16384, 'zeroed pages'])
{
	open(my $fh, '>:raw', "$dir/$f->[0]") or die "open: $!";
	print $fh ("\0" x $f->[1]);
	close($fh) or die "close: $!";
	my ($ret, $out, $err) = $node->psql('postgres',
		q{SELECT freed >= 0 FROM prolly_page_sweep()});
	is($out, 't', "a sweep passes over a map of $f->[2]");
	is($err, '', 'with no error');
	unlink("$dir/$f->[0]");
}

# the standby counts every slot it has (it judges no transaction): as many
# as the primary holds, once it has replayed what was let go of
$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));
is(kept(), '4', 'c3, c5, c6 and the prepared transaction\'s are held');
is(kept($standby), '4', 'the standby replayed the slots let go of');
is($standby->safe_psql('postgres', 'SELECT count(*) FROM ko.t'), '200',
	'and reads the table');

# a hard reset gives the pages c6 kept back, after it all
sql(q{UPDATE ko.t SET v = 100; SELECT dolt_reset('--hard')});
is(sql(q{SELECT sum(v) FROM ko.t}), '11', 'a reset gives the kept pages back');

$standby->stop;
$node->stop;
done_testing();
