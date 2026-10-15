# Copyright (c) 2026, PostgreSQL Global Development Group

# Engine D1 stores rows larger than a node: their values go out of line,
# into value trees split by a rolling hash.  They read back whole, survive
# a crash and replay on a standby, and changing a few bytes of one writes
# a few nodes, not the whole value.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('large_values');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', "default_table_access_method = prolly_d1\n");
$node->start;
$node->backup('b');
my $standby = PostgreSQL::Test::Cluster->new('large_values_standby');
$standby->init_from_backup($node, 'b', has_streaming => 1);
$standby->start;

$node->safe_psql('postgres', q{
	CREATE TABLE big (id int PRIMARY KEY, t text, b bytea);
	INSERT INTO big SELECT g, repeat(md5(g::text), 2000 * g),
		decode(repeat(to_hex(g * 17), 30000), 'hex')
	FROM generate_series(1, 5) g;
	CREATE TABLE keyless (t text);
	INSERT INTO keyless SELECT repeat('k', 100000) FROM generate_series(1, 3);
});
my $digest = q{SELECT string_agg(id || ':' || length(t) || ':' || md5(t) || ':' || md5(b), ',' ORDER BY id) FROM big};
my $before = $node->safe_psql('postgres', $digest);
like($before, qr/^1:64000:/, 'rows larger than a node are stored and read back');
is($node->safe_psql('postgres', 'SELECT count(*), sum(length(t)) FROM keyless'),
	'3|300000', 'identical large rows of a keyless table are counted');

# A few bytes changed in a 192 KB value write a few nodes.
my $w = $node->safe_psql('postgres', q{
	SELECT nodes_written FROM prolly_node_counters('prolly_d1');
	UPDATE big SET t = overlay(t placing 'XYZ' from 96000) WHERE id = 3;
	SELECT nodes_written FROM prolly_node_counters('prolly_d1');});
my ($w0, $w1) = split /\n/, $w;
cmp_ok($w1 - $w0, '<', 40,
	'changing a few bytes of a large value writes a few nodes');
my $after = $node->safe_psql('postgres', $digest);
isnt($after, $before, 'the change is there');

# Through a crash, and on a standby.
$node->stop('immediate');
$node->start;
is($node->safe_psql('postgres', $digest), $after,
	'large values survive a crash');
$node->wait_for_replay_catchup($standby);
is($standby->safe_psql('postgres', $digest), $after,
	'and replay on a standby');

$standby->stop;
$node->stop;
done_testing();
