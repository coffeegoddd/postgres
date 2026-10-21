# Copyright (c) 2026, PostgreSQL Global Development Group

# A collection's scan of the page store for hash pointers reads only the
# blocks written since the last collection: what the last found in the
# rest it keeps in a file, which the page store's token says still holds.
# A second collection with nothing written reads far fewer blocks than the
# first, and a value written between collections is kept.  After a
# collection that failed, or a restart, every block is read again; a file
# damaged while the server was down is discarded.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('changed_pages');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

# a value of its own for each label, stored by content
sub value
{
	return "(SELECT string_agg(md5('$_[0]' || i), '' ORDER BY i)
		FROM generate_series(1, 400) i)";
}
sub root
{
	return sql("SELECT (test_toast_pointer(v)).root FROM c.t WHERE id = $_[0]");
}
sub has { return sql("SELECT test_chunk_has('$_[0]')"); }

# The blocks a collection read (the counts reach the view once the session
# that collected ends).
sub gc_pages
{
	sql(q{SELECT pg_stat_reset_shared('prolly')});
	sql('SELECT test_chunk_gc(false) IS NOT NULL');
	return sql(q{SELECT value FROM pg_stat_prolly WHERE name = 'gc_pages_scanned'});
}

sql('CREATE EXTENSION test_prolly');
sql('CREATE SCHEMA c VERSIONED');
sql('CREATE TABLE c.t (id int PRIMARY KEY, pad text, v text)');
sql(q{INSERT INTO c.t SELECT g, repeat('x', 500), NULL
	FROM generate_series(1, 5000) g});
sql('INSERT INTO c.t VALUES (0, NULL, ' . value('first') . ')');
my $first = root(0);
ok($first ne '', 'the value is stored by content');
sql('VACUUM c.t');
sql('CHECKPOINT');

my $all = gc_pages();
cmp_ok($all, '>', 300, "the first collection reads every block ($all)");
is(has($first), 't', 'and keeps the value a row names');

my $again = gc_pages();
cmp_ok($again, '<', $all / 10,
	"a second, nothing written since, reads far fewer ($again)");
is(has($first), 't', 'and keeps the value still');

# A value written between collections is kept.
sql('INSERT INTO c.t VALUES (-1, NULL, ' . value('between') . ')');
my $between = root(-1);
my $after = gc_pages();
cmp_ok($after, '<', $all / 10, "a collection after a few writes reads few ($after)");
is(has($between), 't', 'a value written between collections is kept');
is(sql('SELECT v = ' . value('between') . ' FROM c.t WHERE id = -1'), 't',
	'and reads back');

# Rewritten pages anywhere in the table: their values are kept.
sql('UPDATE c.t SET v = ' . value('late') . ' WHERE id = 2500');
my $late = root(2500);
gc_pages();
is(has($late), 't', 'a value written in the middle of the table is kept');
is(has($between), 't', 'and the others still');

# A collection that fails after taking the blocks written since the last
# leaves nothing the next may trust: it reads every block, and keeps a
# value written before the failed one began.
SKIP:
{
	skip 'Injection points not supported by this build', 3
	  unless $ENV{enable_injection_points} eq 'yes';
	sql('CREATE EXTENSION injection_points');
	sql('UPDATE c.t SET v = ' . value('failed') . ' WHERE id = 1000');
	my $failed = root(1000);
	my $sc = $node->background_psql('postgres', on_error_stop => 0);
	$sc->query_safe(q{SELECT injection_points_set_local();
		SELECT injection_points_attach('bmap-scan-store', 'wait')});
	$sc->query_until(qr/collecting/, q{
		\echo collecting
		SELECT test_chunk_gc(false) IS NOT NULL;
		\echo collected
	});
	$node->poll_query_until('postgres', q{SELECT count(*) > 0 FROM pg_stat_activity
		WHERE wait_event = 'bmap-scan-store'})
	  or die 'the collection did not stop';
	sql(q{SELECT pg_cancel_backend(pid) FROM pg_stat_activity
		WHERE wait_event = 'bmap-scan-store'});
	$sc->query_until(qr/collected/, '');
	$sc->quit;
	my $next = gc_pages();
	cmp_ok($next, '>', $all / 2,
		"after a failed collection, the next reads every block ($next)");
	is(has($failed), 't', 'and keeps a value written before the failed one');
	cmp_ok(gc_pages(), '<', $all / 10, 'and the one after reads few again');
}

# A restart forgets what was watched: every block is read again.
$node->restart;
my $restarted = gc_pages();
cmp_ok($restarted, '>', $all / 2,
	"after a restart, a collection reads every block ($restarted)");
is(has($late), 't', 'and keeps what rows name');
is(has($between), 't', 'every one of them');
cmp_ok(gc_pages(), '<', $all / 10, 'and the next reads few again');

# The file isn't synced: one damaged while the server was down (its token
# stale after the restart) is discarded, every block read again, not an
# error that stops every collection.
my $cache = $node->data_dir . '/base/'
  . sql(q{SELECT oid FROM pg_database WHERE datname = 'postgres'})
  . '/prolly_page_roots_'
  . sql(q{SELECT oid FROM pg_tablespace WHERE spcname = 'pg_default'});
ok(-s $cache > 20, 'a collection leaves its file');
$node->stop;
truncate($cache, 20) or die "could not truncate $cache: $!";
$node->start;
my $truncated = gc_pages();
cmp_ok($truncated, '>', $all / 2,
	"after its file is cut short in a range, a collection reads every block ($truncated)");
is(has($late), 't', 'and keeps what rows name');
cmp_ok(gc_pages(), '<', $all / 10, 'and the next reads few again');

# A file whose header is garbage, the same.
$node->stop;
open(my $fh, '+<', $cache) or die "could not open $cache: $!";
binmode $fh;
print $fh "garbage, not a header";
close($fh);
$node->start;
my $garbage = gc_pages();
cmp_ok($garbage, '>', $all / 2,
	"after its header is overwritten, a collection reads every block ($garbage)");
is(has($late), 't', 'and keeps what rows name');
is(sql('SELECT v = ' . value('late') . ' FROM c.t WHERE id = 2500'), 't',
	'which reads back');

$node->stop;
done_testing();
