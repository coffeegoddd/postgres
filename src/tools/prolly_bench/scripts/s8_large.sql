-- S8: one transaction updating n consecutive rows.  Defines: scale, n.
\set lo random(1, 100000 * :scale - :n + 1)
\set hi :lo + :n - 1
UPDATE pgbench_accounts SET abalance = abalance + 1 WHERE aid BETWEEN :lo AND :hi;
