import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
import unittest
import socket
import sys

import core
from experiment import changed_ids, integer
from memory import Sampler
from report import fit, report
from human_report import render
import preflight


class BenchTests(unittest.TestCase):
    def test_preflight_fails_before_starting_an_earlier_valid_engine(self):
        # First engine and driver are runnable. The second engine is absent.
        # An actual CLI invocation must discover that before starting the first.
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            core.init(root, True)
            (root/'bin').mkdir()
            driver = root/'bin/sql-driver'
            driver.write_text('#!/bin/sh\necho "usage: sql-driver DRIVER DSN"\nexit 2\n')
            driver.chmod(0o755)
            binary = root/'bin/dolt'
            binary.write_text('#!/bin/sh\nif [ "$1" = version ]; then exit 0; fi\n'
                              'echo "server started" > server-was-started\nexit 1\n')
            binary.chmod(0o755)
            p = subprocess.run([sys.executable, str(core.HERE/'vc_bench.py'), '--root', str(root),
                                'prepare', '--engines', 'dolt,doltgres'],
                               capture_output=True, text=True)
            self.assertEqual(p.returncode, 1)
            self.assertIn('Preflight failed before starting work', p.stderr)
            self.assertIn('doltgres installed binaries/libraries', p.stderr)
            self.assertFalse((root/'dolt').exists())
            self.assertFalse((root/'doltgres').exists())

    def test_preflight_checks_all_missing_binaries(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            core.init(root, True)
            with self.assertRaises(RuntimeError) as result:
                preflight.check(root, core.config(root), core.ENGINES, 'prepare')
            message = str(result.exception)
            for name in ('SQL driver', 'dolt installed', 'doltgres installed', 'prolly installed'):
                self.assertIn(name, message)

    def test_preflight_rejects_an_unrelated_listener(self):
        with tempfile.TemporaryDirectory() as d, socket.socket() as listener:
            listener.bind(('127.0.0.1', 0))
            listener.listen()
            with self.assertRaises(OSError):
                preflight.available_port(Path(d), 'dolt', listener.getsockname()[1])

    def test_human_report_alignment_units_and_cautions(self):
        common = dict(operation='branch', rows=10000, changed_rows=10,
                      history_commits=32, distance=4, repetitions=7)
        summaries = [
            dict(common, engine='prolly', **{'pass': 'timing'}, median_ms=3, min_ms=2, max_ms=4),
            dict(common, engine='dolt', **{'pass': 'timing'}, median_ms=1, min_ms=.5, max_ms=2),
            dict(common, engine='dolt', **{'pass': 'memory'}, median_ms=999999,
                 short_memory_observations=2, median_pss_bytes_peak_delta=2048,
                 median_pss_bytes_retained_delta=-1024, median_pss_bytes_baseline=1048576,
                 median_pss_bytes_peak=2097152, median_cpu_seconds_delta=.125),
        ]
        fits = [dict(engine='dolt', axis='rows', metric='elapsed_ns', operation='branch',
                     changed_rows=10, history_commits=32, distance=4, status='descriptive',
                     exponent=.25, bootstrap_95_percent=[.1, .4], r_squared_log_space=.9)]
        manifest = dict(status='complete', planned_samples=21, config={'payload_bytes': 1024},
                        binaries={'dolt': {'build': {'source': {'commit': '<script>|bad'}}}})
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)
            render(p, manifest, summaries, fits)
            md = (p/'report.md').read_text()
            html = (p/'report.html').read_text()
            # Input order differs from the required engine-column order, and a
            # missing engine must stay blank rather than shift the Prolly value.
            self.assertIn('| Create branch | 4 | 1.00 ms [0.500–2.00]; n=7 | — | 3.00 ms [2.00–4.00]; n=7 |', md)
            for output in (md, html):
                self.assertIn('2.00 KiB / -1.00 KiB [short 2/7]', output)
                self.assertIn('1.00 MiB / 2.00 MiB [short 2/7]', output)
                self.assertIn('125.00 ms', output)
                self.assertIn('0.250 [0.100, 0.400]; R² 0.900', output)
                self.assertNotIn('999999', output)  # Instrumented latency excluded.
                self.assertNotIn('<script>', output)
            self.assertIn('&lt;script&gt;&#124;bad', md)
            self.assertIn('&lt;script&gt;|bad', html)

    def test_integer_aggregate_decoding(self):
        self.assertEqual(integer('1.048576e+06'), 1048576)
        self.assertEqual(integer('9999999360'), 9999999360)
        for value in ('1.5', 'NaN', 'Infinity'):
            with self.assertRaises(ValueError):
                integer(value)

    def test_change_cardinality_and_distribution(self):
        for n, k in [(1, 1), (128, 8), (129, 128), (9615384, 1000)]:
            ids = changed_ids(n, k)
            self.assertEqual(len(set(ids)), k)
            self.assertEqual(ids[0], 1)
            self.assertLessEqual(ids[-1], n)
            self.assertTrue(all(b-a >= 1 for a, b in zip(ids, ids[1:])))

    def test_scaling_controls(self):
        levels = (100, 1000, 10000, 100000)
        for exponent in (0, .5, 1, 2):
            result = fit({x: [3*x**exponent]*7 for x in levels})
            self.assertEqual(result['status'], 'descriptive')
            self.assertAlmostEqual(result['exponent'], exponent)
            for end in result['bootstrap_95_percent']:
                self.assertAlmostEqual(end, exponent)
        self.assertEqual(fit({x: [x]*2 for x in levels})['status'], 'inconclusive')
        self.assertEqual(fit({x: [0]*7 for x in levels})['status'], 'inconclusive')

    def test_refuses_incomplete_comparisons(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)
            (p/'manifest.json').write_text(json.dumps({'status': 'failed', 'planned_samples': 2}))
            (p/'samples.jsonl').write_text('')
            with self.assertRaisesRegex(ValueError, 'incomplete'):
                report(p)

    def test_config_does_not_clamp_impossible_changes(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            core.init(root, True)
            c = json.loads((root/'config.json').read_text())
            c['changes'] = [129]
            core.write_json(root/'config.json', c)
            with self.assertRaisesRegex(ValueError, 'smallest'):
                core.config(root)

    @unittest.skipUnless(Path('/proc/self/smaps_rollup').exists(), 'Linux procfs required')
    def test_memory_sampling_catches_child_allocation(self):
        # The child faults in and holds 32 MiB. This tests the real OS collector,
        # including descendant discovery, rather than a mocked /proc format.
        p = subprocess.Popen(['python3', '-u', '-c',
                              "import sys,time; print('ready'); sys.stdin.readline(); "
                              "b=bytearray(32*1024*1024); print('allocated'); time.sleep(5)"],
                             stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        try:
            self.assertEqual(p.stdout.readline().strip(), 'ready')
            sampler = Sampler(os.getpid(), 5)
            sampler.start()
            p.stdin.write('\n')
            p.stdin.flush()
            self.assertEqual(p.stdout.readline().strip(), 'allocated')
            time.sleep(.06)
            result = sampler.finish(80_000_000)
            self.assertGreater(result['pss_bytes_peak_delta'], 25*1024*1024)
            self.assertFalse(result['short_operation'])
            self.assertIn(p.pid, result['samples'][-1]['pids'])
        finally:
            p.terminate()
            p.wait()
            p.stdin.close()
            p.stdout.close()


if __name__ == '__main__':
    unittest.main()
