"""Format-independent report tables and standalone HTML/Markdown renderers.

Analysis supplies plain text and typed links; neither renderer interprets
Markdown or trusts HTML from benchmark output. Reports are derived artifacts.
"""
from __future__ import annotations

import html
import os
import re
import stat
import tempfile
from contextlib import ExitStack
from dataclasses import dataclass, field
from pathlib import Path
from typing import Protocol
from urllib.parse import quote


@dataclass
class Value:
    text: str
    detail: str = ""
    tone: str = ""
    href: str = ""


@dataclass
class Section:
    title: str
    columns: list[str] = field(default_factory=list)
    rows: list[list[Value | str]] = field(default_factory=list)
    notes: list[str] = field(default_factory=list)
    folded: bool = False


@dataclass
class Document:
    title: str
    notes: list[str] = field(default_factory=list)
    sections: list[Section] = field(default_factory=list)
    passed: bool = True


class Renderer(Protocol):
    def render(self, document: Document) -> str: ...


def link(path: Path, output_dir: Path, label: str) -> Value:
    return Value(label, href=quote(os.path.relpath(path.resolve(), output_dir.resolve()), safe="/"))


def _value(value: Value | str) -> Value:
    return value if isinstance(value, Value) else Value(str(value))


def _anchor(index: int, title: str) -> str:
    return f"s{index}-" + re.sub(r"[^a-z0-9]+", "-", title.lower()).strip("-")


_CSS = """
:root {color-scheme:light dark; --bg:#fff; --fg:#20252b; --line:#d9dfe5;
 --head:#eef2f6; --muted:#596574; --good:#e4f4e9; --bad:#fce8e8;}
@media(prefers-color-scheme:dark) { :root {--bg:#16191d; --fg:#edf0f4;
 --line:#3c4652; --head:#242c35; --muted:#b3bdc9; --good:#163c28; --bad:#492828;} }
* {box-sizing:border-box} body {margin:0; padding:24px; background:var(--bg);
 color:var(--fg); font:14px/1.5 system-ui,sans-serif;} main {max-width:1500px;margin:auto;}
h1 {font-size:26px;} h2,summary {font-size:19px; margin:24px 0 8px;}
a {color:inherit;text-underline-offset:3px;} a:focus,summary:focus {outline:3px solid #638ee8;}
nav {display:flex;flex-wrap:wrap;gap:8px 18px;margin:24px 0;}
.wrap {overflow:auto;max-height:75vh;border:1px solid var(--line);border-radius:6px;}
table {border-collapse:separate;border-spacing:0;width:100%;font-variant-numeric:tabular-nums;}
caption {text-align:left;padding:8px;font-weight:600;} th,td {padding:8px 12px;
 border-bottom:1px solid var(--line);text-align:right;vertical-align:top;white-space:pre-wrap;
 min-width:95px;max-width:560px;overflow-wrap:anywhere;}
th {background:var(--head);position:sticky;top:0;z-index:1;white-space:nowrap;} th:first-child,td:first-child {text-align:left;}
td.good {background:var(--good)} td.bad {background:var(--bad)}
.detail {display:block;color:var(--muted);font-size:12px;margin-top:4px;}
summary {cursor:pointer;} p {max-width:110ch;white-space:pre-wrap;overflow-wrap:anywhere;}
@media(max-width:600px) {body {padding:12px;} th,td {padding:6px;} h1 {font-size:22px;}
 nav {max-height:9rem;overflow:auto;border:1px solid var(--line);padding:8px;}}
@media print {.wrap {max-height:none;overflow:visible;} details {display:block;}
 th {position:static;} nav {display:none;}}
"""


class HtmlRenderer:
    def render(self, document: Document) -> str:
        esc = html.escape
        out = ["<!doctype html><html lang='en'><head><meta charset='utf-8'>",
               "<meta name='viewport' content='width=device-width, initial-scale=1'>",
               f"<title>{esc(document.title)}</title><style>{_CSS}</style></head><body><main>",
               f"<h1>{esc(document.title)}</h1>"]
        out += [f"<p>{esc(n)}</p>" for n in document.notes]
        out.append("<nav aria-label='Report sections'>" + "".join(
            f"<a href='#{_anchor(i, s.title)}'>{esc(s.title)}</a>"
            for i, s in enumerate(document.sections)) + "</nav>")
        for i, section in enumerate(document.sections):
            anchor = _anchor(i, section.title)
            out.append(f"<section id='{anchor}'>")
            out.append(f"<details><summary>{esc(section.title)}</summary>" if section.folded
                       else f"<h2>{esc(section.title)}</h2>")
            out += [f"<p>{esc(n)}</p>" for n in section.notes]
            if section.columns:
                out.append(f"<div class='wrap' tabindex='0' role='region' aria-label='{esc(section.title)}'>"
                           f"<table><caption>{esc(section.title)}</caption><thead><tr>")
                out += [f"<th scope='col'>{esc(c)}</th>" for c in section.columns]
                out.append("</tr></thead><tbody>")
                for row in section.rows:
                    out.append("<tr>")
                    for value in row:
                        v = _value(value)
                        text = esc(v.text)
                        if v.href:
                            text = f"<a href='{esc(v.href)}'>{text}</a>"
                        detail = f"<span class='detail'>{esc(v.detail)}</span>" if v.detail else ""
                        tone = v.tone if v.tone in {"good", "bad"} else ""
                        out.append(f"<td class='{tone}'>{text}{detail}</td>")
                    out.append("</tr>")
                out.append("</tbody></table></div>")
            if section.folded:
                out.append("</details>")
            out.append("</section>")
        return "".join(out) + "</main></body></html>\n"


def markdown_text(value: str) -> str:
    # Escape markup before inserting our own table breaks and typed links.
    value = html.escape(str(value), quote=False)
    value = re.sub(r"([\\`*_{\[\]}()#!|~])", r"\\\1", value)
    return value.replace("\r", "").replace("\n", "<br>")


class MarkdownRenderer:
    def render(self, document: Document) -> str:
        esc = markdown_text
        out = [f"# {esc(document.title)}", "", *[esc(n) + "\n" for n in document.notes]]
        for section in document.sections:
            out += [f"## {esc(section.title)}", "", *[esc(n) + "\n" for n in section.notes]]
            if not section.columns:
                continue
            out += ["| " + " | ".join(esc(c) for c in section.columns) + " |",
                    "|" + "---|" * len(section.columns)]
            for row in section.rows:
                cells = []
                for value in row:
                    v = _value(value)
                    text = esc(v.text)
                    if v.href:
                        text = f"[{text}]({v.href})"
                    if v.detail:
                        text += "<br>" + esc(v.detail)
                    cells.append(text)
                out.append("| " + " | ".join(cells) + " |")
            out.append("")
        return "\n".join(out) + "\n"


RENDERERS: dict[str, Renderer] = {"html": HtmlRenderer(), "markdown": MarkdownRenderer()}


def write(document: Document, stem: Path, formats: str = "both", *,
          markdown_path: Path | None = None) -> list[Path]:
    """Render everything before replacing any output; never modify evidence."""
    names = ["markdown", "html"] if formats == "both" else [formats]
    outputs = [(markdown_path if name == "markdown" and markdown_path else
                stem.with_name(stem.name + (".md" if name == "markdown" else ".html")),
                RENDERERS[name].render(document)) for name in names]
    temporary = []
    with ExitStack() as cleanup:
        for path, text in outputs:
            path.parent.mkdir(parents=True, exist_ok=True)
            # A private directory protects partial output while ordinary file
            # creation respects umask. Preserve an existing report's mode.
            directory = cleanup.enter_context(tempfile.TemporaryDirectory(dir=path.parent, prefix=f".{path.name}."))
            tmp = Path(directory) / path.name
            tmp.write_text(text, encoding="utf-8")
            if path.exists():
                tmp.chmod(stat.S_IMODE(path.stat().st_mode))
            temporary.append((tmp, path))
        for tmp, path in temporary:
            tmp.replace(path)
    return [p for p, _ in outputs]
