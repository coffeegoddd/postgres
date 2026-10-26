"""The engines the harness compares, each behind one interface.

The runner, the fixture loader and preflight never ask which engine they
have: each asks its Engine (ENGINES[name]) how to build, start, stop and
probe it, how to connect, and the SQL its version-control operations,
hashes, diffs and benchmark table take.  A new engine, or a setting that
must match across engines, is one class here.
"""
import json
import os
from pathlib import Path
import signal
import subprocess
import time


class Engine:
    name = None
    # (preflight: a file the source checkout must have)
    source_entry = None
    version_args = ("--version",)
    databases_sql = "SELECT datname FROM pg_database"

    def default_config(self, root, sources):
        """config.json's entry for the engine: its source, binary and port."""
        raise NotImplementedError

    # -- build --------------------------------------------------------------

    def build_command(self, root, c, jobs):
        """(command, working directory) that builds the configured binary."""
        raise NotImplementedError

    def preflight_build(self, checks, root, checkout, source_ok, tools_ok):
        """The build's prerequisites, as named checks (source_ok: its
        checkout passed; tools_ok: the common build tools are there)."""
        raise NotImplementedError

    def preflight_installed(self, binary, probe, require_executable):
        """Checks of an installed binary beyond its version probe."""

    # -- server -------------------------------------------------------------

    def start(self, root, c, home, binary):
        """Launch the server; returns (pid, command)."""
        raise NotImplementedError

    def stop(self, root, c, info, running):
        raise NotImplementedError

    # -- SQL ----------------------------------------------------------------

    def connection(self, port, db):
        """(sql-driver driver name, DSN)."""
        return ("pgx", f"postgres://postgres:password@127.0.0.1:{port}/{db or 'postgres'}"
                       "?sslmode=disable&default_query_exec_mode=simple_protocol")

    def vc_sql(self, op, args):
        return f"SELECT dolt_{op}({','.join(literal(a) for a in args)})"

    def hash_sql(self, ref):
        return f"SELECT hashof({literal(ref)})"

    def diff_sql(self, a, b):
        return f"SELECT count(*) FROM dolt_diff({literal(a)},{literal(b)},'bench_rows')"

    def ids_in(self, ids):
        """The predicate that the benchmark table's id is one of ids."""
        return "id IN (" + ",".join(str(int(i)) for i in ids) + ")"

    def table_count_sql(self):
        """How many benchmark tables the session's database has: 1, or 0
        once dropped."""
        return ("SELECT count(*) FROM information_schema.tables "
                "WHERE table_schema=current_schema() AND table_name='bench_rows'")

    def create_table(self, sql, ddl):
        """Make the benchmark table from its column DDL."""
        sql.query(ddl)

    def settings(self, sql):
        """Server settings worth recording with a run, or None."""
        return None

    # (the counters pass runs only for an engine that keeps work counters)
    has_counters = False

    def counters(self, sql):
        """The engine's cumulative work counters, {name: integer}, with all
        the session's work so far counted by the time they're read; the
        counters pass reports their differences across an operation.  None
        for an engine that keeps none."""
        return None


def literal(value):
    return "'" + str(value).replace("'", "''") + "'"


class GoEngine(Engine):
    """Dolt and Doltgres: one Go binary serving a YAML-configured data dir."""
    server_args = ()
    go_dir = "."
    go_target = None

    def build_command(self, root, c, jobs):
        e = c["engines"][self.name]
        return (["go", "build", "-trimpath", "-o", e["binary"], self.go_target],
                Path(e["source"]) / self.go_dir)

    def preflight_build(self, checks, root, checkout, source_ok, tools_ok):
        import preflight
        import core
        if not (source_ok and tools_ok):
            return
        directory = checkout / self.go_dir
        core.log(f'Checking {self.name} Go modules and C/C++/ICU toolchain')
        checks.test(f'{self.name} Go dependencies', lambda: preflight.go_dependencies(directory, self.go_target))
        checks.test(f'{self.name} C/C++ and ICU development libraries', lambda: preflight.go_compilers(directory))

    def server_config(self, root, c):
        """server.yaml.  Automatic GC is off: each engine reads
        behavior.auto_gc_behavior, and treats its absence as enabled."""
        home = root / self.name
        return (f"log_level: warning\nlistener:\n  host: 127.0.0.1\n  port: {c['engines'][self.name]['port']}\n"
                f"data_dir: {json.dumps(str(home / 'data'))}\ncfg_dir: {json.dumps(str(home / '.doltcfg'))}\n"
                "behavior:\n  auto_gc_behavior:\n    enable: false\n")

    def start(self, root, c, home, binary):
        (home / "data").mkdir(exist_ok=True)
        (home / "server.yaml").write_text(self.server_config(root, c))
        cmd = [str(binary), *self.server_args, str(home / "server.yaml")]
        with (home / "server.log").open("a") as f:
            p = subprocess.Popen(cmd, cwd=home, stdout=f, stderr=f, start_new_session=True)
        return p.pid, cmd

    def stop(self, root, c, info, running):
        os.kill(info["pid"], signal.SIGTERM)
        for _ in range(600):
            stat = Path(f"/proc/{info['pid']}/stat")
            if not running() or (stat.exists() and stat.read_text().rsplit(")", 1)[1].split()[0] == "Z"):
                break
            time.sleep(.1)
        else:
            raise RuntimeError(f"{self.name} did not exit; retained server.json for inspection")
        try:
            os.waitpid(info["pid"], os.WNOHANG)
        except ChildProcessError:
            pass


class Dolt(GoEngine):
    name = "dolt"

    def default_config(self, root, sources):
        return {"source": str(sources / "dolt"), "binary": str(root / "bin/dolt"), "port": 13360}

    source_entry = "go/go.mod"
    version_args = ("version",)
    server_args = ("sql-server", "--config")
    go_dir = "go"
    go_target = "./cmd/dolt"
    databases_sql = "SELECT schema_name FROM information_schema.schemata"

    def connection(self, port, db):
        return "mysql", f"root@tcp(127.0.0.1:{port})/{db or ''}?timeout=15s"

    def vc_sql(self, op, args):
        return f"CALL dolt_{op}({','.join(literal(a) for a in args)})"

    def table_count_sql(self):
        # (a MySQL server's information schema lists every database's tables)
        return ("SELECT count(*) FROM information_schema.tables "
                "WHERE table_schema=database() AND table_name='bench_rows'")


class Doltgres(GoEngine):
    name = "doltgres"

    def default_config(self, root, sources):
        source = sources / "doltgres"
        if not source.exists():
            source = sources / "doltgresql"
        return {"source": str(source), "binary": str(root / "bin/doltgres"), "port": 15432}

    source_entry = "go.mod"
    version_args = ("-version",)
    server_args = ("-config",)
    go_target = "./cmd/doltgres"

    def server_config(self, root, c):
        return super().server_config(root, c) + "user:\n  name: postgres\n  password: password\n"

    def ids_in(self, ids):
        # Doltgres plans a literal IN list in time quadratic in its length
        # (1,000 ids: about 0.3 s, nearly all planning, for a 1 ms read);
        # a subquery over a VALUES list is planned in linear time (an
        # UPDATE of 1,000 rows: 286 ms to 45 ms).  Its VALUES columns have
        # no default names, so the list is aliased.  (= ANY(ARRAY[...]) and
        # UPDATE ... FROM (VALUES ...) are slower still.)
        return ("id IN (SELECT id FROM (VALUES " +
                ",".join(f"({int(i)})" for i in ids) + ") v(id))")


class Prolly(Engine):
    """PostgreSQL with the prolly table access method."""
    name = "prolly"
    source_entry = "configure"
    def default_config(self, root, sources):
        import core
        return {"source": str(core.REPO), "binary": str(root / "build/postgres/inst/bin/postgres"),
                "port": 15433}

    SETTINGS = ("shared_buffers", "work_mem", "fsync", "synchronous_commit", "full_page_writes",
                "autovacuum", "max_wal_size", "checkpoint_timeout")

    def build_command(self, root, c, jobs):
        import core
        source = Path(c["engines"][self.name]["source"])
        return (["python3", str(core.REPO / "src/tools/prolly_bench/build_postgres.py"),
                 "--source", str(source), "--build-dir", str(root / "build/postgres"),
                 "--jobs", str(jobs), "--", "--with-openssl", "CFLAGS=-O2"], core.REPO)

    def preflight_build(self, checks, root, checkout, source_ok, tools_ok):
        import preflight
        import core
        tools_ok_pg = checks.test('PostgreSQL build tools',
                                  lambda: preflight.require_tools(['make', 'bison', 'flex', 'perl', 'pkg-config']))
        if source_ok and tools_ok_pg:
            core.log('Checking PostgreSQL configure dependencies in a temporary directory')
            checks.test('PostgreSQL configure dependencies', lambda: preflight.postgres_dependencies(root, checkout))

    def preflight_installed(self, binary, probe, require_executable):
        if os.geteuid() == 0:
            raise ValueError('PostgreSQL cannot run as root; use a non-root account')
        for name in ('initdb', 'pg_ctl', 'pg_config'):
            tool = binary.parent / name
            require_executable(tool)
            probe([str(tool), '--version'])
        share = Path(probe([str(binary.parent / 'pg_config'), '--sharedir']).strip())
        if not (share / 'postgres.bki').is_file():
            raise ValueError(f'PostgreSQL install is incomplete: missing {share / "postgres.bki"}; run make install')

    def start(self, root, c, home, binary):
        pg_bin = binary.parent
        data = home / "data"
        if not data.exists():
            subprocess.run([str(pg_bin / "initdb"), "-D", str(data), "-U", "postgres",
                            "--auth-local=trust", "--auth-host=trust", "--no-locale", "-E", "UTF8"],
                           check=True, stdout=subprocess.DEVNULL)
            with (data / "postgresql.conf").open("a") as f:
                f.write(f"\nlisten_addresses = '127.0.0.1'\nport = {c['engines'][self.name]['port']}\n"
                        "unix_socket_directories = ''\nshared_buffers = '256MB'\n"
                        "autovacuum = off\nmax_wal_size = '8GB'\ncheckpoint_timeout = '30min'\n")
        cmd = [str(pg_bin / "pg_ctl"), "-D", str(data), "-l", str(home / "server.log"), "-w", "start"]
        subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
        return int((data / "postmaster.pid").read_text().splitlines()[0]), cmd

    def stop(self, root, c, info, running):
        subprocess.run([str(Path(c["engines"][self.name]["binary"]).parent / "pg_ctl"),
                        "-D", str(root / self.name / "data"), "-m", "fast", "-w", "stop"],
                       check=True, stdout=subprocess.DEVNULL)

    def hash_sql(self, ref):
        return f"SELECT dolt_hashof({literal(ref)})"

    def diff_sql(self, a, b):
        return f"SELECT count(*) FROM dolt_diff({literal(a)},{literal(b)},NULL::bench_rows)"

    def create_table(self, sql, ddl):
        versioned = sql.scalar("SELECT nspversioned FROM pg_namespace WHERE nspname='public'")
        if versioned not in ("true", "t"):
            sql.query("DROP SCHEMA public")
            sql.query("CREATE SCHEMA public VERSIONED")
        sql.query(ddl + " USING prolly")
        am = sql.scalar("SELECT amname FROM pg_class c JOIN pg_am a ON a.oid=c.relam "
                        "WHERE c.oid='bench_rows'::regclass")
        if am != "prolly":
            raise RuntimeError(f"table access method: expected 'prolly', got {am!r}")

    has_counters = True

    def counters(self, sql):
        # pg_stat_prolly is per database, and a session's counts reach it
        # only when its statistics are flushed: forced at the end of the
        # first statement, so the read after it sees all the work before.
        sql.query("SELECT pg_stat_force_next_flush()")
        return {name: int(value) for name, value in
                sql.query("SELECT name, value FROM pg_stat_prolly")["rows"]}

    def settings(self, sql):
        names = ",".join(literal(s) for s in self.SETTINGS)
        return sql.query(f"SELECT name,setting FROM pg_settings WHERE name IN ({names})")["rows"]


ENGINES = {e.name: e for e in (Dolt(), Doltgres(), Prolly())}
