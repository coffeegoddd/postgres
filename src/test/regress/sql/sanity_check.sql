VACUUM;

--
-- Sanity check: every system catalog that has OIDs should have
-- a unique index on OID.  This ensures that the OIDs will be unique,
-- even after the OID counter wraps around.  A catalog that can hold a
-- versioned object's rows has a branch column after the OID, since such an
-- object has its OID once per branch.
-- We exclude non-system tables from the check by looking at nspname.
--
SELECT relname, nspname
 FROM pg_class c LEFT JOIN pg_namespace n ON n.oid = relnamespace JOIN pg_attribute a ON (attrelid = c.oid AND attname = 'oid')
 WHERE relkind = 'r' and c.oid < 16384
     AND ((nspname ~ '^pg_') IS NOT FALSE)
     AND NOT EXISTS (SELECT 1 FROM pg_index i WHERE indrelid = c.oid
                     AND indkey[0] = a.attnum
                     AND (indnatts = 1 OR
                          (indnatts = 2 AND
                           (SELECT attname FROM pg_attribute b
                            WHERE b.attrelid = c.oid AND b.attnum = indkey[1]) ~ 'branch$'))
                     AND indisunique AND indimmediate);

-- check that relations without storage don't have relfilenode
SELECT relname, relkind
  FROM pg_class
 WHERE relkind IN ('v', 'c', 'f', 'p', 'I')
       AND relfilenode <> 0;
