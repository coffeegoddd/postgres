# Copyright (c) 2026, PostgreSQL Global Development Group

# A relation made of a captured root takes the root's subtrees as holders
# under the root's slot lock, and only of a root no releaser that may yet
# commit has named.  A root let go of (committed, its subtrees drained)
# after the share first looked at it is refused, rather than shared: the
# copy would hold blocks freed meanwhile, or pages the relation it was
# captured of then changes in place.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('root_share_released');
$node->init;
$node->append_conf('postgresql.conf', "autovacuum = off\nprolly_fold_worker = off");
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{CREATE EXTENSION test_prolly; CREATE EXTENSION injection_points;
	SET debug_block_mapped_heap = on;
	CREATE TABLE big (a int, b text) WITH (fillfactor = 10);
	INSERT INTO big SELECT g, repeat('b', 200) FROM generate_series(1, 3000) g});
my $big = sql(q{SELECT pg_relation_filenode('big')});
my $root = sql(qq{SELECT test_bmap_root_capture($big)});
my $digest = sql(qq{SELECT test_bmap_rows_digest($big)});

# a share stopped after it first found the root held
sql(q{SELECT injection_points_attach('bmap-root-share-checked', 'wait')});
my $s = $node->background_psql('postgres', on_error_stop => 0);
$s->query_until(qr/sharing/, qq{\\echo sharing
SELECT test_bmap_share_root($root);
});
$node->wait_for_event('client backend', 'bmap-root-share-checked');

# meanwhile the root is let go of, committed, its subtrees drained, and the
# relation it was captured of changes the pages
sql(qq{SELECT test_bmap_root_release($root)});
sql(q{SELECT count(*) FROM test_bmap_release_drain()});
sql(q{SET debug_block_mapped_heap = on;
	UPDATE big SET b = 'changed' WHERE a <= 50});
sql(q{SELECT injection_points_wakeup('bmap-root-share-checked');
	SELECT injection_points_detach('bmap-root-share-checked')});
my $out = $s->query(q{SELECT 'shared'});
like($s->{stderr}, qr/holds no captured root/, 'the share refuses the root let go of');
$s->quit;

is(sql(qq{SELECT test_bmap_root_held($root) IS NULL}), 't', 'the root is let go of');
isnt(sql(qq{SELECT test_bmap_rows_digest($big)}), $digest, 'the relation changed');

$node->stop;
done_testing();
