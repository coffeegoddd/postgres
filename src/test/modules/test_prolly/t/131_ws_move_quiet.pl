# Copyright (c) 2026, PostgreSQL Global Development Group

# No user code runs during a move in place: not a trigger or rule of a table it writes, enabled
# ALWAYS, REPLICA or as made, nor an event trigger of any event, enabled ALWAYS or as made, for
# what the move drops, makes, alters or rewrites, nor for what its enum step adds.  Each runs for
# the same things done by hand.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('move_quiet');
$node->init(allows_streaming => 'logical');
$node->append_conf('postgresql.conf', "autovacuum = off\ncapture_to_trees = on\ndebug_block_mapped_heap = on\n");
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_prolly');

my $branch = $node->safe_psql('postgres', 'SELECT pg_branch_id_allocate()');
my $on = "SET catalog_branch = $branch; SELECT test_ws_use_history_store() IS NULL AS on_history \\gset\n";

sub sql
{
	my ($sql) = @_;
	return $node->safe_psql('postgres', "$on$sql");
}

my $commits = 0;
sub commit_root
{
	$commits++;
	return sql(qq{SELECT test_capture_commit_on('app', 'main', 'c$commits')});
}

sub move_to
{
	my ($root, $name) = @_;
	sql(qq{SELECT test_ws_move_prepare('app', '$root')});
	sql(qq{SELECT test_ws_move('app', '$root')});
	is(sql(q{SELECT test_ws_root_capture('app')}), $root, "$name: the tables hold the root");
	is(sql(q{SELECT test_capture_barrier('app')}), $root, "$name: and capture has it");
}

# Event triggers of every event, outside the schema, as they are the database's: each enabled
# ALWAYS, and as made (which runs where session_replication_role is origin).
sql(q{CREATE FUNCTION public.loud_always() RETURNS event_trigger LANGUAGE plpgsql AS
		$$BEGIN RAISE WARNING 'user code ran: event always % %', TG_EVENT, TG_TAG; END$$;
	CREATE FUNCTION public.loud_origin() RETURNS event_trigger LANGUAGE plpgsql AS
		$$BEGIN RAISE WARNING 'user code ran: event origin % %', TG_EVENT, TG_TAG; END$$;
	CREATE EVENT TRIGGER e_start_always ON ddl_command_start EXECUTE FUNCTION public.loud_always();
	ALTER EVENT TRIGGER e_start_always ENABLE ALWAYS;
	CREATE EVENT TRIGGER e_end_always ON ddl_command_end EXECUTE FUNCTION public.loud_always();
	ALTER EVENT TRIGGER e_end_always ENABLE ALWAYS;
	CREATE EVENT TRIGGER e_drop_always ON sql_drop EXECUTE FUNCTION public.loud_always();
	ALTER EVENT TRIGGER e_drop_always ENABLE ALWAYS;
	CREATE EVENT TRIGGER e_rewrite_always ON table_rewrite EXECUTE FUNCTION public.loud_always();
	ALTER EVENT TRIGGER e_rewrite_always ENABLE ALWAYS;
	CREATE EVENT TRIGGER e_end_origin ON ddl_command_end EXECUTE FUNCTION public.loud_origin();
	CREATE EVENT TRIGGER e_drop_origin ON sql_drop EXECUTE FUNCTION public.loud_origin()});

# (a role whose objects can travel in a commit, and that a commit marks)
$node->safe_psql('postgres', q{CREATE ROLE app_owner; SELECT test_ws_role_mark(test_ws_role_bytes(ARRAY['app_owner']))});
sql(q{CREATE SCHEMA app VERSIONED; GRANT ALL ON SCHEMA app TO app_owner});

# Tables with a trigger of each enabling, and a rule enabled ALWAYS.
sql(q{SET ROLE app_owner; SET search_path = app;
	CREATE TYPE mood AS ENUM ('sad', 'happy');
	CREATE FUNCTION app.loud() RETURNS trigger LANGUAGE plpgsql AS
		$$BEGIN RAISE WARNING 'user code ran: trigger % % %', TG_NAME, TG_TABLE_NAME, TG_OP; RETURN NEW; END$$;
	CREATE FUNCTION app.loud_rule(what text) RETURNS void LANGUAGE plpgsql AS
		$$BEGIN RAISE WARNING 'user code ran: rule %', what; END$$;
	CREATE TABLE kept (id integer PRIMARY KEY, v integer, m mood);
	CREATE TABLE altered (id integer PRIMARY KEY, v integer);
	CREATE TABLE gone (id integer PRIMARY KEY);
	CREATE TRIGGER k_always BEFORE INSERT OR UPDATE OR DELETE ON kept FOR EACH ROW EXECUTE FUNCTION app.loud();
	ALTER TABLE kept ENABLE ALWAYS TRIGGER k_always;
	CREATE TRIGGER k_replica AFTER INSERT OR UPDATE OR DELETE ON kept FOR EACH ROW EXECUTE FUNCTION app.loud();
	ALTER TABLE kept ENABLE REPLICA TRIGGER k_replica;
	CREATE TRIGGER k_origin AFTER INSERT OR UPDATE OR DELETE ON kept FOR EACH ROW EXECUTE FUNCTION app.loud();
	CREATE TRIGGER k_off AFTER INSERT OR UPDATE OR DELETE ON kept FOR EACH ROW EXECUTE FUNCTION app.loud();
	ALTER TABLE kept DISABLE TRIGGER k_off;
	CREATE TRIGGER k_statement AFTER INSERT OR UPDATE OR DELETE OR TRUNCATE ON kept
		FOR EACH STATEMENT EXECUTE FUNCTION app.loud();
	ALTER TABLE kept ENABLE ALWAYS TRIGGER k_statement;
	CREATE TRIGGER a_always BEFORE INSERT OR UPDATE OR DELETE ON altered FOR EACH ROW EXECUTE FUNCTION app.loud();
	ALTER TABLE altered ENABLE ALWAYS TRIGGER a_always;
	CREATE RULE k_rule AS ON INSERT TO kept DO ALSO SELECT app.loud_rule('k_rule');
	ALTER TABLE kept ENABLE ALWAYS RULE k_rule;
	CREATE RULE a_rule AS ON UPDATE TO altered DO ALSO SELECT app.loud_rule('a_rule');
	ALTER TABLE altered ENABLE ALWAYS RULE a_rule;
	CREATE TABLE counted (id serial PRIMARY KEY);
	INSERT INTO kept VALUES (1, 1, 'sad'), (2, 2, 'happy');
	INSERT INTO counted DEFAULT VALUES;
	CREATE MATERIALIZED VIEW kept_seen AS SELECT count(*) AS n FROM kept;
	INSERT INTO altered VALUES (1, 1);
	INSERT INTO gone VALUES (1)});
my $first = commit_root();

# The second: rows changed, a column added and one retyped (a rewrite), a table dropped and one
# made, an index and a view made, and the enum given a value a row uses.
sql(q{SET ROLE app_owner; SET search_path = app;
	ALTER TYPE mood ADD VALUE 'meh'});
sql(q{SET ROLE app_owner; SET search_path = app;
	UPDATE kept SET v = v + 10; DELETE FROM kept WHERE id = 1; INSERT INTO kept VALUES (3, 3, 'meh');
	ALTER TABLE altered ADD COLUMN w text;
	ALTER TABLE altered ALTER COLUMN v TYPE bigint;
	UPDATE altered SET w = 'w';
	DROP TABLE gone;
	CREATE TABLE made (id integer PRIMARY KEY); INSERT INTO made VALUES (1);
	CREATE TRIGGER m_replica AFTER INSERT ON made FOR EACH ROW EXECUTE FUNCTION app.loud();
	ALTER TABLE made ENABLE REPLICA TRIGGER m_replica;
	INSERT INTO counted DEFAULT VALUES;
	REFRESH MATERIALIZED VIEW kept_seen;
	CREATE INDEX kept_v ON kept (v);
	CREATE VIEW seen AS SELECT id FROM kept});

# (a REPLICA trigger runs where session_replication_role is replica)
sql(q{SET session_replication_role = replica; INSERT INTO app.made VALUES (2)});
my $second = commit_root();

# Each trigger's enabling, of the tables a move keeps, alters or makes again.
sub enablings
{
	return sql(q{SELECT string_agg(tgname || '=' || tgenabled::text, ',' ORDER BY tgname) FROM pg_trigger
		WHERE tgrelid IN (SELECT oid FROM pg_class WHERE relnamespace = 'app'::regnamespace) AND NOT tgisinternal});
}
my $second_enablings = enablings();

# (what ran for what was done by hand: each kind of user code here runs)
my $log = slurp_file($node->logfile);
for my $what ('event always ddl_command_start', 'event always ddl_command_end', 'event always sql_drop',
	'event always table_rewrite', 'event origin ddl_command_end', 'event origin sql_drop', 'trigger k_always',
	'trigger k_origin', 'trigger k_statement', 'trigger a_always', 'trigger m_replica', 'rule k_rule', 'rule a_rule')
{
	like($log, qr/user code ran: $what/, "by hand, $what runs");
}

unlike($log, qr/user code ran: trigger k_off/, 'by hand, a trigger disabled does not');
my $log_at = -s $node->logfile;
move_to($first, 'to the first');
move_to($second, 'to the second');
is(enablings(), $second_enablings, 'each trigger enabled as it was, its table kept or made again');
move_to($first, 'to the first again');
unlike(slurp_file($node->logfile, $log_at), qr/user code ran/, 'no trigger, rule or event trigger ran during the moves');

done_testing();
