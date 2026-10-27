# Copyright (c) 2026, PostgreSQL Global Development Group

# A page sweep's compaction moves a table's pages down into free blocks,
# each move's old block freed with it, under the table's map header: a
# sweep counting the table (under that header) never finds an old block
# held by nothing but not yet freed.  Sweep A stopped once its compaction
# has moved the pages (its own sweep no longer running); sweep B then
# frees and shortens the store; a table grows back into the block numbers
# cut off; A goes on.  Nothing a table holds is freed: every table reads
# whole after a restart, and a later sweep finds every map whole.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('compact_frees_moved');
$node->init;
$node->append_conf('postgresql.conf', "debug_block_mapped_heap = on\nautovacuum = off\nprolly_fold_worker = off");
$node->start;
sub sql { return $node->safe_psql('postgres', $_[0]); }
sql('CREATE EXTENSION test_prolly; CREATE EXTENSION injection_points');
sub interleave
{
	my ($keep, $gone, $rounds) = @_;
	sql(qq{CREATE TABLE $keep (a int PRIMARY KEY, b text) WITH (fillfactor = 10);
		CREATE TABLE $gone (a int, b text) WITH (fillfactor = 10)});
	sql(qq{DO \$\$
		BEGIN
			FOR i IN 1 .. $rounds LOOP
				INSERT INTO $keep SELECT g, repeat('k', 300) FROM generate_series(i * 10, i * 10 + 9) g;
				INSERT INTO $gone SELECT g, repeat('g', 300) FROM generate_series(i * 10, i * 10 + 9) g;
			END LOOP;
		END \$\$});
}
interleave('keep', 'gone', 200);
my $digest = q{SELECT count(*) || ':' || md5(string_agg(a || b, ',' ORDER BY a)) FROM keep};
my $want = sql($digest);
sql('DROP TABLE gone');

# sweep A: held after its compaction moved keep's pages, before it frees their old blocks
my $a = $node->background_psql('postgres', on_error_stop => 0);
$a->query_safe(q{SELECT injection_points_set_local();
	SELECT injection_points_attach('bmap-compact-moved', 'wait')});
$a->query_until(qr/started/, q{
	\echo started
	SELECT 'A ' || freed || ' ' || truncated || ' ' || moved FROM prolly_page_sweep();
	\echo done
});
$node->poll_query_until('postgres', q{SELECT count(*) > 0 FROM pg_stat_activity
	WHERE wait_event = 'bmap-compact-moved'}) or die 'A did not stop';

# sweep B meanwhile, A's sweep no longer running: it frees and shortens
sql(q{SELECT count(*) FROM prolly_page_sweep()});

# a table grows back into the cut-off block numbers
sql(q{CREATE TABLE grow (a int PRIMARY KEY, b text) WITH (fillfactor = 10);
	INSERT INTO grow SELECT g, repeat('x', 300) FROM generate_series(1, 4000) g});
my $gwant = sql(q{SELECT count(*) || ':' || md5(string_agg(a || b, ',' ORDER BY a)) FROM grow});

sql(q{SELECT injection_points_detach('bmap-compact-moved')}); sql(q{SELECT injection_points_wakeup('bmap-compact-moved')});
my $aout = $a->query_until(qr/done/, '');
like($aout, qr/A \d+ \d+ [1-9]/, 'A moved pages');
unlike($a->{stderr}, qr/ERROR/, 'A ends with no error');
$a->quit;

# blocks freed now are given to another table: none of grow's
sql(q{CREATE TABLE fill (a int, b text) WITH (fillfactor = 10);
	INSERT INTO fill SELECT g, repeat('f', 300) FROM generate_series(1, 4000) g});
my ($ret, $out, $err) = $node->psql('postgres', q{SELECT count(*) FROM prolly_page_sweep()});
is($ret, 0, 'a later sweep finds every map whole') or diag($err);
$node->restart;
is(sql(q{SELECT count(*) || ':' || md5(string_agg(a || b, ',' ORDER BY a)) FROM grow}), $gwant, 'the table that grew reads whole');
is(sql($digest), $want, 'and the moved one');
$node->stop;
done_testing();
