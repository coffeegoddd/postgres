#!/usr/bin/env python3
"""Summarize winbench output files: one line per run.

usage: summarize.py results/*.csv
"""
import re
import statistics
import sys


def kv(fields):
    out = {}
    for f in fields:
        if "=" in f:
            k, v = f.split("=", 1)
            try:
                out[k] = float(v)
            except ValueError:
                out[k] = v
    return out


def med(xs):
    return statistics.median(xs) if xs else float("nan")


def summarize(path):
    commits, images, sqls = [], [], []
    db_pages = None
    done = ""
    initial = ""
    for line in open(path):
        line = line.strip()
        if line.startswith("commit,"):
            commits.append(kv(line.split(",")))
        elif line.startswith("checkout,image"):
            d = kv(line.split(","))
            images.append(d)
            db_pages = d.get("db_pages", db_pages)
        elif line.startswith("checkout,sql"):
            sqls.append(kv(line.split(",")))
        elif line.startswith("# done"):
            done = line
        elif line.startswith("# initial image"):
            initial = line
    if not commits:
        return f"{path}: no data"
    m = re.search(r"store live (\d+) MB, file (\d+) MB, db (\d+) pages \((\d+) MB\)", done)
    live, file_mb, pages, db_mb = (m.groups() if m else ("?",) * 4)
    moving = [i for i in images if i["dist"] != 0 or i.get("pages", 0) > 0]
    return (
        f"{path}\n"
        f"  db {db_mb} MB ({pages} pages); store live {live} MB, file {file_mb} MB; {initial[2:]}\n"
        f"  commit (median of {len(commits)}): rows {med([c['rows'] for c in commits]):.0f}, "
        f"pages {med([c['pages'] for c in commits]):.0f}, image +{med([c['img_new_mb'] for c in commits]):.1f} MB, "
        f"prolly {med([c['prolly_ms'] for c in commits]):.0f} ms, image {med([c['image_ms'] for c in commits]):.0f} ms, "
        f"checkpoint {med([c['checkpoint_ms'] for c in commits]):.0f} ms\n"
        f"  checkout by image (median of {len(moving)}): pages {med([i['pages'] for i in moving]):.0f} "
        f"(local {med([i.get('dirty', 0) for i in moving]):.0f} + diff {med([i.get('diff', 0) for i in moving]):.0f}), "
        f"{med([i['ms'] for i in moving]):.0f} ms, {med([i['ms'] / max(i['pages'], 1) for i in moving]) * 1000:.0f} us/page\n"
        f"  checkout by SQL  (median of {len(sqls)}): rows {med([s['rows'] for s in sqls]):.0f}, "
        f"{med([s['ms'] for s in sqls]):.0f} ms\n"
        f"  image pages per path commit: {by_path(moving)}"
    )


def by_path(images):
    groups = {}
    for i in images:
        p = int(i.get("path", -1))
        if p > 0:
            groups.setdefault(p, []).append(i["pages"])
    return ", ".join(f"path {p}: {med(v):.0f}" for p, v in sorted(groups.items()))


for p in sys.argv[1:]:
    print(summarize(p))
