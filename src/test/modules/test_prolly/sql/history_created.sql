-- What a fragment's commits made is the same however it is found: walked
-- with their parents here, or, past an older fragment's sever, from the
-- delta kept for its boundary.  Each shape of history below is severed in
-- two fragments, let go of, and repaired from a remote, which verifies the
-- set found at the sever against the set found at the repair.

\getenv abs_builddir PG_ABS_BUILDDIR
\getenv abs_srcdir PG_ABS_SRCDIR
\set shape_file :abs_srcdir '/sql/history_created_shape.psql'
\set dir :abs_builddir '/tmp_check/history_created'
SELECT 'file://' || :'dir' || '/' || txid_current() AS url \gset
SELECT current_database() AS home \gset
\set VERBOSITY terse

\set shape revert
\i :shape_file
\set shape resurrect
\i :shape_file
\set shape append
\i :shape_file
\set shape merge
\i :shape_file
\set shape create_drop
\i :shape_file
