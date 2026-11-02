-- The object store conformance suite against "directory": objects as files
-- under a directory (here one in the data directory).  An unknown
-- implementation is an error, as is a directory store with no directory.
CREATE EXTENSION IF NOT EXISTS test_prolly;
SELECT test_os_size('no_such_store', 'x', 'a');
SELECT test_os_size('directory', '', 'a');
\set store directory
\set location pg_test_object_store
\getenv abs_srcdir PG_ABS_SRCDIR
\set suite :abs_srcdir '/sql/object_store_suite.inc'
\i :suite
