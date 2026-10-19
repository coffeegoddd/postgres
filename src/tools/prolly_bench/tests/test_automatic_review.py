"""Failure and scalability regressions for automatic benchmark reports."""
import contextlib
import fcntl
import io
import json
import os
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from evidence_helpers import c_result
from pbench import ccompare, presentation, report_files, reports, table
from pbench.__main__ import main
from pbench.core import runner as runner_mod, validation
from pbench.core.runner import Runner, run_many
from test_report_files import FakeRunner


class AutomaticReviewTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.stdout = contextlib.redirect_stdout(io.StringIO())
        self.stderr = contextlib.redirect_stderr(io.StringIO())
        self.output = self.stdout.__enter__()
        self.stderr.__enter__()
        self.addCleanup(self.stdout.__exit__, None, None, None)
        self.addCleanup(self.stderr.__exit__, None, None, None)

    def test_host_lock_rejection_does_not_write_reports_or_catalog(self):
        active = self.root / 'index.html'
        active.write_text('active benchmark catalog')
        fixture = FakeRunner(self.root)
        runner = Runner.__new__(Runner)
        runner.__dict__.update(fixture.__dict__)
        runner.work_dir = self.root / 'work'
        runner.work_dir.mkdir()
        runner._cells = fixture._cells
        lock_path = self.root / f'prolly-bench-{os.getuid()}.lock'
        with lock_path.open('w') as held:
            fcntl.flock(held, fcntl.LOCK_EX | fcntl.LOCK_NB)
            with patch.object(runner_mod.tempfile, 'gettempdir', return_value=str(self.root)):
                with self.assertRaisesRegex(SystemExit, 'another prolly_bench run'):
                    run_many([runner])
        self.assertEqual(active.read_text(), 'active benchmark catalog')
        self.assertFalse((self.root / 'reports').exists())
        self.assertFalse(runner.out_dir.exists())

    def test_render_failure_keeps_results_and_updates_catalog(self):
        runners = [FakeRunner(self.root), FakeRunner(self.root, 'prolly', 'prolly')]
        with patch.object(report_files, 'result_set', side_effect=RuntimeError('renderer failed')):
            results = run_many(runners)
        self.assertEqual([len(cells) for cells in results], [2, 2])
        self.assertTrue(results.report_errors)
        self.assertTrue(all(not validation.validate_run(r.out_dir) for r in runners))
        self.assertIn('B0', (self.root / 'index.html').read_text())
        manifest = json.loads(next(self.root.glob('reports/*/report.json')).read_text())
        self.assertEqual(manifest['rendering'], 'failed')

    def test_cli_runs_compatibility_checks_even_if_reports_fail(self):
        runners = [FakeRunner(self.root), FakeRunner(self.root, 'prolly', 'prolly')]
        with patch.object(runner_mod, 'Runner', side_effect=runners), \
                patch.object(report_files, 'result_set', side_effect=RuntimeError('renderer failed')), \
                patch.object(validation, 'compatible', wraps=validation.compatible) as compatible:
            status = main(['run', 'S', '--am', 'heap,prolly', '--no-fio', '--out', str(self.root)])
        self.assertEqual(status, 1)
        compatible.assert_called_once_with(runners[0].out_dir, runners[1].out_dir, 'engine')
        self.assertTrue((self.root / 'index.html').exists())

    def test_unexpected_render_errors_are_recorded_as_failed(self):
        runner = FakeRunner(self.root)
        run_many([runner], report_format='none')
        for failure in (TypeError('bad metric'), KeyError('bad key')):
            with self.subTest(failure=type(failure).__name__):
                path = report_files.begin([runner], 'both')
                with patch.object(reports, 'build', side_effect=failure):
                    with self.assertRaises(Exception):
                        report_files.invocation(path, execution='complete')
                manifest = json.loads(path.read_text())
                self.assertEqual(manifest['rendering'], 'failed')
                self.assertFalse(manifest.get('passed'))
                self.assertIn(str(failure), ' '.join(manifest['errors']))

    def test_render_error_does_not_replace_keyboard_interrupt(self):
        runner = FakeRunner(self.root)
        with patch.object(runner, '_run_cell', side_effect=KeyboardInterrupt), \
                patch.object(reports, 'build', side_effect=TypeError('bad render')):
            with self.assertRaises(KeyboardInterrupt):
                run_many([runner])
        manifest = json.loads(next(self.root.glob('reports/*/report.json')).read_text())
        self.assertEqual(manifest['execution'], 'interrupted')
        self.assertEqual(manifest['rendering'], 'failed')
        self.assertTrue((self.root / 'index.html').exists())

    def test_interrupted_sink_records_terminal_state(self):
        runner = FakeRunner(self.root)
        with patch.object(runner, '_run_cell', side_effect=KeyboardInterrupt):
            with self.assertRaises(KeyboardInterrupt):
                run_many([runner])
        run = json.loads((runner.out_dir / 'run.json').read_text())
        self.assertEqual(run['state'], 'interrupted')
        self.assertIn('KeyboardInterrupt', run['error'])
        self.assertTrue(validation.validate_run(runner.out_dir))

    def test_preflight_failure_is_not_an_interruption(self):
        runner = FakeRunner(self.root)
        def fail_preflight(lock):
            runner._host_locked = True
            (runner.out_dir / 'raw').mkdir(parents=True)
            raise RuntimeError('benchmark conditions failed')
        with patch.object(runner, '_open', side_effect=fail_preflight):
            with self.assertRaisesRegex(RuntimeError, 'benchmark conditions failed'):
                run_many([runner])
        manifest = json.loads(next(self.root.glob('reports/*/report.json')).read_text())
        self.assertEqual(manifest['execution'], 'failed')
        self.assertIn('benchmark conditions failed', manifest['execution_error'])

    def test_finalization_failure_preserves_completed_sets_and_marks_failure(self):
        runners = [FakeRunner(self.root), FakeRunner(self.root, 'prolly', 'prolly')]
        with patch.object(runners[1].sinks[0], 'close', side_effect=OSError('cannot finalize')):
            with self.assertRaisesRegex(RuntimeError, 'evidence finalization failed'):
                run_many(runners)
        self.assertEqual(validation.validate_run(runners[0].out_dir), [])
        run = json.loads((runners[1].out_dir / 'run.json').read_text())
        self.assertEqual(run['state'], 'failed')
        manifest = json.loads(next(self.root.glob('reports/*/report.json')).read_text())
        self.assertEqual(manifest['execution'], 'failed')

    def test_catalog_does_not_revalidate_historical_raw_evidence(self):
        runner = FakeRunner(self.root)
        run_many([runner])
        with patch.object(reports, 'load', side_effect=AssertionError('catalog loaded full evidence')):
            report_files.catalog(self.root)
        text = (self.root / 'index.html').read_text()
        self.assertIn('B0', text)
        self.assertIn('not rechecked', text)

    def test_empty_fingerprint_is_an_immutable_snapshot(self):
        runner = FakeRunner(self.root)
        manifest = report_files.begin([runner], 'both')
        report_files.invocation(manifest, execution='interrupted')
        first = json.loads(manifest.read_text())
        self.assertEqual(first['inputs'][0]['hashes'], {})
        run_many([runner], report_format='none')
        _, passed = report_files.invocation(manifest)
        self.assertFalse(passed)
        second = json.loads(manifest.read_text())
        self.assertEqual(second['inputs'][0]['hashes'], {})
        self.assertIn('Source evidence changed', (manifest.parent / 'README.md').read_text())

    def setup_latest_runs(self):
        for name, date in [('old', '2026-10-18'), ('new', '2026-10-19')]:
            runner = FakeRunner(self.root, name)
            runner.label = 'B0'
            run_many([runner], report_format='none')
            path = runner.out_dir / 'run.json'
            run = json.loads(path.read_text())
            run['started'] = date
            path.write_text(json.dumps(run))
        for suite in ('C', 'V'):
            path = self.root / suite
            path.mkdir()
            (path / 'run.json').write_text(json.dumps({'suite': suite, 'label': suite, 'started': '2026-10-19'}))
        return self.root / 'new'

    def test_discovery_validates_only_selected_runs(self):
        newest = self.setup_latest_runs()
        with patch.object(table, 'load_set', wraps=table.load_set) as load:
            found = table.discover(self.root, 'S')
        self.assertEqual([s.path for s in found], [newest])
        self.assertEqual([call.args[0] for call in load.call_args_list], [newest])

    def test_table_cli_loads_only_latest_runs_of_requested_suite(self):
        newest = self.setup_latest_runs()
        load = reports.load
        def selected_only(path):
            self.assertEqual(path, newest)
            return load(path)
        with patch.object(reports, 'load', side_effect=selected_only):
            self.assertEqual(main(['table', '--suite', 'S', '--results', str(self.root)]), 0)

    def test_table_prints_the_markdown_it_writes_without_rendering_twice(self):
        runner = FakeRunner(self.root)
        run_many([runner], report_format='none')
        render = presentation.MarkdownRenderer.render
        stdout = io.StringIO()
        with patch.object(presentation.MarkdownRenderer, 'render', autospec=True, side_effect=render) as rendered, \
                contextlib.redirect_stdout(stdout):
            status = main(['table', str(runner.out_dir), '--output', str(self.root / 'table')])
        self.assertEqual(status, 0)
        self.assertIn((self.root / 'table.md').read_text(), stdout.getvalue())
        self.assertEqual(rendered.call_count, 1)

    def test_c_cli_still_prints_comparison_and_runs_gate_after_report_failure(self):
        sets = [c_result(self.root / am, am, {'C1': [
            {'file': 'select1.test', 'line': 1, 'status': 'ok'}]}) for am in ('heap', 'prolly')]
        runners = [SimpleNamespace(out_dir=s.path, build='', table_am=s.table_am) for s in sets]
        results = runner_mod.RunResults(2)
        results[:] = [s.results for s in sets]
        results.report_errors.append('renderer failed')
        with patch.object(runner_mod, 'Runner', side_effect=runners), \
                patch.object(runner_mod, 'run_many', return_value=results), \
                patch.object(ccompare, 'compare', return_value={'C1': {'counts': {'agree': 1},
                                                                   'stale_exceptions': [], 'gate_failures': 0}}), \
                patch.object(ccompare, 'gate', wraps=ccompare.gate) as gate:
            status = main(['run', 'C', '--am', 'heap,prolly', '--workloads', 'C1', '--no-fio'])
        self.assertEqual(status, 1)
        self.assertEqual(gate.call_count, 2)  # Markdown comparison and CLI gate.
        self.assertEqual(gate.call_args.args[2], ['C1'])
        self.assertIn('# C suite by engine', self.output.getvalue())

    def test_manifest_creation_error_does_not_prevent_measurement_or_finalization(self):
        runner = FakeRunner(self.root)
        with patch.object(report_files, 'begin', side_effect=OSError('cannot write manifest')):
            results = run_many([runner])
        self.assertEqual(len(results[0]), 2)
        self.assertTrue(results.report_errors)
        self.assertEqual(validation.validate_run(runner.out_dir), [])
        self.assertTrue((self.root / 'index.html').exists())

    def test_failed_atomic_finalization_can_still_mark_the_sink_failed(self):
        from pbench.sinks import files
        runner = FakeRunner(self.root)
        write = files.atomic_json
        def fail_complete(path, data):
            if data.get('state') == 'complete':
                raise OSError('disk failure on completion')
            return write(path, data)
        with patch.object(files, 'atomic_json', side_effect=fail_complete):
            with self.assertRaisesRegex(RuntimeError, 'disk failure on completion'):
                run_many([runner])
        run = json.loads((runner.out_dir / 'run.json').read_text())
        self.assertEqual(run['state'], 'failed')
        self.assertTrue(runner.sinks[0].results.closed)

    def test_failed_invocation_cannot_pass_with_complete_file_evidence(self):
        runner = FakeRunner(self.root)
        close = runner.sinks[0].close
        def fail_after_finalizing(*args):
            close(*args)
            raise RuntimeError('another finalization step failed')
        with patch.object(runner.sinks[0], 'close', side_effect=fail_after_finalizing):
            with self.assertRaises(RuntimeError):
                run_many([runner])
        self.assertEqual(validation.validate_run(runner.out_dir), [])
        manifest = next(self.root.glob('reports/*/report.json'))
        self.assertFalse(json.loads(manifest.read_text())['passed'])
        self.assertIn('execution: failed', (manifest.parent / 'README.md').read_text())

    def test_catalog_cache_is_invalidated_by_source_file_changes(self):
        runner = FakeRunner(self.root)
        run_many([runner])
        with (runner.out_dir / 'results.jsonl').open('a') as results:
            results.write('{}\n')
        report_files.catalog(self.root)
        text = (self.root / 'index.html').read_text()
        self.assertIn('source files changed; regenerate report', text)
        self.assertIn('2 / ? / ?', text)
        self.assertNotIn('valid at last report;', text)

    def test_catalog_handles_missing_and_malformed_caches(self):
        runner = FakeRunner(self.root)
        run_many([runner], report_format='none')
        cache = runner.out_dir / 'report-status.json'
        for value in (None, '{', json.dumps({'schema_version': 1, 'hashes': report_files.fingerprint(runner.out_dir),
                                           'problems': None})):
            with self.subTest(cache=value):
                if value is not None:
                    cache.write_text(value)
                report_files.catalog(self.root)
                self.assertIn('not validated; regenerate report', (self.root / 'index.html').read_text())

    def test_catalog_never_hashes_raw_logs_and_explicit_report_rechecks_them(self):
        runner = FakeRunner(self.root)
        run_cell = runner._run_cell
        def measured_cell(key):
            (runner.out_dir / 'raw' / str(key.rep)).write_text('original log')
            return run_cell(key)
        with patch.object(runner, '_run_cell', side_effect=measured_cell):
            run_many([runner])
        (runner.out_dir / 'raw/1').write_text('changed log')
        hash_file = report_files.sha256
        def small_sources_only(path):
            self.assertNotIn('raw', path.parts)
            return hash_file(path)
        with patch.object(report_files, 'sha256', side_effect=small_sources_only):
            report_files.catalog(self.root)
        self.assertIn('valid at last report; raw evidence not rechecked', (self.root / 'index.html').read_text())
        _, passed = report_files.result_set(reports.load(runner.out_dir))
        self.assertFalse(passed)
