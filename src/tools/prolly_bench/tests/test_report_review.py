"""Regressions confirmed from the report renderer review."""
import copy
import json
import os
import stat
import subprocess
import unittest
from pathlib import Path
from unittest.mock import patch

from evidence_helpers import c_result, seal
from pbench import ccompare, presentation, reports
import test_reports


class ReportReviewTest(unittest.TestCase):
    def setUp(self):
        fixtures = test_reports.ReportsTest('test_shared_values_all_metrics_and_standalone_html')
        fixtures.setUp()
        self.addCleanup(fixtures.doCleanups)
        self.root, self.source = fixtures.root, fixtures.source

    def rewrite(self, source):
        (source.path / 'results.jsonl').write_text(''.join(json.dumps(r) + '\n' for r in source.results))
        seal(source.path)
        return reports.load(source.path)

    def c_paths(self, names=('heap', 'prolly')):
        paths = []
        for name in names:
            am = 'heap' if name.startswith('heap') else name
            path = self.root / name
            c_result(path, am, {'C1': [{'file': 'select1.test', 'line': 1, 'status': 'ok'}]},
                     config={'run': {}, 'tools': {'slt_runner': '/nonexistent/slt-runner'}}, label=name)
            paths.append(path)
        return paths

    def test_missing_c_workload_remains_visible_in_a_failed_report(self):
        base, engine = self.c_paths()
        rows = [json.loads(line) for line in (engine / 'results.jsonl').read_text().splitlines()]
        rows[0]['key'].pop('workload')
        (engine / 'results.jsonl').write_text(json.dumps(rows[0]) + '\n')
        seal(engine)
        doc = reports.build([reports.load(base), reports.load(engine)], self.root)
        self.assertFalse(doc.passed)
        for renderer in presentation.RENDERERS.values():
            text = renderer.render(doc)
            self.assertIn('workload', text)
            self.assertIn('INVALID', text)

    def test_invalid_build_keys_do_not_reach_comparison_analyzer(self):
        base = self.source('base-prolly', build='base')
        original = copy.deepcopy(base.results)
        other = self.source('candidate-prolly', build='candidate')
        for field, value in [('rep', None), ('variant', None), ('workload', '?')]:
            with self.subTest(field=field):
                base.results = copy.deepcopy(original)
                if value is None:
                    base.results[0]['key'].pop(field)
                else:
                    base.results[0]['key'][field] = value
                bad = self.rewrite(base)
                doc = reports.build([bad, other], self.root)
                self.assertFalse(doc.passed)
                text = presentation.HtmlRenderer().render(doc)
                self.assertIn('comparison withheld', text.lower())
                self.assertIn(field, text)

    def test_malformed_collector_status_values_are_diagnostics(self):
        source = self.source(values={'tps': 123, 'latency_p99_ms': None})
        for value in ('error', 42, None, ['error'], ['error', 'reason', 'extra']):
            with self.subTest(value=value):
                source.results[0]['collector_status'] = {'write_amp': value}
                malformed = self.rewrite(source)
                doc = reports.build([malformed], self.root)
                self.assertFalse(doc.passed)
                for renderer in presentation.RENDERERS.values():
                    text = renderer.render(doc)
                    self.assertIn('collector', text)
                    self.assertIn('write', text)

    def test_correctness_corpus_failure_does_not_launch_comparator(self):
        base, engine = self.c_paths()
        (engine / 'raw/C1/slt/corpus.json').write_text('{"select1.test": "different-hash"}')
        seal(engine)
        sources = [reports.load(base), reports.load(engine)]
        self.assertTrue(all(not s.problems for s in sources))
        with patch.object(ccompare.subprocess, 'run', side_effect=AssertionError('unexpected comparator')) as run:
            doc = reports.build(sources, self.root)
        run.assert_not_called()
        self.assertFalse(doc.passed)
        self.assertIn('different corpus', presentation.HtmlRenderer().render(doc))
        self.assertFalse((engine / 'compare').exists())

    def test_correctness_timeout_is_a_failed_report_without_retrying_comparison(self):
        paths = self.c_paths()
        sources = [reports.load(p) for p in paths]
        with patch.object(ccompare.subprocess, 'run', side_effect=subprocess.TimeoutExpired('slt-runner', 3600)) as run:
            doc = reports.build(sources, self.root)
        self.assertEqual(run.call_count, 1)
        self.assertFalse(doc.passed)
        self.assertIn('timed out', presentation.HtmlRenderer().render(doc))

    def test_failing_correctness_counts_are_retained_from_one_comparison(self):
        sources = [reports.load(p) for p in self.c_paths()]
        findings = {'C1': {'counts': {'regression': 1}, 'stale_exceptions': []}}
        with patch.object(ccompare, 'compare', return_value=findings) as compare:
            doc = reports.build(sources, self.root)
        compare.assert_called_once()
        self.assertFalse(doc.passed)
        section = next(s for s in doc.sections if s.title == 'Correctness against heap')
        self.assertEqual(section.rows[0][section.columns.index('regression')], '1')
        self.assertEqual(section.rows[0][-1], 'FAIL')

    def test_irrelevant_metric_rows_are_omitted_but_missing_repetitions_stay_visible(self):
        source = self.source(reps=2, values={'tps': 123})
        serial = copy.deepcopy(source.results[0])
        serial['key'].update(workload='S13', variant='serial')
        serial['metrics'] = {'hot_query_seconds': 42}
        missing = copy.deepcopy(serial)
        missing['key']['rep'] = 2
        missing['metrics'] = {}
        source.results.extend([serial, missing])
        source.run['planned_cells'] = [r['key'] for r in source.results]
        doc = reports.build([source], self.root)
        section = next(s for s in doc.sections if s.title.startswith('TPC-H measured hot queries'))
        self.assertEqual([r[0] for r in section.rows], ['S13'])
        self.assertEqual(section.rows[0][-1].text, 'missing 1/2')

    def test_workloads_and_client_counts_sort_numerically(self):
        source = self.source(values={'tps': 123})
        template = source.results[0]
        source.results = []
        expected = []
        for workload in ('S1', 'S2', 'S10'):
            for clients in (1, 4, 16):
                r = copy.deepcopy(template)
                r['key'].update(workload=workload, clients=clients)
                source.results.append(r)
                expected.append((workload, str(clients)))
        source.run['planned_cells'] = [r['key'] for r in source.results]
        doc = reports.build([source], self.root)
        for section in doc.sections:
            if section.title == 'Workload overview' or section.title.startswith('Throughput ('):
                self.assertEqual([(r[0], r[3]) for r in section.rows], expected)

    def test_explicit_baseline_must_be_loaded(self):
        source = self.source()
        with self.assertRaisesRegex(ValueError, 'baseline.*loaded'):
            reports.build([source], self.root, baseline=self.root / 'not-loaded')

    def test_explicit_baseline_is_rejected_outside_s_suite(self):
        source = self.source(suite='V')
        with self.assertRaisesRegex(ValueError, 'baseline.*S suite'):
            reports.build([source], self.root, baseline=source.path)
        source = reports.load(self.c_paths(('heap',))[0])
        with self.assertRaisesRegex(ValueError, 'baseline.*S suite'):
            reports.build([source], self.root, baseline=source.path)

    def test_loaded_s_baseline_still_produces_ratios(self):
        base = self.source('reference', values={'tps': 100})
        other = self.source('candidate', values={'tps': 125})
        doc = reports.build([base, other], self.root, baseline=base.path)
        self.assertTrue(doc.passed)
        throughput = next(s for s in doc.sections if s.title.startswith('Throughput ('))
        candidate = throughput.columns.index('candidate')
        self.assertIn('1.25x', throughput.rows[0][candidate].text)

    def test_missing_or_ambiguous_c_baseline_cannot_pass(self):
        sources = [reports.load(p) for p in self.c_paths(('heap', 'heap2', 'prolly', 'other'))]
        for selection in ([sources[0]], [sources[2]], sources[:3], sources[2:]):
            with self.subTest(labels=[s.label for s in selection]):
                doc = reports.build(selection, self.root)
                self.assertFalse(doc.passed)
                self.assertIn('no correctness verdict', presentation.HtmlRenderer().render(doc))

    def test_new_report_permissions_follow_umask_and_existing_modes_are_preserved(self):
        doc = presentation.Document('Readable report')
        mask = os.umask(0o022)
        try:
            paths = presentation.write(doc, self.root / 'report')
            for path in paths:
                self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o644)
                path.chmod(0o640)
            presentation.write(doc, self.root / 'report')
            for path in paths:
                self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o640)
        finally:
            os.umask(mask)

    def test_new_private_reports_respect_restrictive_umask(self):
        mask = os.umask(0o077)
        try:
            paths = presentation.write(presentation.Document('Private report'), self.root / 'private')
            for path in paths:
                self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600)
        finally:
            os.umask(mask)

    def test_dotted_report_stems_do_not_collide(self):
        for day in ('19', '20'):
            stem = self.root / f'S-2026.10.{day}'
            paths = presentation.write(presentation.Document(f'Report {day}'), stem)
            self.assertEqual(set(paths), {Path(str(stem) + '.md'), Path(str(stem) + '.html')})
        for day in ('19', '20'):
            self.assertIn(f'Report {day}', (self.root / f'S-2026.10.{day}.html').read_text())
