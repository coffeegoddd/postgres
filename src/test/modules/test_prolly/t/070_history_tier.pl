# Copyright (c) 2026, PostgreSQL Global Development Group

# The history store's tier across crashes and on a hot standby: a pack
# written as its transaction commits survives a crash; one written, the
# crash before the commit, is an orphan, its chunks not there, its object
# removed by VACUUM; a crash after the catalog's file is extended for a
# pack's entry, before the entry's record, leaves a page the next pack
# makes; a transaction's chunks past a pack's size go in packs one after
# another; a hot standby, at the same tier, reads them; and a pack all
# dead keeps its object until the standby has replayed past where it
# died.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'injection points not supported by this build';
}

my $tier = PostgreSQL::Test::Utils::tempdir;
my $node = PostgreSQL::Test::Cluster->new('tier');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', qq{
autovacuum = off
chunk_free_in_background = off
history_tier_location = '$tier'
# (no flush of the WAL but what the tier and commits ask)
wal_writer_delay = 10000
});
$node->start;
$node->safe_psql('postgres', q{
	CREATE EXTENSION test_prolly;
	CREATE EXTENSION injection_points;
	CREATE FUNCTION h(d bytea) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS 'SELECT substr(sha256(d), 1, 20)';
	CREATE FUNCTION blob(tag text, len int) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS $$SELECT convert_to(rpad(tag, len, '.'), 'UTF8')$$;
});
$node->backup('b');
my $standby = PostgreSQL::Test::Cluster->new('tier_standby');
$standby->init_from_backup($node, 'b', has_streaming => 1);
$standby->append_conf('postgresql.conf', qq{history_tier_location = '$tier'});
$standby->start;
my $packs = sub {
	opendir(my $dh, $tier) or return 0;
	my $n = grep(/^packs#/, readdir($dh));
	closedir($dh);
	return $n;
};

# a pack committed, then a crash: there
$node->safe_psql('postgres', q{
	SELECT count(*) FROM generate_series(1, 20) g,
		LATERAL test_cs_put('tier', blob('kept' || g, 3000));
	SELECT count(*) FROM generate_series(1, 20) g,
		LATERAL test_cs_ref_root('tier', h(blob('kept' || g, 3000)));
});
is($packs->(), 1, 'a pack a transaction that put chunks (one that only held roots, none)');
$node->stop('immediate');
$node->start;
my $kept = q{SELECT count(*) FILTER (WHERE test_cs_get('tier', h(blob('kept' || g, 3000))) = blob('kept' || g, 3000))
	FROM generate_series(1, 20) g};
is($node->safe_psql('postgres', $kept), '20', 'committed, read back after a crash');

# a pack written, the crash before its transaction commits: an orphan
my $s = $node->background_psql('postgres', on_error_stop => 0);
$s->query_safe(q{SELECT injection_points_set_local()});
$s->query_safe(q{SELECT injection_points_attach('chunk-tier-pack-written', 'wait')});
$s->query_until(qr/putting/, "\\echo putting\nSELECT count(*) FROM generate_series(1, 10) g, LATERAL test_cs_put('tier', blob('cut' || g, 3000));\n");
$node->wait_for_event('client backend', 'chunk-tier-pack-written');
my $before = $packs->();
$node->stop('immediate');
$s->{run}->finish;
$node->start;
is($node->safe_psql('postgres', q{SELECT count(*) FILTER (WHERE test_cs_has('tier', h(blob('cut' || g, 3000)))) FROM generate_series(1, 10) g}),
	'0', 'the crash before the commit: none of its chunks there');
is($packs->(), $before, 'its pack\'s object written');
$node->safe_psql('postgres', 'VACUUM pg_history.tier_index');
is($packs->(), $before - 1, 'and removed by VACUUM, an orphan');
is($node->safe_psql('postgres', q{SELECT ok FROM test_cs_check('tier')}), 't', 'the audit clean');

# the catalog's file extended for a new page of entries, the crash before
# the entry's record: a page of zeros, which the next pack makes a page
my $kept0 = $node->safe_psql('postgres', q{SELECT count(*) FROM test_tier_packs('pg_history.tier_chunks')});
my $per_page = int((8192 - 24) / 40);
my $to_page = $per_page - ($kept0 % $per_page);
$node->safe_psql('postgres', qq{
	DO \$\$BEGIN FOR i IN 1 .. $to_page LOOP
		PERFORM test_cs_put('tier', convert_to('fill' || i, 'UTF8'));
		COMMIT;
	END LOOP; END\$\$;
});
my $e = $node->background_psql('postgres', on_error_stop => 0);
$e->query_safe(q{SELECT injection_points_set_local()});
$e->query_safe(q{SELECT injection_points_attach('chunk-tier-page-extended', 'wait')});
$e->query_until(qr/extending/, "\\echo extending\nSELECT test_cs_put('tier', convert_to('lost', 'UTF8'));\n");
$node->wait_for_event('client backend', 'chunk-tier-page-extended');
$node->stop('immediate');
$e->{run}->finish;
$node->start;
is($node->safe_psql('postgres', q{SELECT (test_cs_put('tier', convert_to('after-zero', 'UTF8'))).stored}),
	't', 'a pack begun on the page of zeros the crash left');
is($node->safe_psql('postgres', q{SELECT convert_from(test_cs_get('tier', substr(sha256(convert_to('after-zero', 'UTF8')), 1, 20)), 'UTF8')}),
	'after-zero', 'and read back');

# more than a pack holds, in one transaction: packs one after another
my $p0 = $packs->();
$node->safe_psql('postgres', q{
	SELECT count(*) FROM generate_series(1, 300) g,
		LATERAL test_cs_put('tier', blob('big' || g, 250000));
	SELECT count(*) FROM generate_series(1, 300) g,
		LATERAL test_cs_ref_root('tier', h(blob('big' || g, 250000)));
});
is($packs->() - $p0, 2, 'some 75 MB in two packs');
is($node->safe_psql('postgres', q{SELECT count(*) FILTER (WHERE test_cs_get('tier', h(blob('big' || g, 250000))) = blob('big' || g, 250000)) FROM generate_series(1, 300) g}),
	'300', 'every one read back');

# a hot standby at the same tier reads them
$node->wait_for_replay_catchup($standby);
is($standby->safe_psql('postgres', $kept), '20', 'the standby reads the tier\'s chunks');

# a pack all dead, the standby's replay paused before: its object kept
# (the standby's readers may read it) until the standby replays past
$standby->safe_psql('postgres', 'SELECT pg_wal_replay_pause()');
$node->safe_psql('postgres', q{
	SELECT count(*) FROM generate_series(1, 20) g,
		LATERAL test_cs_unref_root('tier', h(blob('kept' || g, 3000)));
	SELECT test_cs_free('tier', ARRAY(SELECT h(blob('kept' || g, 3000)) FROM generate_series(1, 20) g));
});
my $p1 = $packs->();
$node->safe_psql('postgres', 'VACUUM pg_history.tier_index');
is($packs->(), $p1, 'all dead, kept while the standby has not replayed past');
is($standby->safe_psql('postgres', $kept), '20', 'the standby reads them still');
$standby->safe_psql('postgres', 'SELECT pg_wal_replay_resume()');
# (VACUUM's records flushed, for the standby to have them)
$node->safe_psql('postgres', 'CHECKPOINT');
$node->wait_for_replay_catchup($standby);
$node->safe_psql('postgres', 'VACUUM pg_history.tier_index');
is($packs->(), $p1 - 1, 'and removed once it has');

$standby->stop;
$node->stop;
done_testing();
