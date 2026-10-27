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
import window


class FakeVersionedSQL:
    """An engine run_cell() can drive, in memory: commit i holds the fixture's
    changed rows at revision i (the rest at 0), each branch a head and a
    working set.  It answers the statements run_cell() sends, so a case's
    validation and restore run against it; keep_dirty_on_hard_reset makes a
    hard reset that leaves the working set as it was, a bug to be caught."""
    def __init__(self, n, k, commits, branch, keep_dirty_on_hard_reset=False,
                 windowed=False, roots=None, in_window=None, write_rows_anyway=False):
        from experiment import changed_ids
        self.commits, self.changed = commits, changed_ids(n, k)
        self.branches = {branch: [len(commits) - 1, self.rows(len(commits) - 1)]}
        self.current, self.keep_dirty = branch, keep_dirty_on_hard_reset
        self.statements = []
        # A model of a history window (windowed): the commits with roots
        # (all by default) and those in the window (all by default).  A
        # hard reset gives back its commit's root, or the nearest before
        # it, or writes its rows, counting the path as pg_stat_prolly does;
        # write_rows_anyway models a move that writes rows with a root.
        self.windowed = windowed
        self.roots = set(commits if roots is None else roots)
        self.in_window = set(commits if in_window is None else in_window)
        self.write_rows_anyway = write_rows_anyway
        self.path = dict.fromkeys(window.PATH_COUNTERS, 0)
        self.targets_checked = []

    def target(self, chain, index, c):
        self.targets_checked.append(chain[index])
        if not self.windowed:
            return window.local_target()
        return window.classify(chain, index, self.in_window.__contains__, self.roots.__contains__,
                               c.get('history_window', {}).get('root_interval_commits', 1),
                               c.get('history_window', {}).get('allow_past', False))

    def path_counters(self):
        return dict(self.path) if self.windowed else None

    def at_sql(self, commit, ids):
        return 'AT ' + json.dumps([commit, list(ids)])

    def _move(self, i):
        if not self.windowed:
            return
        nearest = next((j for j in range(i, -1, -1) if self.commits[j] in self.roots), None)
        if nearest == i:
            self.path['moves_root_kept'] += 1
            if self.write_rows_anyway:
                self.path['move_tables_written'] += 1
        elif nearest is not None:
            self.path['moves_root_nearest'] += 1
            self.path['move_tables_written'] += 1
        else:
            self.path['moves_root_missing'] += 1
            self.path['move_tables_written'] += 1

    def rows(self, i):
        return {r: i for r in self.changed}

    def state(self, ref):
        head, work = self.branches[self.current]
        if ref == 'WORKING':
            return work
        if ref == 'HEAD':
            return self.rows(head)
        return self.rows(self.commits.index(ref))

    def vc_sql(self, op, *args):
        return 'VC ' + json.dumps([op, *args])

    def vc(self, op, *args):
        return self.query(self.vc_sql(op, *args))

    def hash_sql(self, ref='HEAD'):
        return 'HASH ' + ref

    def hash(self, ref='HEAD'):
        return self.scalar(self.hash_sql(ref))

    def diff_sql(self, a, b):
        return 'DIFF ' + json.dumps([a, b])

    def ids_in(self, ids):
        return 'id IN (' + ','.join(map(str, ids)) + ')'

    def table_count_sql(self):
        return 'TABLES bench_rows'

    def counters(self):
        # (work counted: every statement so far, and the UPDATEs apart)
        return {'statements': len(self.statements),
                'updates': sum(q.startswith('UPDATE') for q in self.statements)}

    def scalar(self, q):
        return self.query(q)['rows'][0][0]

    def query(self, q):
        import re
        self.statements.append(q)
        out = [['1']]
        if q.startswith('VC '):
            op, *args = json.loads(q[3:])
            if op == 'branch' and args[0] == '-D':
                assert args[1] != self.current, 'deleting the session branch'
                del self.branches[args[1]]
            elif op == 'branch':
                i = self.commits.index(args[1])
                self.branches[args[0]] = [i, self.rows(i)]
            elif op == 'checkout':
                assert args[0] in self.branches, args[0]
                self.current = args[0]
            elif op == 'reset':
                b = self.branches[self.current]
                b[0] = self.commits.index(args[1])
                if args[0] == '--hard':
                    self._move(b[0])
                if args[0] == '--hard' and (not self.keep_dirty or b[1] is None):
                    b[1] = self.rows(b[0])
        elif q.startswith('HASH '):
            ref = q[5:]
            head = self.branches[self.current][0]
            if ref in self.branches:
                head = self.branches[ref][0]
            elif ref.startswith('HEAD~'):
                head -= int(ref[5:])
            out = [[self.commits[head]]]
        elif q.startswith('AT '):
            commit, ids = json.loads(q[3:])
            rows = self.rows(self.commits.index(commit))
            out = [[str(i), str(rows.get(i, 0))] for i in ids]
        elif q.startswith('DIFF '):
            a, b = (self.state(r) for r in json.loads(q[5:]))
            out = [[str(sum(a.get(i, 0) != b.get(i, 0) for i in set(a) | set(b)))]]
        elif q == 'DROP TABLE bench_rows':
            assert self.branches[self.current][1] is not None, 'no table to drop'
            self.branches[self.current][1] = None
        elif q.startswith('SELECT count(*) FROM dolt_branches WHERE name='):
            out = [[str(int(q.split('=', 1)[1].strip("'") in self.branches))]]
        elif q == 'TABLES bench_rows':
            out = [[str(int(self.branches[self.current][1] is not None))]]
        elif q.startswith('UPDATE'):
            rev, ids = re.match(r'UPDATE bench_rows SET revision=(\d+) WHERE id IN \(([\d,]+)\)', q).groups()
            for i in ids.split(','):
                self.branches[self.current][1][int(i)] = int(rev)
        elif q.startswith('SELECT id,revision'):
            ids = re.search(r'id IN \(([\d,]+)\)', q).group(1).split(',')
            work = self.branches[self.current][1]
            out = [[i, str(work.get(int(i), 0))] for i in ids]
        return {'rows': out, 'elapsed_ns': 1000}


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

    def _run_cell(self, sql, n, k, commits, engine='doltgres', passes=('timing',), warmups=1,
                  history_window=None):
        import random
        from experiment import run_cell
        c = {'passes': list(passes), 'warmups': warmups, 'repetitions': 1, 'history_depths': [1, 3],
             'sample_interval_ms': 10, 'payload_bytes': 16,
             'dirty_sizes': {'small': {'rows': 3}, 'half': {'fraction': 0.5}}}
        if history_window is not None:
            c['history_window'] = history_window
        out = io.StringIO()
        run_cell(None, c, engine, n, k, sql, None, {'commits': commits, 'branch': 'fixture'},
                 out, random.Random(7))
        return [json.loads(line) for line in out.getvalue().splitlines()]

    def test_branch_delete_and_drop_table_cases(self):
        plan = cases({'history_depths': [1, 4], 'dirty_sizes': {'small': {'rows': 3}}})
        keys = {(case['operation'], case['distance'], case['working_set']) for case in plan}
        for d in (0, 1, 4):
            self.assertIn(('branch_delete', d, 'clean'), keys)
            self.assertIn(('branch_delete_used', d, 'clean'), keys)
        self.assertIn(('drop_table', 0, 'clean'), keys)
        self.assertIn(('drop_table', 0, 'small'), keys)
        self.assertFalse(any(op == 'drop_table' and d for op, d, _ in keys))
        n, k, commits = 64, 8, ['c%d' % i for i in range(5)]
        sql = FakeVersionedSQL(n, k, commits, 'fixture')
        records = self._run_cell(sql, n, k, commits, warmups=0)
        ran = {(r['operation'], r['distance'], r['working_set']) for r in records}
        for op in ('branch_delete', 'branch_delete_used'):
            for d in (0, 1, 3):
                self.assertIn((op, d, 'clean'), ran)
        for ws in ('clean', 'small', 'half'):
            self.assertIn(('drop_table', 0, ws), ran)
        self.assertTrue(all(r['status'] == 'ok' for r in records))
        timed = {r['sql'] for r in records if r['operation'] == 'drop_table'}
        self.assertEqual(timed, {'DROP TABLE bench_rows'})
        # (the used branch is checked out before the timed delete; the other never)
        checkouts = [q for q in sql.statements if q.startswith('VC ["checkout", "new_')]
        self.assertEqual(len(checkouts), 3 + 6)
        # every temporary branch is gone at the end, and the table is back
        self.assertEqual(set(sql.branches), {'fixture'})
        self.assertIsNotNone(sql.branches['fixture'][1])

    def test_counters_pass_counts_the_operation_alone(self):
        # The model counts every statement: the operation's deltas are the
        # operation's one statement, not the dirty rows' setup before it or
        # the validation and restoration after.
        n, k, commits = 64, 8, ['c%d' % i for i in range(5)]
        sql = FakeVersionedSQL(n, k, commits, 'fixture')
        records = self._run_cell(sql, n, k, commits, engine='prolly', passes=('timing', 'counters'))
        counted = [r for r in records if r['pass'] == 'counters']
        self.assertEqual(len(counted), len([r for r in records if r['pass'] == 'timing']))
        for r in counted:
            self.assertEqual(r['counters'], {'statements': 1}, r['operation'])
        self.assertTrue(any(r['dirty_rows'] for r in counted))
        self.assertTrue(all('counters' not in r for r in records if r['pass'] == 'timing'))

    def test_counters_pass_only_for_engines_with_counters(self):
        c = {'passes': ['timing', 'counters'], 'row_counts': [10, 20], 'changes': [1],
             'history_depths': [1], 'repetitions': 3, 'dirty_sizes': {}}
        self.assertEqual(core.engine_passes(c, 'prolly'), ['timing', 'counters'])
        self.assertEqual(core.engine_passes(c, 'dolt'), ['timing'])
        from experiment import planned_samples
        per_pass = 2 * 1 * len(cases(c)) * 3
        self.assertEqual(planned_samples(c, ['dolt', 'doltgres', 'prolly']), 4 * per_pass)
        # a Go engine run through the model never asks for counters
        n, k, commits = 64, 8, ['c%d' % i for i in range(5)]
        sql = FakeVersionedSQL(n, k, commits, 'fixture')
        sql.counters = None
        records = self._run_cell(sql, n, k, commits, engine='dolt', passes=('timing', 'counters'))
        self.assertEqual({r['pass'] for r in records}, {'timing'})

    def test_counter_deltas_refuse_resets(self):
        from experiment import counter_deltas
        self.assertEqual(counter_deltas({'a': 1, 'b': 5}, {'a': 1, 'b': 7}), {'b': 2})
        with self.assertRaisesRegex(RuntimeError, 'backwards'):
            counter_deltas({'a': 5}, {'a': 0})
        with self.assertRaisesRegex(RuntimeError, 'names'):
            counter_deltas({'a': 5}, {'b': 5})

    def test_trend_test(self):
        from report import trend
        import math
        s, tau, p = trend([1, 2, 3, 4, 5, 6, 7])
        self.assertEqual((s, tau), (21, 1))
        self.assertAlmostEqual(p, 2 / math.factorial(7))
        s, tau, p = trend([7, 6, 5, 4, 3, 2, 1])
        self.assertEqual((s, tau), (-21, -1))
        # one pair out of order: twice the 7 orders that close or closer
        self.assertAlmostEqual(trend([2, 1, 3, 4, 5, 6, 7])[2], 2 * 7 / math.factorial(7))
        self.assertIsNone(trend([4, 4, 4, 4, 4]))
        self.assertEqual(trend([3, 1, 4, 2, 5, 0, 6])[0], 5)
        # ties: the normal approximation, which still sees a steady rise
        s, tau, p = trend([1, 1, 2, 2, 3, 3, 4, 4, 5, 5])
        self.assertEqual(s, 40)
        self.assertLess(p, 1e-3)

    def _drift_rows(self, values, counters=None):
        rows = []
        for i, v in enumerate(values):
            r = dict(engine='prolly', operation='checkout', rows=1000, changed_rows=10,
                     history_commits=4, distance=1, working_set='clean', dirty_rows=0,
                     repetition=i, elapsed_ns=v, status='ok', warmup=False, **{'pass': 'timing'})
            rows.append(r)
            if counters is not None:
                rows.append(dict(r, **{'pass': 'counters'}, counters=counters[i]))
        return rows

    def test_drift_flags_series_that_follow_run_order(self):
        from report import drift
        fields = ["engine", "operation", "rows", "changed_rows", "history_commits",
                  "working_set", "dirty_rows", "distance", "pass"]
        rising = [10, 11, 13, 14, 18, 19, 25]
        log = [{'log_pages_read': 2 * i + 1, 'nodes_read': 4} for i in range(7)]
        flags = drift(self._drift_rows(rising, log), fields)
        self.assertEqual(sorted((f['pass'], f['metric']) for f in flags),
                         [('counters', 'counter_log_pages_read'), ('timing', 'elapsed_ns')])
        timing = next(f for f in flags if f['pass'] == 'timing')
        self.assertEqual((timing['direction'], timing['first'], timing['last']), ('rising', 10, 25))
        # the same samples out of order, and flat counters, drift not at all
        self.assertEqual(drift(self._drift_rows([14, 10, 25, 11, 19, 13, 18],
                                                [{'nodes_read': 4}] * 7), fields), [])
        # too few samples to tell
        self.assertEqual(drift(self._drift_rows(rising[:4]), fields), [])

    def test_report_shows_counters_and_drift(self):
        records = []
        for i, v in enumerate([1, 2, 3, 4, 5, 6, 7]):
            r = dict(engine='prolly', operation='diff_history', rows=1000, changed_rows=10, history_commits=4,
                     distance=1, working_set='clean', dirty_rows=0, repetition=i, elapsed_ns=v * 1_000_000,
                     status='ok', warmup=False, **{'pass': 'timing'})
            records += [r, dict(r, **{'pass': 'counters'},
                                counters={'nodes_read': 40, 'log_pages_read': 3 + i})]
        with tempfile.TemporaryDirectory() as d:
            path = Path(d)
            core.write_json(path/'manifest.json', {'status': 'complete', 'planned_samples': len(records),
                                                   'config': {'passes': ['timing', 'counters']}})
            (path/'samples.jsonl').write_text(''.join(json.dumps(r)+'\n' for r in records))
            with contextlib.redirect_stdout(io.StringIO()):
                report(path)
            rows = list(__import__('csv').DictReader(open(path/'summary.csv')))
            counted = next(r for r in rows if r['pass'] == 'counters')
            self.assertEqual(float(counted['median_counter_nodes_read']), 40)
            self.assertEqual(float(counted['median_counter_log_pages_read']), 6)
            flagged = json.loads((path/'drift.json').read_text())
            self.assertEqual({f['metric'] for f in flagged}, {'elapsed_ns', 'counter_log_pages_read'})
            for name in ('report.md', 'report.html'):
                text = (path/name).read_text()
                self.assertIn('Engine work counters', text)
                self.assertIn('Run-order drift', text)
                self.assertIn('4.00 ms [1.00–7.00]; n=7 [drift ↑]', text)
                self.assertIn('log_pages_read', text)
                self.assertIn('6 [3–9] [drift ↑]', text)
            self.assertIn('| Diff old → HEAD | 1 | nodes_read | — | — | 40 |', (path/'report.md').read_text())
            self.assertIn('2 series flagged', (path/'report.txt').read_text())

    def test_every_case_validates_and_restores_against_a_model_engine(self):
        n, k, commits = 64, 8, ['c%d' % i for i in range(5)]
        sql = FakeVersionedSQL(n, k, commits, 'fixture')
        records = self._run_cell(sql, n, k, commits)
        self.assertTrue(records and all(r['status'] == 'ok' for r in records))
        hard = [r for r in records if r['operation'] == 'reset_hard']
        # each depth (0, 1, 3) clean and with each dirty size, twice (warmup + 1)
        self.assertEqual(len(hard), 3 * 3 * 2)
        self.assertEqual(sum(r['sql'].startswith('VC ["reset", "--hard"') for r in hard), len(hard))
        # the fixture branch is left as it was, and only it
        self.assertEqual(list(sql.branches), ['fixture'])
        self.assertEqual(sql.current, 'fixture')

    def test_no_warmups_measures_each_case_once(self):
        n, k, commits = 64, 8, ['c%d' % i for i in range(5)]
        sql = FakeVersionedSQL(n, k, commits, 'fixture')
        records = self._run_cell(sql, n, k, commits, warmups=0)
        self.assertTrue(records and not any(r['warmup'] for r in records))
        self.assertEqual(len([r for r in records if r['operation'] == 'reset_hard']), 3 * 3)

    def test_a_hard_reset_that_keeps_dirty_rows_is_caught(self):
        n, k, commits = 64, 8, ['c%d' % i for i in range(5)]
        sql = FakeVersionedSQL(n, k, commits, 'fixture', keep_dirty_on_hard_reset=True)
        with self.assertRaises(RuntimeError) as e:
            self._run_cell(sql, n, k, commits)
        self.assertRegex(str(e.exception), 'working rows|hard reset leaves no working changes|preserved dirty')

    def test_window_settings_hold_every_commit_a_fixture_builds(self):
        from engines import ENGINES
        c = {'history_depths': [32, 100, 500]}
        got = window.settings(c)
        self.assertEqual(got['prolly.history_min_commits'], '501')
        self.assertEqual(got['prolly.history_window'], window.DEFAULT_TIME_WINDOW)
        self.assertEqual(got['prolly.root_interval_commits'], '1')
        self.assertEqual(got['prolly.root_interval'], '0')
        self.assertEqual(ENGINES['prolly'].window_settings(c), got)
        self.assertEqual(ENGINES['dolt'].window_settings(c), {})
        self.assertEqual(ENGINES['doltgres'].window_settings(c), {})
        with self.assertRaisesRegex(ValueError, 'fewer than the 501'):
            window.settings({**c, 'history_window': {'min_commits': 500}})
        with self.assertRaisesRegex(ValueError, 'at least 1'):
            window.settings({**c, 'history_window': {'root_interval_commits': 0}})

        class Admin:
            def __init__(self):
                self.sent = []

            def query(self, q):
                self.sent.append(q)
        admin = Admin()
        ENGINES['prolly'].apply_window(admin, 'bench_n10', c)
        self.assertIn("ALTER DATABASE bench_n10 SET prolly.history_min_commits = '501'", admin.sent)
        self.assertEqual(len(admin.sent), len(got))
        admin = Admin()
        ENGINES['dolt'].apply_window(admin, 'bench_n10', c)
        self.assertEqual(admin.sent, [])

    def test_targets_are_classified_by_window_and_root(self):
        chain = ['c0', 'c1', 'c2', 'c3', 'c4']
        everything = set(chain).__contains__
        self.assertEqual(window.classify(chain, 4, everything, everything, 1),
                         {'window': 'in', 'root': 'kept', 'root_distance': 0})
        roots = {'c0', 'c3'}.__contains__
        self.assertEqual(window.classify(chain, 2, everything, roots, 3),
                         {'window': 'in', 'root': 'nearest', 'root_distance': 2})
        # (the nearest root is past the interval: no root to measure from)
        with self.assertRaisesRegex(window.WindowError, 'keep a root'):
            window.classify(chain, 2, everything, roots, 2)
        with self.assertRaisesRegex(window.WindowError, 'keep a root'):
            window.classify(chain, 1, everything, {'c3'}.__contains__, 1)
        past = {'c3', 'c4'}.__contains__
        with self.assertRaisesRegex(window.WindowError, 'outside the history window'):
            window.classify(chain, 1, past, everything, 1)
        self.assertEqual(window.classify(chain, 1, past, everything, 1, allow_past=True),
                         {'window': 'past', 'root': None, 'root_distance': None})

    def test_path_checks_refuse_anything_but_the_root_path(self):
        kept = {'window': 'in', 'root': 'kept', 'root_distance': 0}
        nearest = {'window': 'in', 'root': 'nearest', 'root_distance': 1}
        window.check_path('reset_hard', 3, kept, {'moves_root_kept': 1})
        window.check_path('reset_hard', 3, nearest, {'moves_root_nearest': 1, 'move_tables_written': 1})
        window.check_path('diff_history', 3, kept, {})
        window.check_path('reset_hard', 0, kept, {})
        for op, d, target, deltas, why in [
                ('reset_hard', 3, kept, {'moves_root_kept': 1, 'move_tables_written': 1}, 'wrote rows'),
                ('checkout', 3, kept, {'move_tables_written': 2}, 'wrote rows'),
                ('reset_hard', 3, kept, {'moves_root_missing': 1, 'move_tables_written': 1}, 'without a root'),
                ('reset_hard', 3, kept, {}, 'moves_root_kept is 0'),
                ('reset_hard', 3, nearest, {'moves_root_kept': 1}, 'moves_root_nearest is 0'),
                ('reset_hard', 3, kept, {'moves_root_kept': 1, 'restores_lock_busy': 1}, 'for a lock'),
                ('at_history', 3, kept, {'severed_refusals': 1}, 'past the horizon'),
                ('diff_history', 3, kept, {'move_tables_written': 1}, 'wrote rows')]:
            with self.assertRaisesRegex(window.PathError, why):
                window.check_path(op, d, target, deltas)
        # (a target past the window is measured apart, held to nothing; an
        # engine without a window has no roots to check)
        window.check_path('reset_hard', 3, {'window': 'past', 'root': None, 'root_distance': None},
                          {'moves_root_missing': 1, 'move_tables_written': 1})
        window.check_path('reset_hard', 3, window.local_target(), {})

    def test_every_target_is_checked_and_every_path_recorded(self):
        n, k, commits = 64, 8, ['c%d' % i for i in range(5)]
        sql = FakeVersionedSQL(n, k, commits, 'fixture', windowed=True)
        records = self._run_cell(sql, n, k, commits, engine='prolly')
        self.assertTrue(records and all(r['status'] == 'ok' for r in records))
        # (one check before each operation, warmups too)
        self.assertEqual(len(sql.targets_checked), len(records))
        self.assertTrue(all(r['window'] == 'in' and r['target_root'] == 'kept' for r in records))
        self.assertTrue(all(set(r['path']) == set(window.PATH_COUNTERS) for r in records))
        hard = [r for r in records if r['operation'] == 'reset_hard' and r['distance']]
        self.assertTrue(hard and all(r['path']['moves_root_kept'] == 1 for r in hard))
        at = [r for r in records if r['operation'] == 'at_history']
        self.assertEqual({r['distance'] for r in at}, {0, 1, 3})
        # the counters pass takes the path from its own reads
        sql = FakeVersionedSQL(n, k, commits, 'fixture', windowed=True)
        sql.counters = lambda: dict(sql.path)
        records = self._run_cell(sql, n, k, commits, engine='prolly', passes=('counters',), warmups=0)
        hard = [r for r in records if r['operation'] == 'reset_hard' and r['distance']]
        self.assertTrue(hard and all(r['path']['moves_root_kept'] == 1 for r in hard))

    def test_a_move_that_writes_rows_fails_the_run(self):
        n, k, commits = 64, 8, ['c%d' % i for i in range(5)]
        sql = FakeVersionedSQL(n, k, commits, 'fixture', windowed=True, write_rows_anyway=True)
        with self.assertRaisesRegex(window.PathError, 'wrote rows'):
            self._run_cell(sql, n, k, commits, engine='prolly')

    def test_a_target_without_a_root_fails_before_it_is_measured(self):
        n, k, commits = 64, 8, ['c%d' % i for i in range(5)]
        sql = FakeVersionedSQL(n, k, commits, 'fixture', windowed=True, roots=set(commits) - {'c3'})
        with self.assertRaisesRegex(window.WindowError, 'keep a root'):
            self._run_cell(sql, n, k, commits, engine='prolly')
        # within the root interval, its nearest root (c2) is measured from
        sql = FakeVersionedSQL(n, k, commits, 'fixture', windowed=True, roots=set(commits) - {'c3'})
        records = self._run_cell(sql, n, k, commits, engine='prolly',
                                 history_window={'root_interval_commits': 2})
        near = [r for r in records if r['distance'] == 1]
        self.assertTrue(near and all(r['target_root'] == 'nearest' and r['root_distance'] == 1 for r in near))
        hard = [r for r in near if r['operation'] == 'reset_hard']
        self.assertTrue(all(r['path']['moves_root_nearest'] == 1 for r in hard))

    def test_targets_past_the_window_are_measured_apart(self):
        n, k, commits = 64, 8, ['c%d' % i for i in range(5)]
        sql = FakeVersionedSQL(n, k, commits, 'fixture', windowed=True, in_window=set(commits) - {'c1'})
        with self.assertRaisesRegex(window.WindowError, 'outside the history window'):
            self._run_cell(sql, n, k, commits, engine='prolly')
        sql = FakeVersionedSQL(n, k, commits, 'fixture', windowed=True, in_window=set(commits) - {'c1'},
                               roots=set(commits) - {'c1'})
        records = self._run_cell(sql, n, k, commits, engine='prolly', warmups=0,
                                 history_window={'allow_past': True})
        self.assertEqual({r['window'] for r in records if r['distance'] == 3}, {'past'})
        self.assertEqual({r['window'] for r in records if r['distance'] != 3}, {'in'})
        with tempfile.TemporaryDirectory() as d:
            path = Path(d)
            core.write_json(path/'manifest.json', {'status': 'complete', 'planned_samples': len(records)})
            (path/'samples.jsonl').write_text(''.join(json.dumps(r)+'\n' for r in records))
            with contextlib.redirect_stdout(io.StringIO()):
                report(path)
            rows = list(__import__('csv').DictReader((path/'summary.csv').open()))
            groups = {(r['operation'], r['distance'], r['window']) for r in rows}
            self.assertIn(('reset_hard', '3', 'past'), groups)
            self.assertNotIn(('reset_hard', '3', 'in'), groups)
            self.assertIn(('reset_hard', '1', 'in'), groups)
            kept = [r for r in rows if r['operation'] == 'reset_hard' and r['distance'] == '1']
            self.assertEqual(kept[0]['target_root'], 'kept')
            self.assertEqual(kept[0]['median_path_moves_root_kept'], '1')
            text = (path/'report.md').read_text()
            self.assertIn('Hard reset [past the history window]', text)
            self.assertIn('root kept; moves_root_kept 1', text)

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
                if pass_ == 'counters':
                    r['counters'] = {'nodes_read': 12}
                elif pass_ != 'timing':
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
                    self._run(root, 'c', ['cpu'], cells), self._run(root, 'd', ['io'], cells),
                    self._run(root, 'e', ['counters'], cells)]
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
            self.assertEqual(float(by_pass['counters']['median_counter_nodes_read']), 12)
            self.assertEqual(by_pass['timing']['median_counter_nodes_read'], '')
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
