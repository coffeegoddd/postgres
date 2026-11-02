# Copyright (c) 2026, PostgreSQL Global Development Group

# The history store's tier across crashes and on a hot standby: a pack
# written as its transaction commits survives a crash; one written, the
# crash before the commit, is an orphan, its chunks not there, its object
# removed by VACUUM; a crash after the catalog's file is extended for a
# pack's entry, before the entry's record, leaves a page the next pack
# makes; a transaction's chunks past a pack's size go in packs one after
# another; a hot standby, at the same tier, reads them; a pack all dead
# keeps its object until the standby has replayed past where it died (or
# while a standby is being made); the store is opened again once its setting
# changes; and standbys promoted apart name their packs apart, and
# remove the packs from before only once adopted.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

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
is($node->safe_psql('postgres', q{
	SELECT count(DISTINCT t.hash) FROM test_tier_packs('pg_history.tier_chunks') p,
		LATERAL test_tier_pack_table('pg_history.tier_chunks', p.id) t
	WHERE p.state <> 'removed' AND p.size > 0
		AND t.hash IN (SELECT h(blob('big' || g, 250000)) FROM generate_series(1, 300) g)}),
	'300', 'each in its pack\'s table, the first pack written full as the transaction went on');

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

# a standby being made (a base backup running, its walsender not yet
# told how far anything replayed): not past
my $dead_pack = sub {
	my ($tag) = @_;
	$node->safe_psql('postgres', qq{
		SELECT count(*) FROM generate_series(1, 5) g,
			LATERAL test_cs_put('tier', blob('$tag' || g, 3000));
		SELECT count(*) FROM generate_series(1, 5) g,
			LATERAL test_cs_ref_root('tier', h(blob('$tag' || g, 3000)));
	});
	$node->safe_psql('postgres', qq{
		SELECT count(*) FROM generate_series(1, 5) g,
			LATERAL test_cs_unref_root('tier', h(blob('$tag' || g, 3000)));
		SELECT test_cs_free('tier', ARRAY(SELECT h(blob('$tag' || g, 3000)) FROM generate_series(1, 5) g));
	});
};
$node->safe_psql('postgres', 'CHECKPOINT');
$node->wait_for_replay_catchup($standby);
my $backup_dir = PostgreSQL::Test::Utils::tempdir . '/made';
my $bb = IPC::Run::start(
	[ 'pg_basebackup', '--no-sync', '--max-rate=32', '--wal-method=none', '--checkpoint=fast',
	  '--pgdata' => $backup_dir, '--dbname' => $node->connstr('postgres') ],
	'>' => \my $bb_out, '2>' => \my $bb_err);
$node->poll_query_until('postgres',
	q{SELECT count(*) > 0 FROM pg_stat_replication WHERE state = 'backup'})
  or die 'no base backup running';
$dead_pack->('quiet');
my $p2 = $packs->();
# (the first VACUUM counts them dead; the standby streaming then past it)
$node->safe_psql('postgres', 'VACUUM pg_history.tier_index');
$node->safe_psql('postgres', 'CHECKPOINT');
$node->wait_for_replay_catchup($standby);
my $log_at = -s $node->logfile;
$node->safe_psql('postgres', 'VACUUM pg_history.tier_index');
is($packs->(), $p2, 'all dead, kept while a standby is being made');
ok($node->log_contains(qr/packs of tier "tier_chunks" all dead are kept for standbys\n.*DETAIL:  Walsender \d+ is taking a base backup/, $log_at),
	'and said so');
$bb->kill_kill;
$node->poll_query_until('postgres',
	q{SELECT count(*) = 0 FROM pg_stat_replication WHERE state = 'backup'})
  or die 'the base backup runs still';
$node->safe_psql('postgres', 'VACUUM pg_history.tier_index');
is($packs->(), $p2 - 1, 'and removed once it is not');

# pg_receivewal streaming, no standby's (it never says how far it
# replayed): not waited for
my $wal_dir = PostgreSQL::Test::Utils::tempdir;
my $rw = IPC::Run::start(
	[ 'pg_receivewal', '--no-loop', '--directory' => $wal_dir,
	  '--dbname' => $node->connstr('postgres') . ' application_name=pg_receivewal' ],
	'>' => \my $rw_out, '2>' => \my $rw_err);
$node->poll_query_until('postgres',
	q{SELECT count(*) > 0 FROM pg_stat_replication WHERE application_name = 'pg_receivewal' AND state = 'streaming'})
  or die "no pg_receivewal streaming: $rw_err";
$dead_pack->('received');
my $p4 = $packs->();
$node->safe_psql('postgres', 'VACUUM pg_history.tier_index');
$node->safe_psql('postgres', 'CHECKPOINT');
$node->wait_for_replay_catchup($standby);
$node->safe_psql('postgres', 'VACUUM pg_history.tier_index');
is($packs->(), $p4 - 1, 'all dead, removed with pg_receivewal streaming');

# a pack kept for a standby replaying, and one after it removed (with
# pg_receivewal, looked at after the standby, streaming still): the
# standby's walsender named
# (the standby's walsender in the first slot, taken when it reconnected
# with no other sender; pg_receivewal's after it)
my $make = sub {
	my ($tag) = @_;
	$node->safe_psql('postgres', qq{
		SELECT count(*) FROM generate_series(1, 5) g,
			LATERAL test_cs_put('tier', blob('$tag' || g, 3000));
		SELECT count(*) FROM generate_series(1, 5) g,
			LATERAL test_cs_ref_root('tier', h(blob('$tag' || g, 3000)));
	});
};
my $kill = sub {
	my ($tag) = @_;
	$node->safe_psql('postgres', qq{
		SELECT count(*) FROM generate_series(1, 5) g,
			LATERAL test_cs_unref_root('tier', h(blob('$tag' || g, 3000)));
		SELECT test_cs_free('tier', ARRAY(SELECT h(blob('$tag' || g, 3000)) FROM generate_series(1, 5) g));
	});
};
$make->('lagging');
$make->('passing');
$kill->('passing');
my $p6 = $packs->();
$node->safe_psql('postgres', 'VACUUM pg_history.tier_index');
$node->safe_psql('postgres', 'CHECKPOINT');
$node->wait_for_replay_catchup($standby);
$standby->safe_psql('postgres', 'SELECT pg_wal_replay_pause()');
$kill->('lagging');
my $standby_sender = $node->safe_psql('postgres',
	q{SELECT pid FROM pg_stat_replication WHERE application_name = 'tier_standby'});
$log_at = -s $node->logfile;
$node->safe_psql('postgres', 'VACUUM pg_history.tier_index');
is($packs->(), $p6 - 1, 'the pack the standby replayed past removed, the other kept');
ok($node->log_contains(qr/DETAIL:  Walsender $standby_sender has not replayed past where they died/, $log_at),
	'the standby\'s walsender named');
$standby->safe_psql('postgres', 'SELECT pg_wal_replay_resume()');
$node->safe_psql('postgres', 'CHECKPOINT');
$node->wait_for_replay_catchup($standby);
$node->safe_psql('postgres', 'VACUUM pg_history.tier_index');
is($packs->(), $p6 - 2, 'and that one once it has');
$rw->kill_kill;
$node->poll_query_until('postgres',
	q{SELECT count(*) = 0 FROM pg_stat_replication WHERE application_name = 'pg_receivewal'})
  or die 'pg_receivewal runs still';

# a sender that has not yet replied (as a standby just connected): waited
# for
# (the walsender not asking it for a reply)
$node->safe_psql('postgres', 'ALTER SYSTEM SET wal_sender_timeout = 0');
$node->reload;
$node->poll_query_until('postgres', q{SELECT current_setting('wal_sender_timeout') = '0'})
  or die 'wal_sender_timeout not reloaded';
my $wal_dir2 = PostgreSQL::Test::Utils::tempdir;
my $rw2 = IPC::Run::start(
	[ 'pg_receivewal', '--no-loop', '--status-interval=0', '--directory' => $wal_dir2,
	  '--dbname' => $node->connstr('postgres') . ' application_name=quiet_receiver' ],
	'>' => \my $rw2_out, '2>' => \my $rw2_err);
$node->poll_query_until('postgres',
	q{SELECT count(*) > 0 FROM pg_stat_replication WHERE application_name = 'quiet_receiver' AND state = 'streaming' AND reply_time IS NULL})
  or die "no quiet pg_receivewal streaming: $rw2_err";
$dead_pack->('unreplied');
my $p5 = $packs->();
$node->safe_psql('postgres', 'VACUUM pg_history.tier_index');
$node->safe_psql('postgres', 'CHECKPOINT');
$node->wait_for_replay_catchup($standby);
$log_at = -s $node->logfile;
$node->safe_psql('postgres', 'VACUUM pg_history.tier_index');
is($packs->(), $p5, 'all dead, kept while a sender has not yet replied');
ok($node->log_contains(qr/DETAIL:  Walsender \d+ has not yet replied/, $log_at), 'and said so');
$rw2->kill_kill;
$node->poll_query_until('postgres',
	q{SELECT count(*) = 0 FROM pg_stat_replication WHERE application_name = 'quiet_receiver'})
  or die 'the quiet pg_receivewal runs still';
$node->safe_psql('postgres', 'VACUUM pg_history.tier_index');
is($packs->(), $p5 - 1, 'and removed once it is gone');
$node->safe_psql('postgres', 'ALTER SYSTEM RESET wal_sender_timeout');
$node->reload;

# the store opened again once the settings change: a pack after a reload
# where they say
my $tier2 = PostgreSQL::Test::Utils::tempdir;
my $r = $node->background_psql('postgres');
$r->query_safe(q{SELECT (test_cs_put('tier', blob('there', 100))).stored});
$r->query_safe(qq{ALTER SYSTEM SET history_tier_location = '$tier2'});
$r->query_safe('SELECT pg_reload_conf()');
# (the session's settings changed between its statements)
my $seen = 0;
for (1 .. 10 * $PostgreSQL::Test::Utils::timeout_default)
{
	last if ($seen = $r->query_safe(q{SELECT current_setting('history_tier_location')}) eq $tier2);
	usleep(100_000);
}
ok($seen, 'the session has the new setting');
$r->query_safe(q{SELECT (test_cs_put('tier', blob('moved', 100))).stored});
$r->query_safe('ALTER SYSTEM RESET history_tier_location');
$r->query_safe('SELECT pg_reload_conf()');
$r->quit;
opendir(my $dh2, $tier2) or die;
is(scalar(grep(/^packs#/, readdir($dh2))), 1, 'a pack where the setting says, after a reload');
closedir($dh2);

# two standbys promoted apart, the primary stopped: each on timeline 2,
# each its own owner, so their packs' names are not each other's
$node->backup('b2');
my $standby2 = PostgreSQL::Test::Cluster->new('tier_standby2');
$standby2->init_from_backup($node, 'b2', has_streaming => 1);
$standby2->append_conf('postgresql.conf', qq{history_tier_location = '$tier'});
$standby2->start;
$node->wait_for_replay_catchup($standby2);
$node->wait_for_replay_catchup($standby);
$node->stop;
$standby->promote;
$standby2->promote;
my $tli = q{SELECT substr(pg_walfile_name(pg_current_wal_lsn()), 1, 8)};
is($standby->safe_psql('postgres', $tli), $standby2->safe_psql('postgres', $tli),
	'both promoted to the same timeline');
my $p3 = $packs->();
my $owner = q{SELECT owner FROM test_tier_packs('pg_history.tier_chunks') ORDER BY id DESC LIMIT 1};
my $old_owner = $standby->safe_psql('postgres', $owner);
for my $n ($standby, $standby2)
{
	$n->safe_psql('postgres', q{
		SELECT (test_cs_put('tier', blob('promoted', 100))).stored;
		SELECT test_cs_ref_root('tier', h(blob('promoted', 100)));
	});
}
my ($o1, $o2) = map { $_->safe_psql('postgres', $owner) } ($standby, $standby2);
ok($o1 ne $o2 && $o1 ne $old_owner && $o2 ne $old_owner, 'three owners');
is($packs->(), $p3 + 2, 'two packs, of two names');
is($standby->safe_psql('postgres', q{SELECT convert_from(test_cs_get('tier', h(blob('promoted', 100))), 'UTF8') = rpad('promoted', 100, '.')}),
	't', 'each reads its own');
is($standby2->safe_psql('postgres', q{SELECT convert_from(test_cs_get('tier', h(blob('promoted', 100))), 'UTF8') = rpad('promoted', 100, '.')}),
	't', 'and so does the other');
$standby2->stop;

# the packs from before the promotion, dead: not removed, until adopted
$standby->safe_psql('postgres', q{
	SELECT count(*) FROM generate_series(1, 300) g,
		LATERAL test_cs_unref_root('tier', h(blob('big' || g, 250000)));
	SELECT test_cs_free('tier', ARRAY(SELECT h(blob('big' || g, 250000)) FROM generate_series(1, 300) g));
});
$p3 = $packs->();
$standby->safe_psql('postgres', 'VACUUM pg_history.tier_index');
is($packs->(), $p3, 'packs dead from before the promotion: kept');
ok($standby->safe_psql('postgres', 'SELECT pg_history_tier_adopt()') > 0, 'adopted');
$standby->safe_psql('postgres', 'VACUUM pg_history.tier_index');
is($packs->(), $p3 - 2, 'and then removed');
is($standby->safe_psql('postgres', q{SELECT convert_from(test_cs_get('tier', h(blob('promoted', 100))), 'UTF8') = rpad('promoted', 100, '.')}),
	't', 'its own read still');

$standby->stop;
done_testing();
