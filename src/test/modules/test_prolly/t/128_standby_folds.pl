# Copyright (c) 2026, PostgreSQL Global Development Group

# A hot standby folds from the table's baseline, as the primary does: its
# change log and baseline are replayed, and its snapshots are real, so
# dolt_status and dolt_diff there read what changed, not every row; and
# give the primary's answers.  Promoted, it folds and commits.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('standby_folds');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;
sub sql { return $node->safe_psql('postgres', $_[0]); }

sql(q{CREATE EXTENSION test_prolly;
	CREATE SCHEMA sf VERSIONED;
	CREATE TABLE sf.t (id int PRIMARY KEY, v text);
	INSERT INTO sf.t SELECT g, 'v' || g FROM generate_series(1, 20000) g;
	SELECT dolt_commit('-Am', 'loaded')});

$node->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('standby_folds_standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->start;
sub on_standby { return $standby->safe_psql('postgres', $_[0]); }

# a fold's full reads on the standby, of the query given
sub full_reads
{
	my ($q) = @_;
	on_standby(q{SELECT pg_stat_force_next_flush(); SELECT pg_stat_reset_shared('prolly')});
	on_standby(qq{$q; SELECT pg_stat_force_next_flush()});
	return on_standby(q{SELECT value FROM pg_stat_prolly WHERE name = 'fold_full_tables'});
}

# rows changed on the primary, not committed to version control
sql(q{UPDATE sf.t SET v = 'changed' WHERE id % 1000 = 0;
	DELETE FROM sf.t WHERE id = 5;
	INSERT INTO sf.t VALUES (20001, 'new')});
$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));

my $status = q{SELECT table_name || ' ' || status FROM dolt_status ORDER BY 1};
my $diff = q{SELECT string_agg(diff_type || ':' || coalesce((to_row).id, (from_row).id), ',' ORDER BY coalesce((to_row).id, (from_row).id))
	FROM dolt_diff('HEAD', 'WORKING', NULL::sf.t)};
is(on_standby($status), sql($status), 'the standby\'s status is the primary\'s');
is(on_standby($diff), sql($diff), 'and its diff');
is(full_reads($diff), '0', 'folded from the baseline, no table read in full');

# committed on the primary: clean on the standby, still from the baseline
sql(q{SELECT dolt_commit('-am', 'changes')});
$node->wait_for_catchup($standby, 'replay', $node->lsn('insert'));
is(full_reads(q{SELECT count(*) FROM dolt_status}), '0',
	'after the primary\'s commit, from its new baseline');
is(on_standby(q{SELECT count(*) FROM dolt_status}), '0', 'and clean');

# what a standby's fold builds is the transaction's, never written: a
# commit there is still refused
my ($ret, $out, $err) = $standby->psql('postgres',
	q{SELECT dolt_commit('--allow-empty', '-m', 'on the standby')});
isnt($ret, 0, 'a commit on the standby is refused');
like($err, qr/read-only|recovery/, 'as read-only');
is(on_standby(q{SELECT count(*) FROM dolt_status}), '0',
	'and folds after it still read');

# promoted: it folds and commits
$standby->promote;
$standby->poll_query_until('postgres', 'SELECT NOT pg_is_in_recovery()')
  or die 'the standby was not promoted';
on_standby(q{UPDATE sf.t SET v = 'after' WHERE id = 1; SELECT dolt_commit('-am', 'promoted')});
is(on_standby(q{SELECT v FROM sf.t WHERE id = 1}), 'after', 'promoted, it commits');
is(on_standby(q{SELECT count(*) FROM dolt_log}), '3', 'onto the history it replayed');

$standby->stop;
$node->stop;
done_testing();
