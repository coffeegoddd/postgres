"""Render the same comparison tables to standalone HTML and Markdown."""
from collections import defaultdict
from html import escape


ENGINES = ("dolt", "doltgres", "prolly")
ENGINE_NAMES = ("Dolt", "Doltgres", "PostgreSQL prolly")
OPERATIONS = {
    "round_trip": "SQL round trip",
    "branch": "Create branch",
    "checkout": "Checkout",
    "reset_soft": "Soft reset",
    "reset_hard": "Hard reset",
    "diff_equal": "Diff identical roots",
    "diff_working": "Diff HEAD → working",
    "diff_history": "Diff old → HEAD",
    "diff_working_history": "Diff old → working",
    "resolve_ancestor": "Resolve ancestor",
}


def number(value, unit="", scale=1):
    if value is None:
        return "—"
    value /= scale
    digits = 0 if abs(value) >= 1000 else 2 if abs(value) >= 1 else 3
    if value and abs(value) < .001:
        formatted = f"{value:.2g}"
    else:
        formatted = f"{value:,.{digits}f}"
    return formatted + (" " + unit if unit else "")


def count(value):
    """A counter's value: whole numbers as they are (a median of two may
    be a half)."""
    return f"{int(value):,}" if value == int(value) else number(value)


def memory_cell(row, fields, unit, scale):
    value = " / ".join(number(row.get("median_" + f), unit, scale) for f in fields)
    if row["short_memory_observations"]:
        value += f" [short {row['short_memory_observations']}/{row['repetitions']}]"
    return value


def resource_cell(row, metric, unit, scale):
    value = number(row.get('median_' + metric), unit, scale)
    if metric == 'cpu_seconds_delta' and row.get('short_cpu_observations'):
        value += f" [short {row['short_cpu_observations']}/{row['repetitions']}]"
    return value


def fit_cell(item):
    if item["status"] == "descriptive":
        lo, hi = item["bootstrap_95_percent"]
        return (f"{item['exponent']:.3f} [{lo:.3f}, {hi:.3f}]; "
                f"R² {number(item['r_squared_log_space'])}")
    reason = item.get("reason", "insufficient evidence")
    if reason.startswith("need >=4"):
        reason = "insufficient sweep"
    elif "too short" in reason:
        reason = "sampling too slow"
    elif "nonpositive" in reason:
        reason = "zero/nonpositive observations"
    return "Inconclusive: " + reason


class Document:
    """Common content model keeps both formats' values and cautions identical."""
    def __init__(self):
        self.markdown = []
        self.html = []
        self.headings = []

    def heading(self, level, title):
        anchor = "section-" + str(len(self.headings))
        self.headings.append((level, title, anchor))
        self.markdown.append(f"{'#'*level} {title}")
        self.html.append(f'<h{level} id="{anchor}">{escape(title)}</h{level}>')

    def paragraph(self, text):
        self.markdown.append(text)
        self.html.append(f"<p>{escape(text)}</p>")

    def table(self, headers, rows):
        def md(value):
            return str(value).replace("&", "&amp;").replace("<", "&lt;").replace(
                ">", "&gt;").replace("|", "&#124;").replace("\n", " ")
        self.markdown.append('\n'.join([
            '| ' + ' | '.join(map(md, headers)) + ' |',
            '| ' + ' | '.join('---' for _ in headers) + ' |',
            *['| ' + ' | '.join(map(md, row)) + ' |' for row in rows],
        ]))
        html = '<div class="table-scroll"><table><thead><tr>'
        html += ''.join(f'<th scope="col">{escape(str(h))}</th>' for h in headers)
        html += '</tr></thead><tbody>'
        for row in rows:
            html += '<tr>'
            html += f'<th scope="row">{escape(str(row[0]))}</th>'
            html += ''.join(f'<td>{escape(str(cell))}</td>' for cell in row[1:])
            html += '</tr>'
        self.html.append(html + '</tbody></table></div>')

    def save(self, path):
        (path / 'report.md').write_text('\n\n'.join(self.markdown) + '\n')
        nav = ' · '.join(f'<a href="#{anchor}">{escape(title)}</a>'
                         for level, title, anchor in self.headings if level == 2)
        (path / 'report.html').write_text('''<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Version-control benchmark comparison</title>
<style>
:root{color-scheme:light dark;--bg:#f5f7fa;--panel:#fff;--ink:#172535;
--muted:#526171;--line:#d8e0e8;--stripe:#f4f7fa;--accent:#075eaa}
@media(prefers-color-scheme:dark){:root{--bg:#101820;--panel:#17222e;--ink:#e5edf6;
--muted:#adbdcd;--line:#34485a;--stripe:#1e2c3a;--accent:#7abfff}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);
font:15px/1.55 system-ui,sans-serif}main{max-width:1500px;margin:auto;padding:32px}
h1{font-size:30px;letter-spacing:-.5px}h2{margin-top:52px;border-bottom:2px solid var(--line);
padding-bottom:10px}h3{margin-top:32px}p{max-width:1050px;color:var(--muted)}
a{color:var(--accent)}nav{background:var(--panel);padding:16px;border:1px solid var(--line);
border-radius:8px;line-height:2}.table-scroll{overflow:auto;margin:16px 0 28px;
border:1px solid var(--line);border-radius:8px;background:var(--panel)}
table{width:100%;border-collapse:collapse;font-variant-numeric:tabular-nums}
th,td{text-align:left;padding:10px 14px;border-bottom:1px solid var(--line);vertical-align:top}
thead th{position:sticky;top:0;background:var(--stripe);white-space:nowrap}
tbody th{font-weight:500;min-width:170px}tbody tr:nth-child(even){background:var(--stripe)}
td{min-width:145px}tr:last-child>*{border-bottom:0}
@media(max-width:700px){main{padding:16px}h1{font-size:25px}th,td{padding:8px}}
@media print{body{background:white;color:black}main{padding:0}.table-scroll{overflow:visible;
border:0}thead{display:table-header-group}thead th{position:static}tr{break-inside:avoid}}
</style></head><body><main>''' +
            f'<nav aria-label="Report sections">{nav}</nav>' +
            '\n'.join(self.html) + '</main></body></html>\n')


def render(path, manifest, summaries, fits, drift=()):
    doc = Document()
    doc.heading(1, "Version-control benchmark comparison")
    doc.paragraph(f"Run: {path.name}. Status: {manifest['status']}. "
                  f"Measured observations: {manifest['planned_samples']:,}; warmups excluded.")
    doc.paragraph("Columns compare matching workloads across engines. All values are medians unless stated otherwise. "
                  "— means not measured or unavailable, never zero. Short pilot runs validate functionality; "
                  "they do not establish performance rankings.")
    doc.heading(2, "Run details")
    info = []
    for engine, label in zip(ENGINES, ENGINE_NAMES):
        binary = manifest.get('binaries', {}).get(engine)
        if not binary:
            info.append([label, 'Not measured', '—'])
            continue
        source = binary.get('build', {}).get('source') or {}
        revision = source.get('commit', 'Unverified source')
        if source.get('status'):
            revision += ' (working tree modified)'
        info.append([label, revision, binary.get('sha256', '—')[:16]])
    doc.table(['Engine', 'Source revision', 'Binary SHA-256 prefix'], info)
    doc.paragraph("Latency is a persistent SQL request through complete result consumption, with warm caches. "
                  "Engine and operation order are seeded. SQL setup, validation, and restoration are outside measurement. "
                  "Memory, CPU and I/O counters come from instrumented passes of their own; their latency is excluded here.")
    doc.paragraph('Clean and dirty working sets are reported separately. Historical changes K and pending dirty rows Q '
                  'are independent dimensions. Small, medium, and large name configurable dirty profiles; exact row counts '
                  'appear in each table heading. Dirty SQL writes are transaction-committed but not version-committed. '
                  'SQL checkout leaves pending edits on the source branch and switches to the destination’s working set.')

    fixtures = defaultdict(lambda: defaultdict(dict))
    for row in summaries:
        fixture = (row['rows'], row['changed_rows'], row['history_commits'],
                   row.get('working_set', 'clean'), row.get('dirty_rows', 0))
        case = (row['operation'], row['distance'])
        fixtures[fixture][(row['pass'], case)][row['engine']] = row

    def case_order(case):
        return (list(OPERATIONS).index(case[0]) if case[0] in OPERATIONS else 99, case)

    def fixture_title(fixture):
        n, k, h, working_set, dirty = fixture
        width = manifest.get('config', {}).get('payload_bytes')
        size = f"; {number(n*(width+16)/1e9, 'logical GB')}" if width is not None else ''
        pending = 'clean working set' if working_set == 'clean' else f'{working_set} dirty working set: {dirty:,} rows'
        return f"{n:,} rows{size} · K={k:,} historical changes · {h} commits · {pending}"

    def fixture_order(item):
        fixture = item[0]
        labels = ['clean', 'diff', 'xsmall', 'small', 'medium', 'large']
        profile = fixture[3]
        return (*fixture[:3], labels.index(profile) if profile in labels else 5, profile, fixture[4])

    def comparisons(title, note, mode, formatter):
        doc.heading(2, title)
        doc.paragraph(note)
        found = False
        for fixture, cases in sorted(fixtures.items(), key=fixture_order):
            selected = sorted((case for pass_, case in cases if pass_ == mode), key=case_order)
            if not selected:
                continue
            found = True
            doc.heading(3, fixture_title(fixture))
            rows = []
            for case in selected:
                engines = cases[(mode, case)]
                rows.append([OPERATIONS.get(case[0], case[0]), case[1],
                             *[formatter(engines[e]) if e in engines else '—' for e in ENGINES]])
            doc.table(['Operation', 'Distance (commits)', *ENGINE_NAMES], rows)
        if not found:
            doc.paragraph("This pass was not measured.")

    cell_fields = ('engine', 'operation', 'rows', 'changed_rows', 'history_commits',
                   'working_set', 'dirty_rows', 'distance', 'pass')
    drifting = {tuple(f.get(k) for k in cell_fields) + (f['metric'],): f for f in drift}

    def drift_mark(row, metric):
        flag = drifting.get(tuple(row.get(k) for k in cell_fields) + (metric,))
        return f" [drift {'↑' if flag['direction'] == 'rising' else '↓'}]" if flag else ''

    comparisons('Latency',
                'Each cell: median milliseconds [minimum–maximum]; n = measured repetitions. '
                'The range describes observed variation, not a confidence interval. Distance 0 is the current root '
                '(checkout switches branches with the same root). [drift ↑] or [drift ↓] marks a cell whose '
                'repetitions rise or fall with run order (see Run-order drift).',
                'timing', lambda r: f"{number(r['median_ms'])} ms [{number(r['min_ms'])}–{number(r['max_ms'])}]; "
                                    f"n={r['repetitions']}{drift_mark(r, 'elapsed_ns')}")

    doc.heading(2, 'Run-order drift')
    doc.paragraph('A series drifts when its samples, taken in run order, rise or fall together more than chance '
                  'allows (two-sided Mann-Kendall test, p < 0.001, at least five samples): a cell\'s latency '
                  'repetitions, or one engine counter over its counters-pass repetitions. A cell\'s repetitions '
                  'are spread across the run, between other cells, so drift means the cost depends on state left '
                  'by what ran before (dead entries piling up, caches filling), not only on the operation '
                  'measured. A drifting counter shows that state directly: the same operation should do the '
                  'same work.')
    if drift:
        rows = []
        for f in drift:
            cell = (f"N={f['rows']:,}, K={f['changed_rows']:,}, D={f['distance']}, {f['working_set']}"
                    + (f" ({f['dirty_rows']:,} rows)" if f['dirty_rows'] else ''))
            if f['metric'] == 'elapsed_ns':
                series, change = 'latency', f"{number(f['first']/1e6)} → {number(f['last']/1e6)} ms"
            else:
                series, change = f['metric'][len('counter_'):], f"{f['first']:,} → {f['last']:,}"
            rows.append([OPERATIONS.get(f['operation'], f['operation']),
                         dict(zip(ENGINES, ENGINE_NAMES)).get(f['engine'], f['engine']), cell, series,
                         f"{f['direction']}; τ {f['kendall_tau']:.2f}, p {f['p_value']:.2g}, n={f['samples']}",
                         change])
        doc.table(['Operation', 'Engine', 'Cell', 'Series', 'Trend', 'First → last'], rows)
    else:
        doc.paragraph('No series drifts.')
    comparisons('Memory growth',
                'Each cell: peak growth / immediately retained growth, in KiB of server-tree PSS. '
                'PSS apportions shared pages. Negative retained growth means resident memory decreased. '
                '[short x/n] marks observations too short for the sampling interval; these peak values can miss '
                'transient memory. All sampled peaks are lower bounds. This measures resident growth, not total allocation volume.',
                'memory', lambda r: memory_cell(r, ('pss_bytes_peak_delta', 'pss_bytes_retained_delta'), 'KiB', 1024))
    comparisons('Memory footprint',
                'Each cell: baseline / sampled peak, in MiB of total server-tree PSS. '
                'This includes the engine’s existing resident memory. [short x/n] has the same meaning as above.',
                'memory', lambda r: memory_cell(r, ('pss_bytes_baseline', 'pss_bytes_peak'), 'MiB', 1024**2))

    doc.heading(2, 'CPU, I/O, and additional memory')
    doc.paragraph('These counters cover the whole server tree, including asynchronous activity. CPU time and faults '
                  'come from the cpu pass and read/write bytes from the io pass: each from one snapshot before the '
                  'operation and one after, nothing sampled in between. (Results recorded before those passes took '
                  'them from the memory pass.) Counter deltas are unavailable when the process set changes. CPU has '
                  'kernel clock-tick resolution. I/O is kernel process read/write bytes, not all buffered traffic. '
                  'RSS double-counts shared mappings; use PSS for total footprint comparisons. Short-observation '
                  'flags above also apply to RSS and private-memory peaks. [short x/n] on CPU time marks '
                  'operations shorter than three clock ticks (usually 30 ms): their CPU time mostly reads as 0 '
                  'or one tick. Write bytes are not like-for-like across engines: PostgreSQL writes most data '
                  'through shared buffers, reaching the kernel later (eviction, the background writer, the '
                  'checkpointer), often outside this operation\'s window or in a later one\'s; the Go engines '
                  'mostly write within the request.')
    metrics = [('RSS sampled peak', 'rss_bytes_peak', 'MiB', 1024**2, 'memory'),
               ('Private resident peak growth', 'private_bytes_peak_delta', 'KiB', 1024, 'memory'),
               ('CPU time', 'cpu_seconds_delta', 'ms', .001, 'cpu'),
               ('Minor faults', 'minor_faults_delta', '', 1, 'cpu'),
               ('Major faults', 'major_faults_delta', '', 1, 'cpu'),
               ('Read bytes', 'read_bytes_delta', 'KiB', 1024, 'io'),
               ('Write bytes', 'write_bytes_delta', 'KiB', 1024, 'io')]

    def measured_in(cases, case, metric, pass_):
        """The engines' rows measuring metric for case: its own pass's, else
        (results from before that pass) the memory pass's if it recorded it."""
        for mode in (pass_, 'memory'):
            engines = cases.get((mode, case))
            if engines and any(('median_' + metric) in r for r in engines.values()):
                return engines
        return None

    has_resources = False
    for fixture, cases in sorted(fixtures.items(), key=fixture_order):
        selected = sorted({case for pass_, case in cases if pass_ in ('memory', 'cpu', 'io')}, key=case_order)
        rows = []
        for case in selected:
            for label, metric, unit, scale, pass_ in metrics:
                engines = measured_in(cases, case, metric, pass_)
                if engines is None:
                    continue
                rows.append([OPERATIONS.get(case[0], case[0]), case[1], label,
                             *[resource_cell(engines[e], metric, unit, scale)
                               if e in engines else '—' for e in ENGINES]])
        if not rows:
            continue
        has_resources = True
        doc.heading(3, fixture_title(fixture))
        doc.table(['Operation', 'Distance', 'Metric', *ENGINE_NAMES], rows)
    if not has_resources:
        doc.paragraph('No memory, cpu or io pass was measured.')

    doc.heading(2, 'Engine work counters')
    doc.paragraph('Each cell: median [minimum–maximum] growth of the engine\'s own work counters across the '
                  'operation, from the counters pass (PostgreSQL prolly: pg_stat_prolly, read before and after '
                  'it, outside timing; Dolt and Doltgres keep none). Counts, not timings: what the operation did '
                  '(tree nodes read, change-log pages read, rows a fold read, chunks appended, pages copied), '
                  'which explains its latency and repeats from run to run. A counter the operation never moved '
                  'is left out; one it moved in only some repetitions has 0 as its minimum.')

    def counter_cell(row, name):
        if 'median_counter_' + name not in row:
            return '0'
        lo, hi = row['min_counter_' + name], row['max_counter_' + name]
        return (count(row['median_counter_' + name]) + (f" [{count(lo)}–{count(hi)}]" if lo != hi else '')
                + drift_mark(row, 'counter_' + name))

    has_counters = False
    for fixture, cases in sorted(fixtures.items(), key=fixture_order):
        rows = []
        for case in sorted((case for pass_, case in cases if pass_ == 'counters'), key=case_order):
            engines = cases[('counters', case)]
            names = sorted({k[len('median_counter_'):] for r in engines.values() for k in r
                            if k.startswith('median_counter_')})
            for name in names:
                rows.append([OPERATIONS.get(case[0], case[0]), case[1], name,
                             *[counter_cell(engines[e], name) if e in engines else '—' for e in ENGINES]])
        if not rows:
            continue
        has_counters = True
        doc.heading(3, fixture_title(fixture))
        doc.table(['Operation', 'Distance', 'Counter', *ENGINE_NAMES], rows)
    if not has_counters:
        doc.paragraph('No counters pass was measured, or no operation moved a counter.')

    doc.heading(2, 'Empirical scaling')
    doc.paragraph('Each cell: fitted exponent [95% bootstrap interval]; R² in log space. '
                  'The model is cost ∝ X^exponent. Values near 0 suggest flat growth over the tested range; '
                  'near 1 suggest linear growth. This is not proof of Big-O. Intervals cover within-run variability only. '
                  'Each row holds the other dimensions fixed. An insufficient sweep needs at least four levels spanning '
                  '8× with five repetitions each. Sampling too slow means a short-operation flag prevents a memory fit. '
                  'Zero/nonpositive observations cannot be fitted in log space.')
    axes = {'rows': 'Table size N', 'changed_rows': 'Historical changed rows K',
            'distance': 'History distance D', 'dirty_rows': 'Pending dirty rows Q'}
    names = {'elapsed_ns': 'Latency', 'pss_bytes_peak_delta': 'PSS peak growth', 'pss_bytes_peak': 'PSS total peak'}
    for axis, title in axes.items():
        fixed = [field for field in ('rows', 'changed_rows', 'history_commits', 'distance', 'working_set', 'dirty_rows')
                 if field != axis and not (axis == 'dirty_rows' and field == 'working_set')
                 and not (axis == 'rows' and field == 'dirty_rows')]
        grouped = defaultdict(dict)
        for item in fits:
            if item['axis'] == axis:
                key = (item['metric'], item['operation'],
                       *(item.get(f, 'clean' if f == 'working_set' else 0) for f in fixed))
                grouped[key][item['engine']] = item
        if not grouped:
            continue
        doc.heading(3, 'Varying ' + title)
        rows = []
        labels = {'rows': 'N', 'changed_rows': 'K', 'history_commits': 'history commits', 'distance': 'D',
                  'working_set': 'working set', 'dirty_rows': 'Q'}
        for key, engines in sorted(grouped.items()):
            metric, op, *values = key
            context = ', '.join(f'{labels[f]}={value:,}' if isinstance(value, int) else f'{labels[f]}={value}'
                                for f, value in zip(fixed, values))
            rows.append([OPERATIONS.get(op, op), names[metric], context,
                         *[fit_cell(engines[e]) if e in engines else '—' for e in ENGINES]])
        doc.table(['Operation', 'Measured cost', 'Held fixed', *ENGINE_NAMES], rows)
    if not fits:
        doc.paragraph('No scaling observations are available.')
    doc.save(path)
