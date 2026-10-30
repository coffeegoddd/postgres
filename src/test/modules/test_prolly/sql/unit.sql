-- The unit tests of the engine's code in the server.  Each row is one test;
-- a failure shows its error in detail.
CREATE EXTENSION test_prolly;

SELECT suite, test, result, detail FROM test_prolly_unit_tests();

-- one suite, by name
SELECT suite, test, result FROM test_prolly_unit_tests('harness');

-- running them again gives the same results: no test depends on running
-- once per session
SELECT count(*) AS failures FROM test_prolly_unit_tests() WHERE result <> 'ok';

SELECT * FROM test_prolly_unit_tests('no_such_suite');
