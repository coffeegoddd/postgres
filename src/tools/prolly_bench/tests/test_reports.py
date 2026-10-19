import json
import tempfile
import unittest
from html.parser import HTMLParser
from pathlib import Path
from unittest.mock import patch

from evidence_helpers import seal, c_result
from pbench import reports, presentation


class Tables(HTMLParser):
    def __init__(self, text):
        super().__init__()
        self.cells = []
        self.tags = []
        self.feed(text)

    def handle_starttag(self, tag, attrs):
        self.tags.append(tag)

    def handle_data(self, data):
        self.cells.append(data)


class ReportsTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def source(self, name='prolly', *, suite='S', am='prolly', build='', reps=1, values=None):
        path = self.root / name
        path.mkdir()
        values = values or {'tps': 123, 'latency_p99_ms': 2, 'custom.counter': 4321}
        rows = [{'key': {'suite': suite, 'workload': suite + '1', 'size': 'memory',
                         'variant': '', 'clients': 4, 'rep': rep, 'build': build},
                 'status': 'ok', 'metrics': values, 'notes': ['whole device <scope>'],
                 'measurement': {'quality': 'smoke', 'seed': rep}} for rep in range(reps)]
        (path / 'results.jsonl').write_text(''.join(json.dumps(r) + '\n' for r in rows))
        seal(path, suite, am, name)
        run = json.loads((path / 'run.json').read_text())
        run.update(build=build, builds=['base', 'candidate'] if build else [])
        (path / 'run.json').write_text(json.dumps(run))
        return reports.load(path)

    def test_shared_values_all_metrics_and_standalone_html(self):
        source = self.source()
        doc = reports.build([source], self.root)
        md = presentation.MarkdownRenderer().render(doc)
        html = presentation.HtmlRenderer().render(doc)
        parsed = Tables(html)
        self.assertTrue(doc.passed)
        for text in ('custom.counter', '4,321', '123', 'unit unspecified', 'whole device'):
            self.assertIn(text, md)
            self.assertIn(text, ''.join(parsed.cells))
        self.assertIn('caption', parsed.tags)
        self.assertNotIn('script', parsed.tags)
        self.assertNotIn('http', html)
        self.assertNotIn("class='good'", html)

    def test_missing_planned_repetition_and_newest_error_are_visible(self):
        source = self.source(reps=2)
        run = json.loads((source.path / 'run.json').read_text())
        run['planned_cells'].append({**run['planned_cells'][0], 'workload': 'S2'})
        (source.path / 'run.json').write_text(json.dumps(run))
        source = reports.load(source.path)
        doc = reports.build([source], self.root)
        self.assertFalse(doc.passed)
        md = presentation.MarkdownRenderer().render(doc)
        self.assertIn('S2', md)
        self.assertIn('0/1 planned', md)
        self.assertIn('INVALID', md)

    def test_missing_metric_not_averaged_over_survivors(self):
        source = self.source(reps=2)
        source.results[1]['metrics'] = {}
        cell = reports._metric_value(source, ('S1', 'memory', '-', 4), source.results,
                                     reports.Metric('tps', 'TPS', 'txn/s'))
        self.assertEqual(cell.text, 'missing 1/2')

    def test_failed_unsupported_and_read_only_cells_explain_status(self):
        source = self.source()
        key = ('S1', 'memory', '-', 4)
        m = reports.Metric('wal_bytes_per_operation', 'WAL', 'bytes')
        source.results[0]['status'] = 'not_implemented'
        source.results[0]['message'] = 'no branches yet'
        cell = reports._metric_value(source, key, source.results, m)
        self.assertIn('not_implemented', cell.text)
        self.assertIn('no branches yet', cell.detail)
        source.results[0]['status'] = 'ok'
        source.results[0]['read_only'] = True
        self.assertEqual(reports._metric_value(source, key, source.results, m).text, 'n/a')

    def test_build_comparisons_present_in_both_formats(self):
        sources = [self.source(b + '-' + am, am=am, build=b, reps=5)
                   for b in ('base', 'candidate') for am in ('heap', 'prolly')]
        doc = reports.build(sources, self.root)
        for renderer in presentation.RENDERERS.values():
            text = renderer.render(doc)
            self.assertIn('Candidate over base', text)
        self.assertTrue(doc.passed)

    def test_incompatible_evidence_withholds_ratios(self):
        base, other = self.source('heap', am='heap'), self.source()
        run = json.loads((other.path / 'run.json').read_text())
        run['comparison_identity'] = 'different'
        (other.path / 'run.json').write_text(json.dumps(run))
        doc = reports.build([base, other], self.root)
        self.assertFalse(doc.passed)
        self.assertIn('comparison withheld', presentation.MarkdownRenderer().render(doc))

    def test_c_suite_compares_heap_without_parsing_markdown(self):
        records = {'C1': [{'file': 'select1.test', 'line': 1, 'status': 'ok'}]}
        sources = []
        for am in ('heap', 'prolly'):
            path = self.root / am
            c_result(path, am, records)
            sources.append(reports.load(path))
        from pbench import ccompare
        comparison = {'C1': {'counts': {'agree': 1}, 'gate_failures': 0, 'stale_exceptions': []}}
        with patch.object(ccompare, 'compare', return_value=comparison):
            doc = reports.build(sources, self.root)
        for renderer in presentation.RENDERERS.values():
            text = renderer.render(doc)
            self.assertIn('Correctness against heap', text)
            self.assertIn('PASS', text)
        self.assertTrue(doc.passed)

    def test_v_has_no_heap_reference(self):
        source = self.source(suite='V', values={'commit_ms': 1, 'wal_bytes': 42})
        doc = reports.build([source], self.root)
        self.assertIn('Commit (ms)', presentation.HtmlRenderer().render(doc))
        self.assertNotIn('1.00x', presentation.MarkdownRenderer().render(doc))

    def test_corrupt_or_legacy_evidence_remains_reportable(self):
        source = self.source()
        (source.path / 'run.json').write_text('{')
        (source.path / 'results.jsonl').write_text('{\n')
        source = reports.load(source.path)
        doc = reports.build([source], self.root)
        self.assertFalse(doc.passed)
        self.assertIn('INVALID', presentation.HtmlRenderer().render(doc))

    def test_escaping_and_portable_links(self):
        value = '<script>alert(1)</script>|[link](javascript:bad)\nsecond'
        doc = presentation.Document(value, sections=[presentation.Section(value, ['column'], [[value,
             presentation.link(self.root / 'with spaces' / 'run.json', self.root, value)]])])
        html = presentation.HtmlRenderer().render(doc)
        md = presentation.MarkdownRenderer().render(doc)
        self.assertNotIn('<script>', html)
        self.assertNotIn('<script>', md)
        self.assertIn('\\|', md)
        self.assertIn('with%20spaces/run.json', html)
        self.assertIn('with%20spaces/run.json', md)

    def test_atomic_regeneration_preserves_evidence_and_previous_report_on_render_error(self):
        source = self.source()
        evidence = {p: p.read_bytes() for p in source.path.iterdir() if p.is_file()}
        doc = reports.build([source], source.path)
        stem = source.path / 'index'
        paths = presentation.write(doc, stem, markdown_path=source.path / 'README.md')
        before = {p: p.read_bytes() for p in paths}
        presentation.write(doc, stem, markdown_path=source.path / 'README.md')
        self.assertEqual(before, {p: p.read_bytes() for p in paths})
        self.assertEqual(evidence, {p: p.read_bytes() for p in evidence})
        with patch.object(presentation.HtmlRenderer, 'render', side_effect=RuntimeError('render failed')):
            with self.assertRaises(RuntimeError):
                presentation.write(doc, stem, markdown_path=source.path / 'README.md')
        self.assertEqual(before, {p: p.read_bytes() for p in paths})
