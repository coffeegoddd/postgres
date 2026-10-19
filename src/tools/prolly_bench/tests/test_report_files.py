import contextlib
import io
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from pbench import presentation, report_files, reports
from pbench.__main__ import main
from pbench.core.interfaces import CellKey, CellResult, RunInfo, Status
from pbench.core.runner import run_many
from pbench.core.validation import validate_run
from pbench.sinks.files import FilesSink


class FakeRunner:
    def __init__(self, root, label='B0', am='heap'):
        self.out_dir = root / label
        self.suite, self.label, self.table_am, self.build, self.builds = 'S', label, am, '', ()
        self.cfg = {'run': {'seed': 1}}
        self.sinks = [FilesSink()]

    def _cells(self):
        for rep in (1, 2):
            yield CellKey('S', 'S1', 'memory', '', 1, rep), {}

    def _open(self, lock):
        (self.out_dir / "raw").mkdir(parents=True)
        self.info = RunInfo(suite='S', label=self.label, table_am=self.table_am, started='2026-10-19',
                            command='test benchmark', config=self.cfg, environment={}, warnings=[],
                            out_dir=self.out_dir, planned_cells=[key for key, _ in self._cells()])
        for sink in self.sinks:
            sink.open(self.info)
        return list(self._cells())

    def _run_cell(self, key):
        return CellResult(key, Status.OK, metrics={'tps': 123, 'counter': 5}, unit='transaction',
                          denominator=1, measurement={'seconds': 1, 'quality': 'smoke', 'seed': key.rep})


class ReportFilesTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.output = io.StringIO()
        self.redirect = contextlib.redirect_stdout(self.output)
        self.redirect.__enter__()
        self.addCleanup(self.redirect.__exit__, None, None, None)

    def test_run_persists_both_formats_and_exact_invocation(self):
        runners = [FakeRunner(self.root), FakeRunner(self.root, 'prolly', 'prolly')]
        run_many(runners)
        for runner in runners:
            self.assertEqual(validate_run(runner.out_dir), [])
            self.assertTrue((runner.out_dir / 'index.html').exists())
            self.assertTrue((runner.out_dir / 'README.md').exists())
        self.assertTrue((self.root / 'index.html').exists())
        manifest = next(self.root.glob('reports/*/report.json'))
        data = json.loads(manifest.read_text())
        self.assertEqual(data['execution'], 'complete')
        self.assertEqual(data['rendering'], 'complete')
        self.assertEqual(len(data['inputs']), 2)
        self.assertTrue(all('results.jsonl' in e['hashes'] for e in data['inputs']))
        before = (manifest.parent / 'index.html').read_bytes()
        self.assertEqual(main(['report', '--manifest', str(manifest)]), 0)
        self.assertEqual((manifest.parent / 'index.html').read_bytes(), before)

    def test_report_files_do_not_change_pre_run_environment_and_locks_cover_rendering(self):
        runner = FakeRunner(self.root)
        open_run = runner._open
        def recording_environment(lock):
            self.assertFalse((self.root / 'reports').exists())
            return open_run(lock)
        runner._host_lock = io.StringIO()
        render = report_files.invocation
        def rendering(*args, **kwargs):
            self.assertFalse(runner._host_lock.closed)
            return render(*args, **kwargs)
        with patch.object(runner, '_open', side_effect=recording_environment), \
                patch.object(report_files, 'invocation', side_effect=rendering):
            run_many([runner])
        self.assertTrue(runner._host_lock.closed)

    def test_render_failure_happens_after_every_evidence_set_is_finalized(self):
        runners = [FakeRunner(self.root), FakeRunner(self.root, 'prolly', 'prolly')]
        with patch.object(report_files, 'result_set', side_effect=RuntimeError('renderer failed')) as render:
            with self.assertRaises(RuntimeError):
                run_many(runners)
        self.assertEqual(render.call_count, 2)
        for runner in runners:
            self.assertEqual(validate_run(runner.out_dir), [])
        manifest = json.loads(next(self.root.glob('reports/*/report.json')).read_text())
        self.assertEqual(manifest['execution'], 'complete')
        self.assertEqual(manifest['rendering'], 'failed')

    def test_interruption_shows_original_plan_and_missing_cells(self):
        runner = FakeRunner(self.root)
        with patch.object(runner, '_run_cell', side_effect=KeyboardInterrupt):
            with self.assertRaises(KeyboardInterrupt):
                run_many([runner])
        manifest = next(self.root.glob('reports/*/report.json'))
        data = json.loads(manifest.read_text())
        self.assertEqual(data['execution'], 'interrupted')
        self.assertFalse(data['passed'])
        self.assertEqual(json.loads((runner.out_dir / 'run.json').read_text())['state'], 'running')
        text = (manifest.parent / 'README.md').read_text()
        self.assertIn('0/2 planned', text)
        self.assertIn('missing', text)

    def test_opt_out_and_format_selection(self):
        runner = FakeRunner(self.root)
        run_many([runner], report_format='none')
        self.assertFalse((self.root / 'reports').exists())
        self.assertFalse((runner.out_dir / 'README.md').exists())
        self.assertEqual(main(['report', str(runner.out_dir), '--format', 'html']), 0)
        self.assertTrue((runner.out_dir / 'index.html').exists())
        self.assertFalse((runner.out_dir / 'README.md').exists())
        self.assertEqual(main(['report', str(runner.out_dir), '--format', 'markdown']), 0)
        self.assertTrue((runner.out_dir / 'README.md').exists())

    def test_catalog_keeps_malformed_and_newer_incomplete_runs(self):
        runner = FakeRunner(self.root)
        run_many([runner])
        bad = self.root / 'newer-error'
        bad.mkdir()
        (bad / 'run.json').write_text('{')
        report_files.catalog(self.root)
        html = (self.root / 'index.html').read_text()
        self.assertIn('newer-error', html)
        self.assertIn('invalid result evidence', html)
        self.assertIn('B0', html)

    def test_cli_table_compare_and_input_errors(self):
        runners = [FakeRunner(self.root), FakeRunner(self.root, 'prolly', 'prolly')]
        run_many(runners)
        inputs = [str(r.out_dir) for r in runners]
        stem = str(self.root / 'custom')
        self.assertEqual(main(['table', *inputs, '--output', stem]), 0)
        self.assertTrue(self.root.joinpath('custom.html').exists())
        self.assertEqual(main(['compare', *inputs, '--format', 'both', '--output', stem]), 0)
        self.assertEqual(main(['report', *inputs, '--output', stem]), 0)
        self.assertEqual(main(['report', '--catalog', str(self.root)]), 0)
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(main(['report', *inputs]), 1)
            self.assertEqual(main(['compare', *inputs, '--format', 'html']), 1)
            self.assertEqual(main(['report', inputs[0], '--catalog', str(self.root)]), 1)

    def test_manifest_detects_replaced_source_evidence(self):
        runner = FakeRunner(self.root)
        run_many([runner])
        manifest = next(self.root.glob('reports/*/report.json'))
        data = json.loads((runner.out_dir / 'environment.json').read_text())
        data['hostname'] = 'changed'
        (runner.out_dir / 'environment.json').write_text(json.dumps(data))
        paths, passed = report_files.invocation(manifest)
        self.assertFalse(passed)
        self.assertIn('Source evidence changed', (manifest.parent / 'README.md').read_text())

    def test_c_table_without_heap_cannot_establish_a_comparison(self):
        from evidence_helpers import c_result
        path = self.root / 'prolly'
        c_result(path, 'prolly', {'C1': [{'file': 'select1.test', 'line': 1, 'status': 'ok'}]})
        self.assertEqual(main(['report', str(path)]), 0)
        self.assertEqual(main(['table', str(path), '--output', str(self.root / 'comparison')]), 1)
        self.assertIn('Comparison unavailable', (self.root / 'comparison.html').read_text())
