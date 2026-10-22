import json
import contextlib
import io
import os
from pathlib import Path
import subprocess
import tempfile
import time
import unittest
import socket
import sys

import core
from experiment import changed_ids, integer, union_count, cases
from memory import Sampler
from report import fit, report
from human_report import render
import preflight


class BenchTests(unittest.TestCase):
    def test_dirty_sizes_validate_and_do_not_clamp(self):
        self.assertEqual(core.dirty_rows({'fraction': .0001}, 10000), 1)
        self.assertEqual(core.dirty_rows({'fraction': .01}, 128), 2)
        self.assertEqual(core.dirty_rows({'fraction': .1}, 9615384), 961539)
        self.assertEqual(core.dirty_rows({'rows': 17}, 128), 17)
        for spec in ({'rows': 129}, {'rows': 0}, {'rows': 1.5}, {'fraction': 0},
                     {'fraction': 1.1}, {'fraction': 'NaN'}, {'fraction': 'bad'},
                     {'rows': 2, 'fraction': .1}, {}):
            with self.assertRaises(ValueError):
                core.dirty_rows(spec, 128)

    def test_dirty_and_historical_union_handles_partial_overlap(self):
        for n in (1, 7, 16, 31):
            for a in range(1, n+1):
                for b in range(1, n+1):
                    self.assertEqual(union_count(n, a, b),
                                     len(set(changed_ids(n, a)) | set(changed_ids(n, b))))

    def test_case_plan_covers_each_dirty_size_operation_and_depth(self):
        plan = cases({'history_depths': [1, 4]})
        keys = {(case['operation'], case['distance'], case['working_set']) for case in plan}
        self.assertEqual(len(keys), len(plan))
        for size in ('clean', 'small', 'medium', 'large'):
            for op in ('branch', 'checkout', 'reset_soft', 'reset_hard'):
                for depth in (0, 1, 4):
                    self.assertIn((op, depth, size), keys)
        clean_only = cases({'history_depths': [1], 'dirty_sizes': {}})
        self.assertEqual({c['working_set'] for c in clean_only}, {'clean', 'diff'})

    def test_dirty_profiles_do_not_overwrite_clean_report_cells(self):
        summaries = []
        for profile, count, value in [('clean', 0, 1), ('small', 1, 2), ('large', 100, 3)]:
            summaries.append(dict(engine='dolt', operation='checkout', rows=1000, changed_rows=10,
                                  history_commits=4, distance=1, working_set=profile, dirty_rows=count,
                                  repetitions=7, median_ms=value, min_ms=value, max_ms=value,
                                  **{'pass': 'timing'}))
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)
            render(p, {'status': 'complete', 'planned_samples': 21}, summaries, [])
            for name in ('report.md', 'report.html'):
                text = (p/name).read_text()
                self.assertIn('clean working set', text)
                self.assertIn('small dirty working set: 1 rows', text)
                self.assertIn('large dirty working set: 100 rows', text)
                for value in ('1.00 ms', '2.00 ms', '3.00 ms'):
                    self.assertIn(value, text)

    def test_dirty_scaling_varies_count_without_fixing_profile_label(self):
        records = []
        for profile, count in [('tiny', 1), ('small', 10), ('medium', 100), ('large', 1000)]:
            for rep in range(5):
                records.append(dict(engine='dolt', operation='branch', rows=10000, changed_rows=10,
                                    history_commits=4, distance=0, working_set=profile, dirty_rows=count,
                                    repetition=rep, elapsed_ns=count*1000, status='ok', warmup=False,
                                    **{'pass': 'timing'}))
        with tempfile.TemporaryDirectory() as d:
            path = Path(d)
            core.write_json(path/'manifest.json', {'status': 'complete', 'planned_samples': len(records)})
            (path/'samples.jsonl').write_text(''.join(json.dumps(r)+'\n' for r in records))
            with contextlib.redirect_stdout(io.StringIO()):
                report(path)
            fits = json.loads((path/'scaling.json').read_text())
            dirty = [f for f in fits if f['axis'] == 'dirty_rows']
            self.assertEqual(len(dirty), 1)
            self.assertEqual(dirty[0]['status'], 'descriptive')
            self.assertAlmostEqual(dirty[0]['exponent'], 1)
            self.assertEqual(len([f for f in fits if f['axis'] == 'rows']), 4)

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

    def test_default_dirty_profiles_can_fit_both_axes(self):
        # Four table sizes, the default fraction profiles: a fit across the
        # profiles at each size, and one across sizes for each profile.
        records = []
        for n in (100000, 1000000, 10000000, 100000000):
            for profile, spec in core.default_dirty_sizes().items():
                q = core.dirty_rows(spec, n)
                for rep in range(5):
                    records.append(dict(engine='dolt', operation='branch', rows=n, changed_rows=10,
                                        history_commits=4, distance=0, working_set=profile, dirty_rows=q,
                                        repetition=rep, elapsed_ns=q*1000, status='ok', warmup=False,
                                        **{'pass': 'timing'}))
        with tempfile.TemporaryDirectory() as d:
            path = Path(d)
            core.write_json(path/'manifest.json', {'status': 'complete', 'planned_samples': len(records)})
            (path/'samples.jsonl').write_text(''.join(json.dumps(r)+'\n' for r in records))
            with contextlib.redirect_stdout(io.StringIO()):
                report(path)
            fits = [f for f in json.loads((path/'scaling.json').read_text()) if f['metric'] == 'elapsed_ns']
            dirty = [f for f in fits if f['axis'] == 'dirty_rows']
            self.assertEqual(len(dirty), 4)
            self.assertTrue(all(f['status'] == 'descriptive' for f in dirty))
            sizes = [f for f in fits if f['axis'] == 'rows']
            self.assertEqual(sorted(f['working_set'] for f in sizes), sorted(core.default_dirty_sizes()))
            for f in sizes:
                self.assertEqual(f['status'], 'descriptive')
                self.assertAlmostEqual(f['exponent'], 1, places=2)

    def test_short_cpu_observations_are_flagged(self):
        cells = dict(cpu_seconds_delta=0.01, major_faults_delta=0, minor_faults_delta=1)
        with tempfile.TemporaryDirectory() as d:
            path = self._run(Path(d), 'a', ['cpu'], cells)
            with contextlib.redirect_stdout(io.StringIO()):
                report(path)
            rows = list(__import__('csv').DictReader(open(path/'summary.csv')))
            self.assertEqual(rows[0]['short_cpu_observations'], '2')
            self.assertIn('[short 2/2]', (path/'report.md').read_text())

    def test_id_sets_doltgres_plans_in_linear_time(self):
        ids = [1, 962, 1923]
        for name in ('dolt', 'prolly'):
            self.assertEqual(core.ENGINES[name].ids_in(ids), 'id IN (1,962,1923)')
        self.assertEqual(core.ENGINES['doltgres'].ids_in(ids),
                         'id IN (SELECT id FROM (VALUES (1),(962),(1923)) v(id))')

    def test_go_engines_both_disable_auto_gc(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            core.init(root, True)
            c = core.config(root)
            for engine in ('dolt', 'doltgres'):
                cfg = core.ENGINES[engine].server_config(root, c)
                self.assertIn('behavior:\n  auto_gc_behavior:\n    enable: false\n', cfg, engine)
                self.assertEqual(cfg.count('\nbehavior:'), 1, engine)

    def test_passes_validate(self):
        for ok in (['timing'], ['memory'], ['cpu', 'io'], list(core.PASSES)):
            core.check_passes(ok)
        for bad in ([], ['timing', 'timing'], ['memory', 'disk']):
            with self.assertRaises(ValueError):
                core.check_passes(bad)

    def _run(self, root, name, passes, cells, sha='a'):
        """A complete run directory of the given passes: one cell, two reps."""
        path = root/name
        path.mkdir()
        records = []
        for pass_ in passes:
            for rep in range(2):
                r = dict(engine='dolt', operation='branch', rows=1000, changed_rows=10, history_commits=4,
                         distance=0, working_set='clean', dirty_rows=0, repetition=rep, elapsed_ns=1_000_000,
                         status='ok', warmup=False, **{'pass': pass_})
                if pass_ != 'timing':
                    r['memory'] = dict(cells, short_operation=pass_ != 'memory')
                records.append(r)
        core.write_json(path/'manifest.json', {
            'status': 'complete', 'planned_samples': len(records),
            'config': {'rows': [1000], 'passes': passes, 'payload_bytes': 16},
            'binaries': {'dolt': {'sha256': sha}}})
        (path/'samples.jsonl').write_text(''.join(json.dumps(r)+'\n' for r in records))
        return path

    def test_separate_pass_runs_merge_into_one_report(self):
        cells = dict(pss_bytes_baseline=1, pss_bytes_peak=2, pss_bytes_peak_delta=1, pss_bytes_retained_delta=0,
                     rss_bytes_peak=3, private_bytes_peak_delta=0, cpu_seconds_delta=0.25,
                     major_faults_delta=0, minor_faults_delta=7, read_bytes_delta=4096, write_bytes_delta=8192)
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            runs = [self._run(root, 'a', ['timing'], cells), self._run(root, 'b', ['memory'], cells),
                    self._run(root, 'c', ['cpu'], cells), self._run(root, 'd', ['io'], cells)]
            from report import merge
            out = merge(runs, root/'results')
            with contextlib.redirect_stdout(io.StringIO()):
                report(out)
            manifest = json.loads((out/'manifest.json').read_text())
            self.assertEqual(manifest['config']['passes'], list(core.PASSES))
            rows = list(__import__('csv').DictReader(open(out/'summary.csv')))
            by_pass = {r['pass']: r for r in rows}
            self.assertEqual(set(by_pass), set(core.PASSES))
            # each pass reports its own counters, not the others'
            self.assertEqual(by_pass['cpu']['median_cpu_seconds_delta'], '0.25')
            self.assertEqual(by_pass['cpu']['median_read_bytes_delta'], '')
            self.assertEqual(float(by_pass['io']['median_write_bytes_delta']), 8192)
            self.assertEqual(by_pass['memory']['median_cpu_seconds_delta'], '')
            text = (out/'report.md').read_text()
            self.assertIn('CPU time | 250.00 ms', text)
            self.assertIn('Write bytes | 8.00 KiB', text)

    def test_merge_refuses_mismatched_runs(self):
        cells = dict(cpu_seconds_delta=0, major_faults_delta=0, minor_faults_delta=0)
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            from report import merge
            a = self._run(root, 'a', ['timing'], cells)
            twice = self._run(root, 'b', ['timing'], cells)
            with self.assertRaisesRegex(ValueError, 'more than once'):
                merge([a, twice], root/'results')
            other = self._run(root, 'c', ['cpu'], cells, sha='b')
            with self.assertRaisesRegex(ValueError, 'binary differs'):
                merge([a, other], root/'results')

    @unittest.skipUnless(Path('/proc/self/smaps_rollup').exists(), 'Linux procfs required')
    def test_unsampled_counters_take_two_snapshots(self):
        sampler = Sampler(os.getpid(), None)
        sampler.start()
        sum(range(2_000_000))
        result = sampler.finish(50_000_000)
        self.assertEqual(result['periodic_samples'], 0)
        self.assertEqual(len(result['samples']), 2)
        self.assertIsNotNone(result['cpu_seconds_delta'])

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
