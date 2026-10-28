"""Deterministic fixtures and checked, repeatable version-control operations."""
import datetime
from decimal import Decimal
import hashlib
import json
from pathlib import Path
import platform
import random
import shutil
import time
import uuid

from core import (ENGINES, SQL, default_dirty_sizes, dirty_rows, disk_size, engine_passes, log, server, sha256,
                  trace, write_json)
from memory import Sampler
import window

def changed_ids(n, k):
    return [1 + i*n//k for i in range(k)]


def counter_deltas(before, after):
    """Each counter's growth across an operation: the counters it moved
    (a counter it didn't is 0; one that moved backwards was reset meanwhile,
    which fails the sample rather than reporting a negative amount of work)."""
    if set(before) != set(after):
        raise RuntimeError("the engine's counters changed names during an operation")
    deltas = {name: after[name] - before[name] for name in after}
    if any(d < 0 for d in deltas.values()):
        raise RuntimeError("an engine counter went backwards during an operation (reset?)")
    return {name: d for name, d in sorted(deltas.items()) if d}


def update(sql, n, k, revision):
    """Set revision on the k rows changed_ids(n, k) names, in one
    transaction (the engine's statements: dirty_update_sql()), so its
    commit flushes the log once, not once for each statement."""
    statements = sql.dirty_update_sql(n, k, revision)
    sql.query("BEGIN")
    for statement in statements:
        sql.query(statement)
    sql.query("COMMIT")


def expect(actual, expected, what):
    if actual != expected:
        raise RuntimeError(f"{what}: expected {expected!r}, got {actual!r}")


def integer(value):
    # MySQL-compatible SUM can arrive as scientific notation through database/sql.
    number = Decimal(value)
    if not number.is_finite() or number != number.to_integral_value():
        raise ValueError(f"Expected an exact integer, got {value}")
    return int(number)


def probe_ids(n, k):
    """Three of the changed rows: the first, middle and last."""
    ids = changed_ids(n, k)
    return sorted({ids[0], ids[len(ids)//2], ids[-1]})


def side_ids(n, k):
    """Rows no fixture commit changes, beside three changed ones: what a
    three-way merge's other side changes."""
    changed = set(changed_ids(n, k))
    ids = sorted({i + 1 for i in probe_ids(n, k) if i + 1 <= n and i + 1 not in changed})
    if not ids:
        raise ValueError(f"a three-way merge needs rows no commit changes; K={k} changes all {n}")
    return ids


def probe(sql, n, k, revision):
    picks = probe_ids(n, k)
    rows = sql.query("SELECT id,revision FROM bench_rows WHERE " +
                     sql.ids_in(picks) + " ORDER BY id")["rows"]
    expect(rows, [[str(i), str(revision)] for i in picks], "working rows")


def fixture_path(root, engine, n):
    return root / engine / f"bench_n{n}.json"


def signature(c, n):
    return {"rows": n, "payload_bytes": c["payload_bytes"], "generator": "sha256-id-block-base64-v1"}


def prepare(root, c, engines):
    for engine in engines:
        with server(root, c, engine):
            for n in c["row_counts"]:
                db = f"bench_n{n}"
                path = fixture_path(root, engine, n)
                state = json.loads(path.read_text()) if path.exists() else {"signature": signature(c, n), "fixtures": {}}
                expect(state["signature"], signature(c, n), "fixture configuration")
                with SQL(root, c, engine) as admin:
                    databases = {row[0] for row in admin.query(ENGINES[engine].databases_sql)["rows"]}
                    if db in databases and not path.exists():
                        raise RuntimeError(f"Refusing to use unmanaged database {db}")
                    write_json(path, state)
                    if db not in databases:
                        admin.query(f"CREATE DATABASE {db}")
                    # (before any history is built: a root let go of, its
                    # commit outside the window, isn't kept again)
                    ENGINES[engine].apply_window(admin, db, c)
                    settings = ENGINES[engine].window_settings(c)
                    if state.get("window_settings", settings) != settings:
                        raise RuntimeError(f"{engine}/{db}: its history was built under other window settings "
                                           f"({state['window_settings']}); prepare into a new root")
                    state["window_settings"] = settings
                    write_json(path, state)
                with SQL(root, c, engine, db) as sql:
                    if "base" not in state:
                        ENGINES[engine].create_table(sql, "CREATE TABLE IF NOT EXISTS bench_rows (id BIGINT PRIMARY KEY, "
                                                          "revision BIGINT NOT NULL, payload TEXT NOT NULL)")
                        count, low, high = map(int, sql.query(
                            "SELECT count(*),coalesce(min(id),0),coalesce(max(id),0) FROM bench_rows")["rows"][0])
                        if count != high or low != (1 if count else 0) or count > n:
                            raise RuntimeError("Partial load is not a contiguous prefix; inspect before resuming")
                        log(f"{engine}/{db}: loading {n:,} rows ({n*(c['payload_bytes']+16)/1e9:.3f} logical GB), resuming at {count:,}")
                        last_log = time.monotonic()
                        for start in range(count+1, n+1, c["batch_rows"]):
                            sql.request(start=start, count=min(c["batch_rows"], n-start+1), width=c["payload_bytes"])
                            if time.monotonic() - last_log >= 15:
                                log(f"{engine}/{db}: {min(n, start+c['batch_rows']-1):,}/{n:,} rows")
                                last_log = time.monotonic()
                        # Scan exactly once, outside measurements; catches missing rows,
                        # width differences, and a resume against a modified working set.
                        check = list(map(integer, sql.query("SELECT count(*),sum(octet_length(payload)),sum(revision) FROM bench_rows")["rows"][0]))
                        expect(check, [n, n*c["payload_bytes"], 0], "loaded table")
                        # Verify generated contents at reproducible positions, too.
                        for i in sorted({1, max(1, n//2), n}):
                            raw = b"".join(hashlib.sha256(i.to_bytes(8, 'little') + b.to_bytes(8, 'little')).digest()
                                           for b in range(c["payload_bytes"]//32 + 2))
                            import base64
                            expected = base64.b64encode(raw).decode()[:c["payload_bytes"]]
                            expect(sql.scalar(f"SELECT payload FROM bench_rows WHERE id={i}"), expected, "payload probe")
                        sql.vc("commit", "-Am", "vc_bench baseline", "--allow-empty", "--author", "VC Bench <vc-bench@example.invalid>")
                        state["base"] = sql.hash()
                        state["logical_bytes"] = n*(c["payload_bytes"]+16)
                        state["verified_rows"] = n
                        write_json(path, state)
                    for k in c["changes"]:
                        h = max(c["history_depths"])
                        key = f"k{k}_h{h}"
                        if state["fixtures"].get(key, {}).get("ready"):
                            continue
                        f = state["fixtures"].get(key)
                        branch = "fixture_" + key
                        if f is None:
                            sql.vc("branch", branch, state["base"])
                            f = {"branch": branch, "commits": [state["base"]], "ready": False}
                            state["fixtures"][key] = f
                            write_json(path, state)
                        sql.vc("checkout", branch)
                        expect(sql.hash(), f["commits"][-1], "resumed fixture HEAD (a commit may have completed before manifest persistence)")
                        log(f"{engine}/{db}: history with {k:,} changed rows, {h} commits")
                        for revision in range(len(f["commits"]), h+1):
                            update(sql, n, k, revision)
                            sql.vc("commit", "-Am", f"vc_bench {key} revision {revision}", "--author", "VC Bench <vc-bench@example.invalid>")
                            f["commits"].append(sql.hash())
                            write_json(path, state)
                        probe(sql, n, k, h)
                        expect(int(sql.scalar(sql.diff_sql(state["base"], "HEAD"))), k, "history diff cardinality")
                        f["ready"] = True
                        write_json(path, state)
                    state["server_data_size_after_prepare"] = disk_size(root / engine / "data")
                    write_json(path, state)


def cases(c):
    result = [{"operation": "round_trip", "distance": 0},
              {"operation": "branch", "distance": 0},
              {"operation": "checkout", "distance": 0},
              {"operation": "reset_soft", "distance": 0},
              {"operation": "reset_hard", "distance": 0},
              {"operation": "diff_equal", "distance": 0},
              {"operation": "diff_working", "distance": 0}]
    for d in sorted(set(c["history_depths"])):
        for op in ("branch", "checkout", "reset_soft", "reset_hard", "diff_history",
                   "diff_working_history", "resolve_ancestor"):
            result.append({"operation": op, "distance": d})
    for d in [0, *sorted(set(c["history_depths"]))]:
        result.append({"operation": "at_history", "distance": d})
    # (merges from a base D back: a fast-forward of the scratch branch,
    # reset to the base, to HEAD; and a three-way merge of HEAD into a
    # branch made at the base, with a commit of its own, deleted after so
    # its commits keep no roots)
    for d in sorted(set(c["history_depths"])):
        for op in ("merge_ff", "merge"):
            result.append({"operation": op, "distance": d})
    for case in result:
        case['working_set'] = 'diff' if case['operation'] in ('diff_working', 'diff_working_history') else 'clean'
    for d in [0, *sorted(set(c["history_depths"]))]:
        for op in ("branch_delete", "branch_delete_used"):
            result.append({"operation": op, "distance": d, "working_set": "clean"})
    result.append({"operation": "drop_table", "distance": 0, "working_set": "clean"})
    for label in c.get('dirty_sizes', default_dirty_sizes()):
        for d in [0, *sorted(set(c['history_depths']))]:
            for op in ('branch', 'checkout', 'reset_soft', 'reset_hard'):
                result.append({'operation': op, 'distance': d, 'working_set': label})
        result.append({'operation': 'drop_table', 'distance': 0, 'working_set': label})
    return result


def run_cell(root, c, engine, n, k, sql, pid, fixture, output, rng):
    h = len(fixture["commits"])-1
    latest = fixture["commits"][-1]
    token = uuid.uuid4().hex[:12]
    scratch = "run_" + token
    tmpbranch = "new_" + token
    targets = {}
    sql.vc("branch", scratch, latest)
    sql.vc("checkout", scratch)
    try:
        for d in [0, *sorted(set(c["history_depths"]))]:
            name = f"target_{token}_d{d}"
            sql.vc("branch", name, fixture["commits"][h-d])
            targets[d] = name
        for mode in engine_passes(c, engine):
            for rep in range(-c["warmups"], c["repetitions"]):
                order = cases(c)
                rng.shuffle(order)
                for number, case in enumerate(order, 1):
                    op, d = case["operation"], case["distance"]
                    label = f"{op} D={d} {case['working_set']}"
                    trace(f"==== CASE {number}/{len(order)} {label}: setup")
                    started = time.perf_counter()
                    timed_at = restored_at = None
                    old = fixture["commits"][h-d]
                    # (asked first: the target in the window, kept with a
                    # root, or measured apart; never a slow sample)
                    target = sql.target(fixture["commits"], h-d, c)
                    profile = case['working_set']
                    dirty = profile != 'clean'
                    count = (k if profile == 'diff' else
                             dirty_rows(c.get('dirty_sizes', default_dirty_sizes())[profile], n) if dirty else 0)
                    if dirty:
                        trace(f"---- setup: dirty write ({count:,} rows)")
                        update(sql, n, count, h+1)
                    if op == "merge_ff":
                        sql.vc("reset", "--hard", old)
                        sql.before_merge()
                    elif op == "merge":
                        # (the other side's commit: rows no fixture commit
                        # changes, at a revision none has)
                        sql.vc("branch", tmpbranch, old)
                        sql.vc("checkout", tmpbranch)
                        sql.query(f"UPDATE bench_rows SET revision={h+2} WHERE " + sql.ids_in(side_ids(n, k)))
                        sql.vc("commit", "-Am", "vc_bench merge side", "--author", ENGINES[engine].AUTHOR)
                        sql.before_merge()
                    if op.startswith("branch_delete"):
                        # A branch at the old commit; the used one checked out
                        # once first, so the engine has made what a session on
                        # it needs (prolly: its copies of the tables).
                        sql.vc("branch", tmpbranch, old)
                        if op == "branch_delete_used":
                            sql.vc("checkout", tmpbranch)
                            sql.query("SELECT id,revision FROM bench_rows WHERE " +
                                      sql.ids_in(probe_ids(n, k)) + " ORDER BY id")
                            sql.vc("checkout", scratch)
                    if op == "round_trip":
                        query = "SELECT 1"
                    elif op.startswith("branch_delete"):
                        query = sql.vc_sql("branch", "-D", tmpbranch)
                    elif op == "drop_table":
                        query = "DROP TABLE bench_rows"
                    elif op == "branch":
                        query = sql.vc_sql("branch", tmpbranch, old)
                    elif op == "checkout":
                        query = sql.vc_sql("checkout", targets[d])
                    elif op == "reset_soft":
                        query = sql.vc_sql("reset", "--soft", old)
                    elif op == "reset_hard":
                        query = sql.vc_sql("reset", "--hard", old)
                    elif op == "resolve_ancestor":
                        query = sql.hash_sql(f"HEAD~{d}")
                    elif op == "at_history":
                        query = sql.at_sql(old, probe_ids(n, k))
                    elif op == "merge_ff":
                        query = sql.merge_sql(fixture["branch"], "vc_bench merge")
                    elif op == "merge":
                        query = sql.merge_sql(fixture["branch"], "vc_bench merge")
                    elif op == "diff_equal":
                        query = sql.diff_sql(latest, latest)
                    elif op == "diff_working":
                        query = sql.diff_sql("HEAD", "WORKING")
                    elif op == "diff_history":
                        query = sql.diff_sql(old, latest)
                    else:
                        query = sql.diff_sql(old, "WORKING")
                    record = {"engine": engine, "rows": n, "logical_bytes": n*(c["payload_bytes"]+16),
                              "changed_rows": k, "history_commits": h, **case,
                              "dirty_rows": count, "dirty_row_logical_bytes": count*(c['payload_bytes']+16),
                              "pass": mode, "repetition": rep, "warmup": rep < 0,
                              "window": target["window"], "target_root": target["root"],
                              "root_distance": target["root_distance"],
                              "sql": query, "status": "started"}
                    # (only the memory pass samples while the operation runs)
                    sampler = (Sampler(pid, c["sample_interval_ms"] if mode == "memory" else None)
                               if mode in ("memory", "cpu", "io") and rep >= 0 else None)
                    result = {}
                    try:
                        # (read after the setup, so its work isn't the operation's,
                        # and again before the restore, whose isn't either)
                        before = sql.counters() if mode == "counters" else None
                        # (the counters pass's read has them already)
                        path_before = sql.path_counters() if mode != "counters" else None
                        if sampler:
                            sampler.start()
                        trace("---- timed")
                        timed_at = time.perf_counter()
                        try:
                            result = sql.query(query)
                        finally:
                            restored_at = time.perf_counter()
                            trace("---- restore")
                            if sampler:
                                record["memory"] = sampler.finish(result.get("elapsed_ns", 0))
                        record["elapsed_ns"] = result["elapsed_ns"]
                        if mode == "counters":
                            record["counters"] = counter_deltas(before, sql.counters())
                            if before is not None and path_before is None and \
                                    set(window.PATH_COUNTERS) <= set(before):
                                record["path"] = {name: record["counters"].get(name, 0)
                                                  for name in window.PATH_COUNTERS}
                        if path_before is not None:
                            after = sql.path_counters()
                            record["path"] = {name: after[name] - path_before[name] for name in window.PATH_COUNTERS}
                        # (a merge changes the rows between its base and the
                        # other side: the history's K, or the side's rows)
                        bound = {"merge_ff": 0, "merge": k}.get(op)
                        window.check_path(op, d, target, record.get("path", {}), bound)
                        # The restore, outside timing and memory sampling: the
                        # session back on the scratch branch, at the fixture's
                        # head with a clean working set, and the case's
                        # branches deleted, so every case starts from the same
                        # state.  (The run measures timing; it checks nothing
                        # about the operations' results.)
                        if op == "branch":
                            sql.vc("branch", "-D", tmpbranch)
                        elif op == "checkout":
                            sql.vc("checkout", scratch)
                        elif op == "merge":
                            sql.vc("checkout", scratch)
                            sql.vc("branch", "-D", tmpbranch)
                        elif op.startswith("diff_"):
                            record["diff_rows"] = int(result["rows"][0][0])
                        if dirty or op in ("reset_soft", "reset_hard", "drop_table", "merge_ff"):
                            sql.vc("reset", "--hard", latest)
                        record["status"] = "ok"
                    except Exception as e:
                        record["status"], record["error"] = "failed", str(e)
                        raise
                    finally:
                        # (the harness's own work around the timed statement:
                        # its setup and its restore)
                        ended = time.perf_counter()
                        record["setup_sec"] = round((timed_at or ended) - started, 3)
                        if restored_at is not None:
                            record["restore_sec"] = round(ended - restored_at, 3)
                        output.write(json.dumps(record) + "\n")
                        output.flush()
                        summary = (f"case {number}/{len(order)} {label}: {record['status']}; "
                                   f"setup {record['setup_sec']:.1f} s, timed "
                                   + (f"{record['elapsed_ns'] / 1e6:.1f} ms" if "elapsed_ns" in record else "-")
                                   + (f", restore {record['restore_sec']:.1f} s"
                                      if "restore_sec" in record else ""))
                        trace(f"==== {summary}")
                        log(f"{engine}/n={n:,}/k={k:,} {mode}: {summary}")
                log(f"{engine}/n={n}/k={k}: {mode} {'warmup' if rep < 0 else 'repetition '+str(rep+1)} complete")
    finally:
        # On an error preserve scratch branches for inspection; on success remove
        # all temporary refs so branch-count growth cannot bias later cells.
        import sys
        if sys.exc_info()[0] is None:
            sql.vc("checkout", fixture["branch"])
            for name in [scratch, *targets.values()]:
                sql.vc("branch", "-D", name)


def planned_samples(c, engines):
    """Measured samples a run makes: each engine's passes (engine_passes())
    of every case, size and change count."""
    return sum(len(engine_passes(c, e)) for e in engines) * \
        len(c['row_counts'])*len(c['changes'])*len(cases(c))*c['repetitions']


def run(root, c, engines):
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ") + "-" + uuid.uuid4().hex[:6]
    out = root / "results" / stamp
    out.mkdir(parents=True)
    rng = random.Random(c["seed"])
    engine_order = list(engines)
    rng.shuffle(engine_order)
    manifest = {"status": "running", "config": c, "engine_order": engine_order,
                "platform": platform.platform(), "cpu_count": __import__('os').cpu_count(),
                "memory_info": Path('/proc/meminfo').read_text(), "binaries": {},
                "planned_samples": planned_samples(c, engines),
                "method": "persistent SQL; warm caches; one engine at a time; memory, CPU and I/O of the server process tree in passes of their own"}
    write_json(out / "manifest.json", manifest)
    try:
        for engine in engine_order:
            binary = Path(c["engines"][engine]["binary"])
            manifest["binaries"][engine] = {"path": str(binary), "sha256": sha256(binary)}
            provenance = root / "build" / (engine + ".json")
            if provenance.exists():
                attestation = json.loads(provenance.read_text())
                expect(attestation["sha256"], manifest["binaries"][engine]["sha256"], "build binary hash")
                manifest["binaries"][engine]["build"] = attestation
            else:
                manifest["binaries"][engine]["provenance"] = "external binary; source revision unverified"
            write_json(out / "manifest.json", manifest)
            with server(root, c, engine) as pid:
                with (out / "samples.jsonl").open("a") as output:
                    ns = list(c["row_counts"])
                    rng.shuffle(ns)
                    for n in ns:
                        state = json.loads(fixture_path(root, engine, n).read_text())
                        expect(state["signature"], signature(c, n), "fixture configuration")
                        write_json(out / f"{engine}-n{n}-fixture.json", state)
                        with SQL(root, c, engine) as admin:
                            ENGINES[engine].apply_window(admin, f"bench_n{n}", c)
                        with SQL(root, c, engine, f"bench_n{n}") as sql:
                            settings = ENGINES[engine].settings(sql)
                            if settings is not None:
                                write_json(out / f"{engine}-settings.json", settings)
                            for k in c["changes"]:
                                f = state["fixtures"][f"k{k}_h{max(c['history_depths'])}"]
                                if not f.get("ready"):
                                    raise ValueError("Fixture is incomplete; run prepare")
                                log(f"Measuring {engine}/n={n:,}/k={k:,}")
                                windows = manifest.setdefault("windows", {}).setdefault(engine, {})
                                cell = windows.setdefault(f"n{n}_k{k}", {})
                                cell["before"] = sql.window_state([f["branch"]])
                                write_json(out / "manifest.json", manifest)
                                run_cell(root, c, engine, n, k, sql, pid, f, output, rng)
                                cell["after"] = sql.window_state([f["branch"]])
                                write_json(out / "manifest.json", manifest)
            shutil.copyfile(root / engine / "server.log", out / (engine + "-server.log"))
            manifest["binaries"][engine]["data_directory_size"] = disk_size(root / engine / "data")
        manifest["status"] = "complete"
    except BaseException as e:
        manifest["status"], manifest["error"] = "failed", str(e)
        raise
    finally:
        manifest["driver_sha256"] = sha256(root / "bin/sql-driver")
        manifest["harness_sha256"] = {p.name: sha256(p) for p in Path(__file__).parent.glob('*.py')}
        write_json(out / "manifest.json", manifest)
        log(f"Results: {out}")
    return out
