-- The conformance suites, run against every registered implementation of
-- each interface.  The example interface test_checksum has one correct
-- implementation, which passes, and one that ignores its input's last
-- byte, which the suite must catch.
CREATE EXTENSION IF NOT EXISTS test_prolly;

-- (the checksums themselves, which a failure shows, depend on the
-- platform's byte order, so they are left out)
SELECT iface, implementation, test, result,
       regexp_replace(detail, ' \([0-9]+ vs [0-9]+\)$', '') AS detail
FROM test_prolly_conformance();

-- a failure says where its check is
SELECT location ~ '^test_checksum\.c:[0-9]+$' AS has_location
FROM test_prolly_conformance('test_checksum')
WHERE result = 'failed';

SELECT * FROM test_prolly_conformance('no_such_interface');
