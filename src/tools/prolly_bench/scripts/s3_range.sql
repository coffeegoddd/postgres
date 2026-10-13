-- S3: range scans over pgbench_accounts.  Defines: scale, range.
-- Arithmetic is done by pgbench, so prepared statements get typed values.
\set lo random(1, 100000 * :scale - :range)
\set hi :lo + :range - 1
SELECT sum(abalance) FROM pgbench_accounts WHERE aid BETWEEN :lo AND :hi;
