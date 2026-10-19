# Copyright (c) 2026, PostgreSQL Global Development Group

# Appends reserve their range under the journal's lock and are written
# outside it, a transaction's last ones once its commit record is flushed.
# A reader meeting a range reserved and not yet written skips it, and
# reads it once written; a collection waits for such appends before it
# reads what they go to; an abort writes what its transaction reserved;
# and bytes no record holds at a segment's end, which a crash can leave,
# are cut off before the next append.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('appends');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql('CREATE EXTENSION test_prolly');
sql('CREATE EXTENSION injection_points');

sub hash { return sql(qq{SELECT test_content_hash('$_[0]')}); }

# A transaction stopped once its last chunks are reserved: its range is
# the journal's, its bytes not written yet.
sub stop_at_reserved
{
	my ($sql) = @_;
	my $a = $node->background_psql('postgres');
	$a->query_safe(q{SELECT injection_points_set_local();
		SELECT injection_points_attach('prolly-append-reserved', 'wait')});
	$a->query_until(qr/started/, qq{
		\\echo started
		BEGIN;
		$sql;
		COMMIT;
		\\echo done
	});
	$node->poll_query_until('postgres', q{SELECT count(*) > 0 FROM pg_stat_activity
		WHERE wait_event = 'prolly-append-reserved'})
	  or die 'the appender did not stop';
	return $a;
}

sub go_on
{
	my ($a) = @_;
	sql(q{SELECT injection_points_wakeup('prolly-append-reserved')});
	$a->query_until(qr/done/, '');
	$a->quit;
}

# A reader skips a range in flight to the chunks after it.
my $a = stop_at_reserved(q{SELECT test_chunk_put('first')});
sql(q{SELECT test_chunk_put('second')});
my $reader = $node->background_psql('postgres');
is($reader->query_safe(qq{SELECT convert_from(test_chunk_get('} . hash('second') . q{'), 'UTF8')}),
	'second', 'a chunk appended after a range in flight is read');
is($reader->query_safe(qq{SELECT test_chunk_has('} . hash('first') . q{')}),
	'f', 'the chunks in flight aren\'t');
go_on($a);
is($reader->query_safe(qq{SELECT convert_from(test_chunk_get('} . hash('first') . q{'), 'UTF8')}),
	'first', 'and are once written, by the reader that skipped them');
$reader->quit;

# A reader that meets a range in flight inside it, its first records
# whole (a write seen in part), skips the rest of it too.  The part is
# made by hand: a record of the segment's copied to the range's start, in
# place of the range's own first record (that reader is done with once
# the range is written).
my $dir = sql(q{SELECT 'base/' || oid FROM pg_database WHERE datname = 'postgres'});
my $segment = sql(qq{SELECT '$dir/' || max(f) FROM pg_ls_dir('$dir') f
	WHERE f LIKE 'prolly\\_journal%'});
my $path = $node->data_dir . "/$segment";
my $small_at = -s $path;
sql(q{SELECT test_chunk_put('s')});
my $small = (-s $path) - $small_at;
my $range_at = -s $path;
$a = stop_at_reserved(q{SELECT test_chunk_put(repeat('long', 100)::bytea)});
cmp_ok((-s $path) - $range_at, '>', $small, 'the range is longer than the record');
open(my $seg, '+<', $path) or die "open: $!";
binmode $seg;
my $record;
sysseek($seg, $small_at, 0) or die "seek: $!";
sysread($seg, $record, $small) == $small or die "read: $!";
sysseek($seg, $range_at, 0) or die "seek: $!";
syswrite($seg, $record) == $small or die "write: $!";
close $seg;
sql(q{SELECT test_chunk_put('after the part')});
$reader = $node->background_psql('postgres');
is($reader->query_safe(qq{SELECT convert_from(test_chunk_get('} . hash('after the part') . q{'), 'UTF8')}),
	'after the part', 'a chunk after a range written in part is read');
$reader->quit;
go_on($a);

# A checkpoint waits for the appends in flight as it begins, not for those
# reserved after: a second appender stopped later doesn't hold it.
$a = stop_at_reserved(q{SELECT test_chunk_put('before the checkpoint')});
my $ckpt = $node->background_psql('postgres');
$ckpt->query_until(qr/checkpointing/, q{
	\echo checkpointing
	CHECKPOINT;
	\echo checkpointed
});
$node->poll_query_until('postgres', q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE backend_type = 'checkpointer' AND wait_event = 'ProllyJournalAppend'})
  or die 'the checkpoint did not wait';
my $root_now = sql('SELECT test_chunk_root()');
my $later = $node->background_psql('postgres');
$later->query_safe(q{SELECT injection_points_set_local();
	SELECT injection_points_attach('prolly-root-padded', 'wait')});
$later->query_until(qr/started/, q{
	\echo started
	BEGIN;
	SELECT test_chunk_put('after the checkpoint began');
	SELECT test_chunk_move_root(} . ($root_now eq '' ? 'NULL' : "'$root_now'") . q{,
		test_content_hash('after the checkpoint began'));
	COMMIT;
	\echo done
});
$node->poll_query_until('postgres', q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE wait_event = 'prolly-root-padded'})
  or die 'the second appender did not stop';
go_on($a);
$ckpt->query_until(qr/checkpointed/, '');
pass('the checkpoint finished with the later append in flight');
$ckpt->quit;
sql(q{SELECT injection_points_wakeup('prolly-root-padded')});
$later->query_until(qr/done/, '');
$later->quit;

# A chunk put by a callback that runs after the chunk store's at
# pre-commit (registered before the store was opened) is written.
my $fresh_put = $node->background_psql('postgres');
$fresh_put->query_safe(q{BEGIN;
	SELECT test_chunk_put_at_commit('put at commit');
	SELECT test_chunk_put('put before');
	COMMIT;});
$fresh_put->quit;
is(sql(qq{SELECT convert_from(test_chunk_get('} . hash('put at commit') . q{'), 'UTF8')}),
	'put at commit', 'a chunk put after the store\'s pre-commit is written');

# A collection waits for an append in flight before reading its segment.
my $root = sql('SELECT test_chunk_root()');
my $h = hash('rooted');
my $expected = $root eq '' ? 'NULL' : "'$root'";
$a = stop_at_reserved(qq{SELECT test_chunk_put('rooted');
	SELECT test_chunk_move_root($expected, '$h')});
my $gc = $node->background_psql('postgres');
$gc->query_until(qr/collecting/, q{
	\echo collecting
	SELECT test_chunk_gc(false) IS NOT NULL;
	\echo collected
});
$node->poll_query_until('postgres', q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE wait_event = 'ProllyJournalAppend'})
  or die 'the collection did not wait';
pass('a collection waits for an append in flight');
go_on($a);
$gc->query_until(qr/collected/, '');
$gc->quit;
is(sql('SELECT test_chunk_root()'), $h, 'the append\'s root moved');
is(sql(qq{SELECT convert_from(test_chunk_get('$h'), 'UTF8')}), 'rooted',
	'and its chunk survived the collection');

# An abort after its reservation writes the bytes: after a restart (no
# range in flight remembered), the chunks after them are read.
sql(q{SELECT injection_points_attach('prolly-append-reserved', 'error')});
my ($ret, $out, $err) = $node->psql('postgres',
	q{BEGIN; SELECT test_chunk_put('aborted'); COMMIT;});
like($err, qr/error triggered for injection point prolly-append-reserved/,
	'a commit fails after its reservation');
sql(q{SELECT injection_points_detach('prolly-append-reserved')});
sql(q{SELECT test_chunk_put('after abort')});
$node->restart;
is(sql(qq{SELECT convert_from(test_chunk_get('} . hash('after abort') . q{'), 'UTF8')}),
	'after abort', 'a chunk after an aborted reservation is read');

# Zeros at a segment's end, as a crash leaves a reservation the WAL never
# had: cut off before the next append, which is read.
$segment = sql(qq{SELECT '$dir/' || max(f) FROM pg_ls_dir('$dir') f
	WHERE f LIKE 'prolly\\_journal%'});
$node->stop;
open(my $fh, '>>', $node->data_dir . "/$segment") or die "open: $!";
binmode $fh;
print $fh "\0" x 100;
close $fh;
$node->start;
my $log_offset = -s $node->logfile;
sql(q{SELECT test_chunk_put('after zeros')});
ok($node->log_contains(qr/cutting 100 bytes no record holds off the end of chunk journal/,
	$log_offset), 'the zeros are cut off');
my $fresh = $node->background_psql('postgres');
is($fresh->query_safe(qq{SELECT convert_from(test_chunk_get('} . hash('after zeros') . q{'), 'UTF8')}),
	'after zeros', 'the append after them is read');
$fresh->quit;

$node->stop;
done_testing();
