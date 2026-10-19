import contextlib
import io
import json
import tempfile
import unittest
from pathlib import Path

from pbench import finding_reports, presentation
from pbench.__main__ import main
import test_creport


class FindingReportsTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def test_full_s_artifact_keeps_findings_dropped_from_bounded_comment(self):
        counters = {'layer': 'counters', 'thresholds': {}, 'passed': True, 'accepted': [],
                    'findings': [{'workload': 'S1', 'engine': 'prolly', 'counter': f'counter{i}',
                                  'unit': 'transaction', 'units': 100, 'base': 100, 'candidate': 101,
                                  'result': 'jitter'} for i in range(1000)]}
        path = self.root / 'counters.json'
        path.write_text(json.dumps(counters))
        out = self.root / 'comment.md'
        stem = self.root / 'report'
        with contextlib.redirect_stdout(io.StringIO()):
            status = main(['sreport', '--counters', str(path), '--head', 'abc', '--base', 'def',
                           '--out', str(out), '--report-output', str(stem)])
        self.assertEqual(status, 0)
        self.assertTrue(out.read_text().startswith('<!-- s-suite -->'))
        self.assertLessEqual(len(out.read_text()), 65000)
        self.assertNotIn('counter999', out.read_text())
        for suffix in ('.md', '.html'):
            text = stem.with_suffix(suffix).read_text()
            self.assertIn('counter999', text)
            self.assertIn('Counter findings', text)
            self.assertIn('PASS', text)
            self.assertNotIn('<script>', text)

    def test_missing_s_findings_produce_failed_full_report(self):
        with contextlib.redirect_stdout(io.StringIO()):
            status = main(['sreport', '--head', 'abc', '--base', 'def', '--out', str(self.root / 'comment.md'),
                           '--failed-job', 'timing: failed'])
        self.assertEqual(status, 1)
        text = (self.root / 'comment-full.html').read_text()
        self.assertIn('S suite: FAIL', text)
        self.assertIn('timing: failed', text)

    def test_full_c_report_has_same_shard_gate_and_record_details(self):
        fixture = test_creport.ReportTest('test_pass')
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        result = fixture.report(max_chars=500)
        document = finding_reports.correctness(result.summary, {'base': 'base', 'head': 'head'}, result.details)
        self.assertEqual(document.passed, result.passed)
        for renderer in presentation.RENDERERS.values():
            text = renderer.render(document)
            self.assertIn('C suite: PASS', text)
            self.assertIn('Engine summary', text)
            self.assertIn('95.833%', text)
            self.assertIn('1/2', text)
            self.assertIn('2/2', text)
        fixture.skip.add(('2/2', 'prolly'))
        failed = fixture.report()
        document = finding_reports.correctness(failed.summary, {}, failed.details)
        self.assertFalse(document.passed)
        self.assertIn('C suite: FAIL', presentation.HtmlRenderer().render(document))

    def test_calibration_with_no_evidence_still_leaves_html_and_markdown(self):
        out = self.root / 'calibration.md'
        with contextlib.redirect_stdout(io.StringIO()):
            status = main(['scalibrate', '--out', str(out)])
        self.assertEqual(status, 1)
        self.assertIn('No valid calibration', (self.root / 'calibration-full.html').read_text())
        self.assertTrue((self.root / 'calibration-full.md').exists())

    def test_gate_text_is_escaped_and_report_only_timing_is_explicit(self):
        timing = {'report_only': True, 'cells': [
            {'workload': 'S1', 'metric': 'tps', 'result': 'inconclusive', 'ratio': 1.01,
             'low': 0.95, 'high': 1.1, 'attempts': ['first', 'retry'], 'problems': ['<script>|bad']}]}
        doc = finding_reports.storage(None, timing, passed=False, metadata={}, failed_jobs=[])
        html = presentation.HtmlRenderer().render(doc)
        md = presentation.MarkdownRenderer().render(doc)
        self.assertIn('report-only', html)
        self.assertIn('retry', html)
        self.assertNotIn('<script>', html)
        self.assertNotIn('<script>', md)
        self.assertIn('inconclusive', md)
