# Copyright (c) 2026, PostgreSQL Global Development Group

# A status takes the last fold of its branch known, in any backend, once
# writers have ended since: the working set is that fold's root but for
# the relations written since (status_from_fold).  Where that can't be
# told so it compares as before, and answers the same: a fold that left
# objects out, one from another baseline, a snapshot older than a write
# that ended since, another backend's writer still open, a catalog change
# since the fold (DDL right after a commit), and a hard reset.  Each
# answer is checked against a fold forced to read everything.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('status_from_fold');
$node->init;
$node->append_conf('postgresql.conf', q{
autovacuum = off
prolly_fold_worker = off
});
$node->start;

sub sql { return $node->safe_psql('postgres', $_[0]); }

my $status = q{SELECT coalesce(string_agg(table_name || ':' || status, ',' ORDER BY table_name), '')
	FROM dolt_status};
my $full = qq{SET debug_prolly_force_full_fold = on; $status; RESET debug_prolly_force_full_fold};

# statuses answered from the last fold so far, counted by every backend
sub from_fold
{
	return sql(q{SELECT pg_stat_force_next_flush();
		SELECT value FROM pg_stat_prolly WHERE name = 'status_from_fold'});
}

# A status in a new session: its answer, whether it was from the last
# fold, and whether a fold forced to read everything answers the same.
sub fresh_status
{
	my ($name, $want, $taken) = @_;
	my $before = from_fold();
	my $got = sql($status);
	my $after = from_fold();
	is($got, $want, "$name: the status's answer");
	is($after - $before, $taken ? 1 : 0,
		"$name: " . ($taken ? 'taken from the last fold' : 'not taken from the last fold'));
	is(sql($full), $got, "$name: a full fold's answer is the same");
}

sql(q{
	CREATE SCHEMA s VERSIONED;
	DO $$ BEGIN FOR i IN 1..10 LOOP
		EXECUTE format('CREATE TABLE s.t%s (id int PRIMARY KEY, v int)', i);
		EXECUTE format('INSERT INTO s.t%s VALUES (1, 0)', i);
	END LOOP; END $$;
	SELECT dolt_commit('-Am', 'tables');
});

# after a commit, a write by another backend: taken, the table written alone
sql(q{UPDATE s.t1 SET v = 1});
fresh_status('after a commit', 's.t1:modified', 1);
sql(q{SELECT dolt_commit('-am', 't1')});

# a repeatable read snapshot older than a write that ended since
my $rr = $node->background_psql('postgres');
$rr->query_safe(q{BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM s.t2});
sql(q{UPDATE s.t2 SET v = 1});
my $before = from_fold();
my $rr_got = $rr->query_safe($status);
my $rr_full = $rr->query_safe($full);
$rr->query_safe(q{COMMIT});
$rr->quit;
is($rr_got, '', 'older snapshot: the write it doesn\'t see isn\'t in its answer');
is(from_fold() - $before, 0, 'older snapshot: not taken from the last fold');
is($rr_full, $rr_got, 'older snapshot: a full fold\'s answer is the same');
fresh_status('older snapshot, later', 's.t2:modified', 1);
sql(q{SELECT dolt_commit('-am', 't2')});

# another backend's writer still open
my $writer = $node->background_psql('postgres');
$writer->query_safe(q{BEGIN; UPDATE s.t3 SET v = 1});
sql(q{UPDATE s.t4 SET v = 1});
fresh_status('a writer open', 's.t4:modified', 0);
$writer->query_safe(q{ROLLBACK});
$writer->quit;
fresh_status('the writer rolled back', 's.t4:modified', 1);
sql(q{SELECT dolt_commit('-am', 't4')});

# a catalog change right after a commit
sql(q{ALTER TABLE s.t5 ADD COLUMN w int});
fresh_status('DDL after a commit', 's.t5:modified', 0);
sql(q{SELECT dolt_commit('-am', 't5')});

# a hard reset of written rows
sql(q{UPDATE s.t6 SET v = 1});
fresh_status('before a hard reset', 's.t6:modified', 1);
sql(q{SELECT dolt_reset('--hard')});
fresh_status('after a hard reset', '', 0);

# a fold from another baseline: the commit forced to fold in full
# publishes none, and moves the baseline
sql(q{UPDATE s.t7 SET v = 1});
fresh_status('before a forced commit', 's.t7:modified', 1);
sql(q{SET debug_prolly_force_full_fold = on; SELECT dolt_commit('-am', 't7')});
sql(q{UPDATE s.t8 SET v = 1});
fresh_status('another baseline', 's.t8:modified', 0);
sql(q{SELECT dolt_commit('-am', 't8')});

# a fold that left objects out (a foreign table with a trigger, which a
# commit can't hold): its names reported as not covered
sql(q{
	CREATE FOREIGN DATA WRAPPER sff_fdw;
	CREATE SERVER sff_srv FOREIGN DATA WRAPPER sff_fdw;
	CREATE FUNCTION public.sff_noop() RETURNS trigger LANGUAGE plpgsql AS 'BEGIN RETURN NEW; END';
	CREATE FOREIGN TABLE s.ft (id int) SERVER sff_srv;
	CREATE TRIGGER noop BEFORE INSERT ON s.ft FOR EACH ROW EXECUTE FUNCTION public.sff_noop();
});
is(sql($status), 's.ft:not covered', 'objects left out: reported');
# (a checkpoint's fold, published and known: it left the foreign table out)
sql(q{SELECT length(prolly_checkpoint()) > 0});
is(sql($status), 's.ft:not covered', 'objects left out: reported after a checkpoint');
sql(q{UPDATE s.t9 SET v = 1});
fresh_status('a fold that left objects out', 's.ft:not covered,s.t9:modified', 0);

$node->stop;
done_testing();
