"""Regressions for full CI reports, gate isolation and artifact layout."""
import contextlib
import io
import json
import os
import shutil
from html.parser import HTMLParser
from urllib.parse import unquote
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from pbench import creport, finding_reports, presentation, sreport
from pbench.__main__ import main
import test_creport
from test_scalibrate import counter_run

REPO = Path(__file__).resolve().parents[4]


def step_script(workflow, name):
    lines = (REPO / '.github/workflows' / workflow).read_text().splitlines()
    start = next(i for i, line in enumerate(lines) if line.strip() == '- name: ' + name)
    start = next(i for i in range(start, len(lines)) if lines[i].strip() == 'run: |') + 1
    body = []
    for line in lines[start:]:
        if line.strip() and not line.startswith('          '):
            break
        body.append(line[10:])
    return '\n'.join(body)


class CIReportReviewTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.output, self.errors = io.StringIO(), io.StringIO()
        for context in (contextlib.redirect_stdout(self.output), contextlib.redirect_stderr(self.errors)):
            context.__enter__()
            self.addCleanup(context.__exit__, None, None, None)

    def fixture(self):
        fixture = test_creport.ReportTest('test_pass')
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        return fixture

    def counters(self, passed=True, findings=()):
        path = self.root / 'counters.json'
        path.write_text(json.dumps({'passed': passed, 'thresholds': {}, 'findings': list(findings)}))
        return path

    def test_c_failure_examples_keep_engine_name_and_engine_result(self):
        fixture = self.fixture()
        observed = {'status': 'not_ok', 'msg': 'lost a row'}
        fixture.diffs['2/2']['C4'] = [{'file': 'random/r.test', 'line': 1, 'class': 'regression',
                                     'engine': observed, 'baseline': {'status': 'ok'}}]
        report = fixture.report()
        example = report.details['Failure examples'][0]
        self.assertEqual(example['engine'], 'prolly')
        self.assertEqual(example['engine_result'], observed)
        self.assertEqual(example['query'], 'SELECT 1 FROM t1')

    def test_c_heap_new_and_changed_regressions_have_queries(self):
        fixture = self.fixture()
        for am in ('heap', 'prolly'):
            fixture.records['1/2', am]['C1'][0].update(status='not_ok', sqlstate='22012')
            fixture.records['2/2', am]['C3'][2].update(status='not_ok', sqlstate='XX000')
        report = fixture.report()
        examples = [r for r in report.details['Failure examples'] if r['engine'] == 'heap']
        self.assertEqual({r['file'] for r in examples}, {'select1.test', 'index/a.test'})
        self.assertTrue(all(r['class'] == 'heap regression' and 'SELECT' in r['query'] for r in examples))
        self.assertTrue(any(r.get('listed') == ['not_ok', '42883', '-'] for r in examples))
        document = finding_reports.correctness(report.summary, {}, report.details)
        section = next(s for s in document.sections if s.title == 'Details: Failure examples')
        self.assertIn('query', section.columns)
        self.assertEqual(len(section.rows), 2)

    def test_s_full_report_failure_keeps_gate_verdict_and_comment_output(self):
        for passed in (True, False):
            for target, failure in ((presentation, OSError('report not writable')),
                                    (finding_reports, TypeError('bad finding'))):
                with self.subTest(passed=passed, target=target.__name__):
                    path = self.counters(passed)
                    name = 'write' if target is presentation else 'storage'
                    with patch.object(target, name, side_effect=failure):
                        status = main(['sreport', '--counters', str(path), '--head', 'head', '--base', 'base',
                                       '--out', str(self.root / 'comment.md')])
                    self.assertEqual(status, int(not passed))
                    self.assertIn((self.root / 'comment.md').read_text(), self.output.getvalue())
                    self.assertIn(str(failure), self.errors.getvalue())

    def test_s_missing_counter_name_does_not_break_full_report(self):
        path = self.counters(findings=[{'workload': 'S1', 'engine': 'prolly', 'base': 1,
                                       'candidate': 1, 'result': 'same'}])
        status = main(['sreport', '--counters', str(path), '--head', 'head', '--base', 'base',
                       '--out', str(self.root / 'comment.md')])
        self.assertEqual(status, 0)
        self.assertTrue((self.root / 'comment-full.html').exists())

    def test_c_full_report_failure_keeps_summary_diagnostics_and_verdict(self):
        for passed in (True, False):
            for target, failure in ((presentation, OSError('cannot write HTML')),
                                    (finding_reports, TypeError('invalid full report'))):
                with self.subTest(passed=passed, target=target.__name__):
                    report = creport.Report('<!-- c-suite -->\ncomment\n', passed,
                                            [] if passed else ['gate failed'], {'pass': passed})
                    name = 'write' if target is presentation else 'correctness'
                    with patch.object(creport, 'build', return_value=report), \
                            patch.object(target, name, side_effect=failure):
                        status = main(['creport', 'shard-results', '--shards', '1', '--base-baseline', 'base.gz',
                                       '--out', str(self.root / 'comment.md'),
                                       '--json', str(self.root / 'summary.json')])
                    self.assertEqual(status, int(not passed))
                    self.assertEqual(json.loads((self.root / 'summary.json').read_text()), report.summary)
                    self.assertIn('C suite: ' + ('PASS' if passed else 'FAIL'), self.output.getvalue())
                    for problem in report.problems:
                        self.assertIn(problem, self.errors.getvalue())
                    self.assertIn(str(failure), self.errors.getvalue())

    def test_calibration_full_report_failure_keeps_primary_outputs(self):
        with patch.object(presentation, 'write', side_effect=OSError('no HTML permission')):
            status = main(['scalibrate', '--out', str(self.root / 'calibration.md'),
                           '--json', str(self.root / 'calibration.json')])
        self.assertEqual(status, 1)  # No calibration evidence, independent of rendering.
        self.assertIn((self.root / 'calibration.md').read_text(), self.output.getvalue())
        self.assertIsNone(json.loads((self.root / 'calibration.json').read_text())['counters'])
        self.assertIn('no HTML permission', self.errors.getvalue())

    def test_full_s_findings_are_rendered_once_and_keep_extra_evidence(self):
        counters = {'findings': [{'workload': 'S1', 'engine': 'prolly', 'counter': 'uniquecounter123',
                                 'result': 'same', 'base': 1, 'candidate': 1, 'threshold_note': 'retained detail'}]}
        timing = {'cells': [{'workload': 'S1', 'engine': 'prolly', 'metric': 'uniquemetric123',
                             'result': 'same', 'over_heap': {'base': 1, 'candidate': 1}}]}
        doc = finding_reports.storage(counters, timing, passed=True, metadata={}, failed_jobs=[])
        for renderer in presentation.RENDERERS.values():
            text = renderer.render(doc)
            self.assertEqual(text.count('uniquecounter123'), 1)
            self.assertEqual(text.count('uniquemetric123'), 1)
            self.assertIn('retained detail', text)
            self.assertIn('over', text)

    def test_full_c_engine_summary_is_rendered_once(self):
        doc = finding_reports.correctness({'pass': True, 'engines': {'uniqueengine123': {
            'ran': 2, 'ok': 2, 'gate': True, 'counts': {'agree': 2}, 'stale_exceptions': 0}}}, {})
        for renderer in presentation.RENDERERS.values():
            self.assertEqual(renderer.render(doc).count('uniqueengine123'), 1)

    def test_full_report_text_accepts_mixed_json_keys(self):
        doc = finding_reports.calibration({'counters': {'rows': [{'value': {'str': 1, 2: 3}}]}})
        self.assertIn('str', presentation.HtmlRenderer().render(doc))

    def test_s_counter_parts_are_merged_once(self):
        with patch.object(sreport, 'merge_counters', wraps=sreport.merge_counters) as merge:
            main(['sreport', '--counters', str(self.counters()), '--head', 'h', '--base', 'b',
                  '--out', str(self.root / 'comment.md')])
        self.assertEqual(merge.call_count, 1)

    def test_nightly_judgement_preserves_provenance_for_missing_clean_and_regressed_results(self):
        script = step_script('performance.yml', 'Judge the night')
        bench = self.root / 'bin'
        bench.mkdir()
        program = bench / 'prolly_bench'
        program.write_text('''#!/usr/bin/env python3
import json, pathlib, sys
pathlib.Path('arguments.json').write_text(json.dumps(sys.argv[1:]))
pathlib.Path('nightly/comment.md').write_text('nightly comment\\n')
sys.exit(0 if pathlib.Path('nightly/sgate.json').exists() else 1)
''')
        program.chmod(0o755)
        for state in ('missing', 'clean', 'regressed'):
            with self.subTest(state=state):
                root = self.root / state
                (root / 'nightly').mkdir(parents=True)
                if state != 'missing':
                    (root / 'nightly/sgate.json').write_text(json.dumps({'cells': [
                        {'result': 'confirmed' if state == 'regressed' else 'same'}]}))
                env = dict(os.environ, BENCH=str(bench), BASE='base', CANDIDATE='head', RUN_URL='https://ci/run',
                           GITHUB_OUTPUT=str(root / 'outputs'), GITHUB_STEP_SUMMARY=str(root / 'summary'))
                result = subprocess.run(['bash', '-e', '-c', script], cwd=root, env=env,
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                args = json.loads((root / 'arguments.json').read_text())
                self.assertIn('--runner', args)
                self.assertEqual(args[args.index('--runner') + 1], 'ubuntu-24.04 (GitHub-hosted)')
                self.assertEqual('--timing' in args, state != 'missing')
                self.assertEqual((root / 'outputs').read_text(), 'verdict=' + ('none' if state == 'missing' else state) + '\n')
                self.assertIn('nightly comment', (root / 'summary').read_text())

    def test_report_uploads_do_not_duplicate_source_artifacts(self):
        for workflow, artifact, excluded in (
                ('correctness.yml', 'c-suite-report', ['shards']),
                ('performance-calibration.yml', 's-calibration-report', ['runs', 'calibration']),
                ('performance.yml', 's-suite-report', ['counters', 'timing'])):
            text = (REPO / '.github/workflows' / workflow).read_text()
            block = text.split('name: ' + artifact + '\n')[1].split('retention-days:')[0]
            with self.subTest(artifact=artifact):
                for path in excluded:
                    self.assertNotIn(path, [line.strip() for line in block.splitlines()])

    def test_passing_calibration_keeps_verdict_when_full_report_build_fails(self):
        runs = [counter_run(self.root, f'counters-1-{i}', {'wal_bytes': 1000.0}) for i in range(4)]
        with patch.object(finding_reports, 'calibration', side_effect=TypeError('bad calibration adapter')):
            status = main(['scalibrate', '--counters', *map(str, runs),
                           '--out', str(self.root / 'calibration.md'),
                           '--json', str(self.root / 'calibration.json')])
        self.assertEqual(status, 0)
        self.assertEqual(json.loads((self.root / 'calibration.json').read_text())['counters']['runs'], 4)
        self.assertIn((self.root / 'calibration.md').read_text(), self.output.getvalue())
        self.assertIn('bad calibration adapter', self.errors.getvalue())

    def test_clean_nightly_keeps_verdict_when_full_report_write_fails(self):
        path = self.root / 'timing.json'
        path.write_text(json.dumps({'report_only': True, 'cells': [
            {'workload': 'S1', 'clients': 1, 'engine': 'prolly', 'metric': 'throughput',
             'ratio': 1, 'low': .99, 'high': 1.01, 'result': 'same'}]}))
        with patch.object(presentation, 'write', side_effect=OSError('nightly HTML not writable')):
            status = main(['sreport', '--nightly', '--timing', str(path), '--head', 'h', '--base', 'b',
                           '--out', str(self.root / 'nightly.md')])
        self.assertEqual(status, 0)
        self.assertIn('no confirmed regression', (self.root / 'nightly.md').read_text())
        self.assertIn((self.root / 'nightly.md').read_text(), self.output.getvalue())
        self.assertIn('nightly HTML not writable', self.errors.getvalue())

    def test_source_links_work_when_original_artifacts_are_restored_beside_reports(self):
        class Links(HTMLParser):
            def __init__(self):
                super().__init__()
                self.paths = []
            def handle_starttag(self, tag, attrs):
                if tag == 'a':
                    self.paths.extend(value for key, value in attrs if key == 'href' and not value.startswith('#'))
        for name, output_dir, sources in (
                ('C', 'comment', ['shards/c-shard-1/2026-C-heap']),
                ('S', 'comment', ['counters/s-counters-0', 'timing']),
                ('calibration', 'calibration', ['runs/counters-1-1', 'runs/timing-1'])):
            with self.subTest(report=name):
                staging = self.root / name / 'staging'
                unpacked = self.root / name / 'unpacked'
                paths = [staging / source for source in sources]
                for path in paths:
                    path.mkdir(parents=True)
                    (path / 'index.html').write_text('saved run report')
                reports = finding_reports.write(lambda: presentation.Document('Full report'),
                    staging / output_dir / 'comment.md', staging / output_dir / 'report', paths)
                (unpacked / output_dir).mkdir(parents=True)
                for report in reports:
                    shutil.copyfile(report, unpacked / output_dir / report.name)
                for source in sources:
                    shutil.copytree(staging / source, unpacked / source)
                html = unpacked / output_dir / 'report.html'
                links = Links()
                links.feed(html.read_text())
                self.assertEqual(len(links.paths), 2 * len(sources))
                self.assertTrue(all((html.parent / unquote(path)).exists() for path in links.paths))
