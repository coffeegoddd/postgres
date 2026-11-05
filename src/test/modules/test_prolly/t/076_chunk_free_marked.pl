
# Copyright (c) 2026, PostgreSQL Global Development Group

# Freeing costs what is freed.  A transaction letting go of a chunk's last
# hold (a pointer moved or dropped, a root let go of) marks its bucket of
# the chunk index, and its commit starts a freer, which frees what nothing
# holds in the buckets marked, and what they alone held, and unmarks them:
# no VACUUM, and no walk of the index.  Two letting go of a chunk's last
# references at once, each seeing the other's left, mark it too.  The
# marks survive a crash, and follow their chunks as buckets split.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('free_marked');
$node->init;
$node->append_conf('postgresql.conf', qq{
shared_preload_libraries = 'test_prolly'
autovacuum = off
log_min_messages = debug1
max_prepared_transactions = 2
});
$node->start;
$node->safe_psql('postgres', q{
	CREATE EXTENSION test_prolly;
	CREATE FUNCTION h(d bytea) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS 'SELECT substr(sha256(d), 1, 20)';
	CREATE FUNCTION leaf(i int) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS $$SELECT test_engine_chunk('L'::bytea || decode(md5('m' || i), 'hex'))$$;
	CREATE FUNCTION node(VARIADIC kids bytea[]) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS $$SELECT test_engine_chunk('R'::bytea || set_byte('\x00'::bytea, 0, cardinality(kids)) ||
			(SELECT string_agg(h(k), '' ORDER BY o) FROM unnest(kids) WITH ORDINALITY u(k, o)))$$;
	CREATE FUNCTION tree(a int, b int) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS 'SELECT node(VARIADIC ARRAY(SELECT leaf(i) FROM generate_series(a, b) i))';
	CREATE FUNCTION marked() RETURNS int8 LANGUAGE sql
		AS $$SELECT test_chunk_index_marked('pg_history.chunk_index')$$;
});
# (no freer running: the next to start is the one a test means)
my $idle = sub {
	my ($name) = @_;
	ok($node->poll_query_until('postgres',
		q{SELECT count(*) = 0 FROM pg_stat_activity WHERE backend_type = 'chunk freer'
			AND query <> 'waiting on transactions holding chunks to free'}),
		$name // '(no freer running)');
};
my $waiting = q{SELECT count(*) FROM pg_stat_activity WHERE backend_type = 'chunk freer'
	AND query = 'waiting on transactions holding chunks to free'};
my $gone = sub {
	my ($hash_sql, $name) = @_;
	ok($node->poll_query_until('postgres',
		"SELECT NOT test_cs_has('pages', $hash_sql)"), $name);
};

# a store of 20,000 chunks a pointer holds, and a small tree another does
$node->safe_psql('postgres', q{
	SELECT count(*) FROM generate_series(1, 20000) i, LATERAL test_cs_put('pages', leaf(i));
	CREATE TABLE parents AS SELECT p, tree(p * 100 + 1, p * 100 + 100) AS d FROM generate_series(0, 199) p;
	SELECT count(*) FROM parents, LATERAL test_cs_put('pages', d);
	SELECT test_cs_put('pages', node(VARIADIC ARRAY(SELECT d FROM parents ORDER BY p)));
	INSERT INTO pg_history.pointers VALUES ('big', h(node(VARIADIC ARRAY(SELECT d FROM parents ORDER BY p))));
	SELECT count(*) FROM generate_series(30001, 30005) i, LATERAL test_cs_put('pages', leaf(i));
	SELECT test_cs_put('pages', tree(30001, 30005));
	INSERT INTO pg_history.pointers VALUES ('small', h(tree(30001, 30005)));
});
is($node->safe_psql('postgres', 'SELECT marked()'), '0', 'no bucket marked');

# the small tree's pointer dropped: freed by the freer its commit starts,
# looking in the buckets marked alone
my $offset = -s $node->logfile;
$node->safe_psql('postgres', q{DELETE FROM pg_history.pointers WHERE name = 'small'});
$gone->(q{h(tree(30001, 30005))}, 'the dropped pointer\'s root freed, no VACUUM');
$gone->(q{h(leaf(30003))}, 'and its leaves');
ok($node->poll_query_until('postgres', 'SELECT marked() = 0'), 'its bucket unmarked');
$node->wait_for_log(qr/chunk freer freed 6 chunks of chunk index \d+, looking in (\d+) buckets marked/, $offset);
my ($looked) = slurp_file($node->logfile, $offset) =~
	/chunk freer freed 6 chunks of chunk index \d+, looking in (\d+) buckets marked/;
# (a bucket a chunk freed, at most: the root's, and those of the leaves
# its freeing let go of)
cmp_ok($looked, '<=', 6, "the freer looked in $looked buckets");
cmp_ok($node->safe_psql('postgres', q{SELECT test_chunk_index_buckets('pg_history.chunk_index')}), '>', 200,
	'in an index of more than 200 buckets');
is($node->safe_psql('postgres', q{SELECT test_cs_get('pages', h(leaf(12345))) = leaf(12345)}), 't',
	'the big tree\'s chunks there');

# two letting go of a child's last two references at once: each sees the
# other's left, and the one after marks it
$node->safe_psql('postgres', q{
	SELECT count(*) FROM generate_series(40000, 40002) i, LATERAL test_cs_put('pages', leaf(i));
	SELECT test_cs_put('pages', node(leaf(40000), leaf(40001)));
	SELECT test_cs_put('pages', node(leaf(40000), leaf(40002)));
});
my $a = $node->background_psql('postgres');
my $b = $node->background_psql('postgres');
$a->query_safe(q{BEGIN; SELECT test_cs_free('pages', ARRAY[h(node(leaf(40000), leaf(40001)))])});
$b->query_safe(q{BEGIN; SELECT test_cs_free('pages', ARRAY[h(node(leaf(40000), leaf(40002)))])});
$a->query_safe('COMMIT');
$b->query_safe('COMMIT');
$a->quit;
$b->quit;
$gone->(q{h(leaf(40000))}, 'the child both let go of freed, no VACUUM');

# marks survive a crash: a pointer dropped, no freer started, the server
# stopped at once
ok($node->poll_query_until('postgres', 'SELECT marked() = 0'), 'every bucket unmarked');
$node->safe_psql('postgres', q{
	SELECT count(*) FROM generate_series(50001, 50003) i, LATERAL test_cs_put('pages', leaf(i));
	SELECT test_cs_put('pages', tree(50001, 50003));
	INSERT INTO pg_history.pointers VALUES ('crash', h(tree(50001, 50003)));
});
$node->safe_psql('postgres', q{SET chunk_free_in_background = off;
	DELETE FROM pg_history.pointers WHERE name = 'crash'});
is($node->safe_psql('postgres', 'SELECT marked()'), '1', 'a bucket marked, no freer started');
$node->stop('immediate');
$node->start;
is($node->safe_psql('postgres', 'SELECT marked()'), '1', 'marked still after the crash');
# (a freer started by the next commit that marks frees it too)
$node->safe_psql('postgres', q{
	SELECT test_cs_put('pages', leaf(50100));
	INSERT INTO pg_history.pointers VALUES ('kick', h(leaf(50100)));
	DELETE FROM pg_history.pointers WHERE name = 'kick';
});
$gone->(q{h(tree(50001, 50003))}, 'and freed by the next freer');

# marks follow their chunks as buckets split: 100 roots let go of, no
# freer started, then enough put to split many buckets
$node->safe_psql('postgres', q{
	SELECT count(*) FROM generate_series(60001, 60100) i, LATERAL test_cs_put('pages', leaf(i));
	INSERT INTO pg_history.pointers SELECT 's' || i, h(leaf(i)) FROM generate_series(60001, 60100) i;
});
$node->safe_psql('postgres', q{SET chunk_free_in_background = off;
	DELETE FROM pg_history.pointers WHERE name LIKE 's%'});
$node->safe_psql('postgres', q{SET chunk_free_in_background = off;
	SELECT count(*) FROM generate_series(70001, 110000) i, LATERAL test_cs_put('pages', leaf(i));
	INSERT INTO pg_history.pointers VALUES ('more', h(leaf(70001)));});
$node->safe_psql('postgres', q{
	SELECT test_cs_put('pages', leaf(60200));
	INSERT INTO pg_history.pointers VALUES ('kick2', h(leaf(60200)));
	DELETE FROM pg_history.pointers WHERE name = 'kick2';
});
ok($node->poll_query_until('postgres',
	q{SELECT count(*) = 0 FROM generate_series(60001, 60100) i WHERE test_cs_has('pages', h(leaf(i)))}),
	'every root let go of before the splits freed');
is($node->safe_psql('postgres', q{SELECT ok FROM test_cs_check('pages')}), 't', 'the audit clean');

# a transaction left open with a change there (prepared) stops no other
# freeing: a freer leaves its bucket, and goes on
$node->safe_psql('postgres', q{
	SELECT count(*) FROM generate_series(80001, 80004) i, LATERAL test_cs_put('pages', leaf(i));
	INSERT INTO pg_history.pointers SELECT 'p' || i, h(leaf(i)) FROM generate_series(80001, 80004) i;
	BEGIN;
	DELETE FROM pg_history.pointers WHERE name = 'p80001';
	PREPARE TRANSACTION 'held';
});
$node->safe_psql('postgres', q{DELETE FROM pg_history.pointers WHERE name = 'p80002'});
$gone->(q{h(leaf(80002))}, 'a pointer dropped as a prepared one is open: freed');
$idle->('the freer, the prepared one\'s bucket left, gone');
$node->safe_psql('postgres', q{DELETE FROM pg_history.pointers WHERE name = 'p80003'});
$gone->(q{h(leaf(80003))}, 'another dropped after: freed, by another freer');
is($node->safe_psql('postgres', q{SELECT test_cs_has('pages', h(leaf(80001)))}), 't',
	'the prepared one\'s not yet');
$node->safe_psql('postgres', q{COMMIT PREPARED 'held'});
$gone->(q{h(leaf(80001))}, 'committed: freed, the waiter on it asking');

# a reference let go of as another transaction lets go of a root of one
# chunk: the root's marks (another changing the counts), and the
# reference's commit asks again (a freer left the bucket for it)
$node->safe_psql('postgres', q{
	SELECT count(*) FROM generate_series(81001, 81002) i, LATERAL test_cs_put('pages', leaf(i));
	SELECT test_cs_put('pages', node(leaf(81001), leaf(81002)));
	INSERT INTO pg_history.pointers VALUES ('x', h(leaf(81001)));
	-- (the parent's other child held too: its freeing marks nothing)
	INSERT INTO pg_history.pointers VALUES ('x2', h(leaf(81002)));
});
$idle->();
my $ra = $node->background_psql('postgres');
$ra->query_safe(q{BEGIN; SELECT test_cs_free('pages', ARRAY[h(node(leaf(81001), leaf(81002)))])});
$offset = -s $node->logfile;
$node->safe_psql('postgres', q{DELETE FROM pg_history.pointers WHERE name = 'x'});
# (the freer the drop started, its pass done before the other commits: it
# waits on the other, the freeing let go of)
ok($node->poll_query_until('postgres', q{SELECT count(*) = 1 FROM pg_stat_activity
	WHERE backend_type = 'chunk freer' AND query = 'waiting on transactions holding chunks to free'}),
	'the freer waiting on the open transaction');
$idle->();
is($node->safe_psql('postgres', q{SELECT test_cs_has('pages', h(leaf(81001)))}), 't',
	'held by the open transaction\'s reference');
$ra->query_safe('COMMIT');
$ra->quit;
$gone->(q{h(leaf(81001))}, 'the reference let go of, committed: freed');

# the store reads its chunks by its index's format, whatever a session
# sets (a session's chunk_format is the format of indexes it makes): a
# chunk naming one not there is refused under the leaf format too
my ($ret, $out, $err) = $node->psql('postgres',
	'SET chunk_format = leaf; SELECT test_cs_put(\'pages\', node(leaf(990001)))');
like($err, qr/which the store does not have/,
	'the history store reads its chunks by its own format, not the session\'s');

# many commits, one freer: a freer held as it begins a pass, twenty
# pointers dropped meanwhile, none registers another; let go, it passes
# again and frees them all
SKIP:
{
	skip 'injection points not supported by this build', 7
		if $ENV{enable_injection_points} ne 'yes';
	$node->safe_psql('postgres', q{
		CREATE EXTENSION injection_points;
		SELECT count(*) FROM generate_series(82001, 82021) i, LATERAL test_cs_put('pages', leaf(i));
		INSERT INTO pg_history.pointers SELECT 'q' || i, h(leaf(i)) FROM generate_series(82001, 82021) i;
		SELECT injection_points_attach('chunk-freer-pass', 'wait');
	});
	my $offset = -s $node->logfile;
	$node->safe_psql('postgres', q{DELETE FROM pg_history.pointers WHERE name = 'q82001'});
	$node->wait_for_event('chunk freer', 'chunk-freer-pass');
	$node->safe_psql('postgres', qq{DELETE FROM pg_history.pointers WHERE name = 'q$_'})
		for 82002 .. 82021;
	my @started = slurp_file($node->logfile, $offset) =~ /starting background worker process "chunk freer/g;
	is(scalar(@started), 1, 'twenty-one commits, one freer');
	$node->safe_psql('postgres', q{
		SELECT injection_points_detach('chunk-freer-pass');
		SELECT injection_points_wakeup('chunk-freer-pass');
	});
	ok($node->poll_query_until('postgres',
		q{SELECT count(*) = 0 FROM generate_series(82001, 82021) i WHERE test_cs_has('pages', h(leaf(i)))}),
		'all freed by it, passing again');
	@started = slurp_file($node->logfile, $offset) =~ /starting background worker process "chunk freer/g;
	is(scalar(@started), 1, 'and none more (its own commits ask for none)');

	# a transaction ending between a freer's freeing and its unmarking:
	# the chunk it let go of (held as the freer freed) freed again, once
	$node->safe_psql('postgres', q{
		SELECT count(*) FROM generate_series(83001, 83002) i, LATERAL test_cs_put('pages', leaf(i));
		SELECT test_cs_put('pages', node(leaf(83001), leaf(83002)));
		INSERT INTO pg_history.pointers VALUES ('y', h(leaf(83001)));
		INSERT INTO pg_history.pointers VALUES ('y2', h(leaf(83002)));
	});
	$idle->();
	$node->safe_psql('postgres', q{SELECT injection_points_attach('chunk-freer-freed', 'wait')});
	my $ya = $node->background_psql('postgres');
	$ya->query_safe(q{BEGIN; SELECT test_cs_free('pages', ARRAY[h(node(leaf(83001), leaf(83002)))])});
	$node->safe_psql('postgres', q{DELETE FROM pg_history.pointers WHERE name = 'y'});
	$node->wait_for_event('chunk freer', 'chunk-freer-freed');
	is($node->safe_psql('postgres', q{SELECT test_cs_has('pages', h(leaf(83001)))}), 't',
		'held as the freer freed');
	$ya->query_safe('COMMIT');
	$ya->quit;
	$node->safe_psql('postgres', q{
		SELECT injection_points_detach('chunk-freer-freed');
		SELECT injection_points_wakeup('chunk-freer-freed');
	});
	$gone->(q{h(leaf(83001))}, 'a transaction ended between freeing and unmarking: its chunk freed');
	$idle->();
}

# noted, and ending, as the pass that noted it ends: waited on all the same
# (it ended), asked again
SKIP:
{
	skip 'injection points not supported by this build', 20
		if $ENV{enable_injection_points} ne 'yes';
	my $race = sub {
		my ($tag, $ending) = @_;
		$node->safe_psql('postgres', qq{
			SELECT count(*) FROM generate_series(${tag}1, ${tag}2) i, LATERAL test_cs_put('pages', leaf(i));
			SELECT test_cs_put('pages', node(leaf(${tag}1), leaf(${tag}2)));
			INSERT INTO pg_history.pointers VALUES ('r$tag', h(leaf(${tag}1))), ('s$tag', h(leaf(${tag}2)));
		});
		$idle->();
		my $t1 = $node->background_psql('postgres');
		$t1->query_safe(qq{BEGIN; SELECT test_cs_free('pages', ARRAY[h(node(leaf(${tag}1), leaf(${tag}2)))])});
		$node->safe_psql('postgres', q{SELECT injection_points_attach('chunk-freer-passed', 'wait')});
		$node->safe_psql('postgres', qq{DELETE FROM pg_history.pointers WHERE name = 'r$tag'});
		$node->wait_for_event('chunk freer', 'chunk-freer-passed');
		$t1->query_safe($ending);
		$t1->quit;
		$node->safe_psql('postgres', q{
			SELECT injection_points_detach('chunk-freer-passed');
			SELECT injection_points_wakeup('chunk-freer-passed');
		});
	};
	$race->(8401, 'COMMIT');
	$gone->(q{h(leaf(84011))}, 'the reference let go of committed as the pass noting it ended: freed');

	# (aborted: the reference held again, the root the one let go of)
	$race->(8402, 'ROLLBACK');
	is($node->safe_psql('postgres', q{SELECT test_cs_has('pages', h(leaf(84021)))}), 't',
		'aborted: held by the reference again');

	# two running in its bucket, one let go of a reference, one taking a
	# root, both ended (one aborted): its chunk freed
	$node->safe_psql('postgres', q{
		SELECT count(*) FROM generate_series(85001, 85002) i, LATERAL test_cs_put('pages', leaf(i));
		SELECT test_cs_put('pages', node(leaf(85001), leaf(85002)));
		INSERT INTO pg_history.pointers VALUES ('z', h(leaf(85001))), ('z2', h(leaf(85002)));
	});
	$idle->();
	my $t1 = $node->background_psql('postgres');
	my $t3 = $node->background_psql('postgres');
	$t1->query_safe(q{BEGIN; SELECT test_cs_free('pages', ARRAY[h(node(leaf(85001), leaf(85002)))])});
	$t3->query_safe(q{BEGIN; SELECT test_cs_ref_root('pages', h(leaf(85001)))});
	$node->safe_psql('postgres', q{DELETE FROM pg_history.pointers WHERE name = 'z'});
	ok($node->poll_query_until('postgres', "SELECT ($waiting) = 1"), 'a waiter on them');
	$t1->query_safe('COMMIT');
	$t3->query_safe('ROLLBACK');
	$t1->quit;
	$t3->quit;
	$gone->(q{h(leaf(85001))}, 'two running there, one committed, one aborted: freed');

	# a transaction that marks and is prepared: a pass as it prepares, the
	# waiter on it until it is committed (no commit after asking)
	$node->safe_psql('postgres', q{
		SELECT test_cs_put('pages', leaf(88001));
		INSERT INTO pg_history.pointers VALUES ('pp', h(leaf(88001)));
	});
	$idle->();
	$node->safe_psql('postgres', q{BEGIN; DELETE FROM pg_history.pointers WHERE name = 'pp';
		PREPARE TRANSACTION 'pp'});
	ok($node->poll_query_until('postgres', "SELECT ($waiting) = 1"), 'prepared: a waiter on it');
	$node->safe_psql('postgres', q{COMMIT PREPARED 'pp'});
	$gone->(q{h(leaf(88001))}, 'the prepared drop committed: freed, nothing else asking');

	# one waited on long, another noted after and ended: the other's freed
	# while the long one runs
	my $hold = sub {
		my ($tag) = @_;
		$node->safe_psql('postgres', qq{
			SELECT count(*) FROM generate_series(${tag}1, ${tag}2) i, LATERAL test_cs_put('pages', leaf(i));
			SELECT test_cs_put('pages', node(leaf(${tag}1), leaf(${tag}2)));
			INSERT INTO pg_history.pointers VALUES ('h$tag', h(leaf(${tag}1))), ('k$tag', h(leaf(${tag}2)));
		});
		$idle->();
		my $sess = $node->background_psql('postgres');
		$sess->query_safe(qq{BEGIN; SELECT test_cs_free('pages', ARRAY[h(node(leaf(${tag}1), leaf(${tag}2)))])});
		$node->safe_psql('postgres', qq{DELETE FROM pg_history.pointers WHERE name = 'h$tag'});
		return $sess;
	};
	my $long = $hold->(8901);
	ok($node->poll_query_until('postgres', "SELECT ($waiting) = 1"), 'a waiter on the long one');
	my $short = $hold->(8902);
	$idle->();
	$short->query_safe('COMMIT');
	$short->quit;
	$gone->(q{h(leaf(89021))}, 'the short one ended: freed, the long one running');
	$long->query_safe('COMMIT');
	$long->quit;
	$gone->(q{h(leaf(89011))}, 'the long one ended: freed');
	ok($node->poll_query_until('postgres', "SELECT ($waiting) = 0"), 'no waiter left');

	# a waiter stopped: the next freer waits in its stead
	my $t = $hold->(8903);
	ok($node->poll_query_until('postgres', "SELECT ($waiting) = 1"), 'a waiter');
	my $old = $node->safe_psql('postgres', qq{SELECT pid FROM pg_stat_activity
		WHERE backend_type = 'chunk freer' AND query = 'waiting on transactions holding chunks to free'});
	$node->safe_psql('postgres', "SELECT pg_terminate_backend($old)");
	ok($node->poll_query_until('postgres', "SELECT ($waiting AND pid <> $old) = 1"),
		'another waiting in its stead');
	$t->query_safe('COMMIT');
	$t->quit;
	$gone->(q{h(leaf(89031))}, 'the waiter stopped, the transaction ended: freed');

	# a freer stopped by an error lets the freeing go: the next commit's
	# freer frees
	$node->safe_psql('postgres', q{
		SELECT count(*) FROM generate_series(86001, 86002) i, LATERAL test_cs_put('pages', leaf(i));
		INSERT INTO pg_history.pointers VALUES ('e1', h(leaf(86001))), ('e2', h(leaf(86002)));
	});
	$idle->();
	$node->safe_psql('postgres', q{SELECT injection_points_attach('chunk-freer-pass', 'error')});
	my $offset = -s $node->logfile;
	$node->safe_psql('postgres', q{DELETE FROM pg_history.pointers WHERE name = 'e1'});
	$node->wait_for_log(qr/error triggered for injection point chunk-freer-pass/, $offset);
	$node->safe_psql('postgres', q{SELECT injection_points_detach('chunk-freer-pass')});
	$idle->();
	$node->safe_psql('postgres', q{DELETE FROM pg_history.pointers WHERE name = 'e2'});
	$gone->(q{h(leaf(86001))}, 'a freer stopped by an error: the next frees');
	$gone->(q{h(leaf(86002))}, '(and its own)');

	# VACUUM's freer, the freeing another's: frees nothing, and asks it to
	# pass again (a chunk put and never held, VACUUM's mark alone, freed by
	# the other's pass after)
	$idle->();
	$node->safe_psql('postgres', q{SELECT injection_points_attach('chunk-freer-passed', 'wait')});
	$node->safe_psql('postgres', q{
		SELECT test_cs_put('pages', leaf(87001)), test_cs_put('pages', leaf(87002));
		INSERT INTO pg_history.pointers VALUES ('v', h(leaf(87001)));
		DELETE FROM pg_history.pointers WHERE name = 'v';
	});
	$node->wait_for_event('chunk freer', 'chunk-freer-passed');
	$offset = -s $node->logfile;
	$node->safe_psql('postgres', 'VACUUM pg_history.chunk_index');
	$node->wait_for_log(qr/chunk freer freed 0 chunks of chunk index \d+, looking in 0 buckets/, $offset);
	$node->safe_psql('postgres', q{
		SELECT injection_points_detach('chunk-freer-passed');
		SELECT injection_points_wakeup('chunk-freer-passed');
	});
	$gone->(q{h(leaf(87002))}, 'VACUUM\'s freer, the freeing another\'s: the other passes again, freeing what VACUUM marked');
}

$node->stop;
done_testing();
