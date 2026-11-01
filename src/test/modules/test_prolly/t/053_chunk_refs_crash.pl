# Copyright (c) 2026, PostgreSQL Global Development Group

# A put's references, and a root, are its transaction's: a crash with the
# transaction open leaves neither, and the store's audit agrees, after the
# crash, after VACUUM, and on a standby.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('chunk_refs');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', "autovacuum = off\nchunk_free_in_background = off");
$node->start;
$node->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('chunk_refs_standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->start;

my $fmt = q{SELECT test_chunk_format_refs(true);};
my $fns = q{
	CREATE OR REPLACE FUNCTION h(d bytea) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS 'SELECT substr(sha256(d), 1, 20)';
	CREATE OR REPLACE FUNCTION node(tag text, VARIADIC kids bytea[]) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS $$SELECT '\x52'::bytea || set_byte('\x00'::bytea, 0, cardinality(kids))
			|| (SELECT string_agg(h(k), ''::bytea ORDER BY o) FROM unnest(kids) WITH ORDINALITY u(k, o))
			|| convert_to(tag, 'UTF8')$$;
};
$node->safe_psql('postgres', qq{
	CREATE EXTENSION test_prolly;
	CREATE TABLE rl () USING prolly_chunks;
	CREATE TABLE ri () USING prolly_chunk_index;
	$fmt
	$fns
	SELECT test_chunk_put('rl', 'ri', h(d), d) FROM (VALUES ('\\x61'::bytea), ('\\x62')) v(d);
	SELECT test_chunk_put('rl', 'ri', h(node('p', '\\x61')), node('p', '\\x61'));
});

# a put naming both leaves, and a root of the first node, both left open
my $open = $node->background_psql('postgres');
$open->query_safe($fmt);
$open->query_safe(q{BEGIN});
$open->query_safe(q{SELECT test_chunk_put('rl', 'ri', h(node('q', '\x61', '\x62')), node('q', '\x61', '\x62'))});
$open->query_safe(q{SELECT test_chunk_ref_root('ri', h(node('p', '\x61')))});
is($open->query_safe(q{SELECT refs FROM test_chunk_index_entry('ri', h('\x62'))}), '1',
	'the open transaction sees its reference');
$node->safe_psql('postgres', 'CHECKPOINT');
$node->stop('immediate');
$open->{run}->finish;
$node->start;

# (each check turns the test format on first: its last line is the answer)
sub last_line { my @l = split /\n/, shift; return $l[-1] // ''; }
my $counts = qq{$fmt SELECT string_agg(refs || '/' || roots, ' ' ORDER BY k)
	FROM (VALUES (1, '\\x61'::bytea), (2, '\\x62'), (3, node('p', '\\x61'))) v(k, d),
	LATERAL test_chunk_index_entry('ri', h(d))};
is(last_line($node->safe_psql('postgres', $counts)), "1/0 0/0 0/0",
	'after the crash, only the committed put\'s reference is counted');
is(last_line($node->safe_psql('postgres', qq{$fmt SELECT test_chunk_has('rl', 'ri', h(node('q', '\\x61', '\\x62')))})),
	'f', 'and the cut short put is not found');
my $audit = qq{$fmt SELECT ok || ' ' || coalesce(why, '') FROM test_chunk_check('rl', 'ri')};
is(last_line($node->safe_psql('postgres', $audit)), 'true ', 'the audit agrees');
$node->safe_psql('postgres', 'VACUUM ri');
is(last_line($node->safe_psql('postgres', $counts)), "1/0 0/0 0/0", 'VACUUM keeps the counts');
is(last_line($node->safe_psql('postgres', $audit)), 'true ', 'and the audit still agrees');
$node->wait_for_replay_catchup($standby);
is(last_line($standby->safe_psql('postgres', $counts)), "1/0 0/0 0/0", 'so does a standby');

$standby->stop;
$node->stop;
done_testing();
