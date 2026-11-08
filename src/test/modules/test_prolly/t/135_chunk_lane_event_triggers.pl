# Copyright (c) 2026, PostgreSQL Global Development Group

# An append to a chunk log that makes it a new lane runs no user's event trigger: the lane's
# making is the engine's, not DDL of the appender's, so an event trigger of each DDL event, enabled
# ALWAYS, that raises does not fail it.  DDL done by hand still runs them.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('lane_events');
# (the history store holds raw bytes here: initdb makes it with the leaf format, which references
# no chunk)
$node->init(extra => ['-c', 'chunk_format=leaf']);
$node->append_conf('postgresql.conf', "autovacuum = off\n");
$node->start;
$node->safe_psql('postgres', q{
	CREATE EXTENSION test_prolly;
	CREATE FUNCTION d(i int8) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS $$SELECT convert_to(repeat('lane chunk ' || i || ' ', 300), 'UTF8')$$;
	CREATE FUNCTION h(b bytea) RETURNS bytea LANGUAGE sql IMMUTABLE
		AS 'SELECT substr(sha256(b), 1, 20)';
});
my $lanes = q{SELECT count(*) FROM pg_class WHERE relname LIKE 'chunks_lane_%'
	AND relnamespace = 'pg_history'::regnamespace};

# A backend that holds a lane, so that the next to append needs one of its own.
my $holder = $node->background_psql('postgres');
$holder->query_safe(q{SELECT (test_cs_put('pages', d(1))).stored});
my $before = $node->safe_psql('postgres', $lanes);

$node->safe_psql('postgres', q{
	CREATE FUNCTION public.refuse() RETURNS event_trigger LANGUAGE plpgsql AS
		$$BEGIN RAISE EXCEPTION 'user code ran: % %', TG_EVENT, TG_TAG; END$$;
	CREATE EVENT TRIGGER e_start ON ddl_command_start EXECUTE FUNCTION public.refuse();
	ALTER EVENT TRIGGER e_start ENABLE ALWAYS;
	CREATE EVENT TRIGGER e_end ON ddl_command_end EXECUTE FUNCTION public.refuse();
	ALTER EVENT TRIGGER e_end ENABLE ALWAYS;
});

my ($rc, $out, $err) = $node->psql('postgres', q{SELECT (test_cs_put('pages', d(2))).stored});
is($rc, 0, 'an append that makes a new lane succeeds') or diag($err);
unlike($err, qr/user code ran/, 'and runs no event trigger');
is($node->safe_psql('postgres', $lanes), $before + 1, 'a lane was made');
is($node->safe_psql('postgres', q{SELECT test_cs_get('pages', h(d(2))) = d(2)}), 't', 'and the chunk reads back');

($rc, $out, $err) = $node->psql('postgres', q{CREATE TABLE public.by_hand (id integer)});
like($err, qr/user code ran: ddl_command_start CREATE TABLE/, 'DDL by hand runs them');

$holder->quit;
$node->stop;
done_testing();
