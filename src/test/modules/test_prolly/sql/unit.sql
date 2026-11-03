-- The unit tests of the engine's code in the server.  Each row is one test;
-- a failure shows its error in detail.
CREATE EXTENSION test_prolly;

-- this run loads the module on first use, not at preload; t/001_preloaded.pl
-- covers the preloaded case
SELECT test_prolly_loaded_at_preload();

SELECT suite, test, result, detail FROM test_prolly_unit_tests();

-- one suite, by name
SELECT suite, test, result FROM test_prolly_unit_tests('harness');

-- running them again gives the same results: no test depends on running
-- once per session
SELECT count(*) AS failures FROM test_prolly_unit_tests() WHERE result <> 'ok';

SELECT * FROM test_prolly_unit_tests('no_such_suite');

-- the engine's long operations check for interrupts: a build of many rows,
-- cancelled as it begins, cut short (not run to its end)
SELECT test_engine_build(1000, false), test_engine_build_finished();
SELECT test_engine_build(100000, true);
SELECT test_engine_build_finished();
