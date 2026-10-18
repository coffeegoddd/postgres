-- Objects of versioned schemas the canonical form doesn't describe are
-- refused by a commit and listed as not covered, never left out of a
-- commit's hash: a clone or checkout wouldn't have them.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS test_prolly;
RESET client_min_messages;

CREATE SCHEMA ud VERSIONED;
CREATE TABLE ud.t (id int PRIMARY KEY, a int, b int);
SELECT length(dolt_commit('-Am', 'table')::text) AS committed;

CREATE OPERATOR ud.=== (LEFTARG = int, RIGHTARG = int, FUNCTION = int4eq);
CREATE OPERATOR FAMILY ud.fam USING btree;
CREATE OPERATOR CLASS ud.cls FOR TYPE int4 USING hash AS
  OPERATOR 1 =, FUNCTION 1 hashint4(int4);
CREATE CONVERSION ud.conv FOR 'LATIN1' TO 'UTF8' FROM iso8859_1_to_utf8;
CREATE TEXT SEARCH CONFIGURATION ud.cfg (COPY = pg_catalog.simple);
CREATE TEXT SEARCH DICTIONARY ud.dict (TEMPLATE = pg_catalog.simple);
CREATE TEXT SEARCH PARSER ud.prs (START = prsd_start, GETTOKEN = prsd_nexttoken,
  END = prsd_end, LEXTYPES = prsd_lextype);
CREATE TEXT SEARCH TEMPLATE ud.tmpl (INIT = dsimple_init, LEXIZE = dsimple_lexize);

SELECT * FROM dolt_status WHERE status = 'not covered' ORDER BY table_name COLLATE "C";
SELECT dolt_commit('-Am', 'refused');

-- each one gone, the commit goes through
DROP OPERATOR ud.=== (int, int);
DROP OPERATOR CLASS ud.cls USING hash;
DROP OPERATOR FAMILY ud.cls USING hash;
DROP OPERATOR FAMILY ud.fam USING btree;
DROP CONVERSION ud.conv;
DROP TEXT SEARCH CONFIGURATION ud.cfg;
DROP TEXT SEARCH DICTIONARY ud.dict;
DROP TEXT SEARCH PARSER ud.prs;
DROP TEXT SEARCH TEMPLATE ud.tmpl;
SELECT * FROM dolt_status WHERE status = 'not covered';
SELECT length(dolt_commit('-Am', 'none')::text) AS committed;

-- only a commit refuses them: a move discards them as uncommitted work
-- (a reset), or leaves them on their branch (a checkout); a branch's first
-- use doesn't take them from the branch it was copied from
CREATE OPERATOR ud.=== (LEFTARG = int, RIGHTARG = int, FUNCTION = int4eq);
CREATE FOREIGN DATA WRAPPER ud_fdw;
CREATE SERVER ud_srv FOREIGN DATA WRAPPER ud_fdw;
CREATE FOREIGN TABLE ud.ft (a int) SERVER ud_srv;
SELECT dolt_reset('--hard');
SELECT count(*) AS operators FROM pg_operator WHERE oprnamespace = 'ud'::regnamespace;
SELECT count(*) AS foreign_tables FROM pg_class WHERE relnamespace = 'ud'::regnamespace
  AND relkind = 'f';
SELECT dolt_branch('ud_other');
CREATE OPERATOR ud.=== (LEFTARG = int, RIGHTARG = int, FUNCTION = int4eq);
SELECT dolt_checkout('ud_other');
SELECT count(*) AS operators FROM pg_operator WHERE oprnamespace = 'ud'::regnamespace;
SELECT dolt_checkout('main');
SELECT count(*) AS operators FROM pg_operator WHERE oprnamespace = 'ud'::regnamespace;
SELECT * FROM dolt_status WHERE status = 'not covered';
SELECT dolt_reset('--hard');
SELECT dolt_branch('-D', 'ud_other');
DROP SERVER ud_srv;
DROP FOREIGN DATA WRAPPER ud_fdw;

DROP SCHEMA ud CASCADE;
SELECT length(dolt_commit('-Am', 'ud gone')::text) AS committed;
