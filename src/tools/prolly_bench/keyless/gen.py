#!/usr/bin/env python3
"""Write the keyless benchmark's data and scripts into a work directory.

    gen.py WORKDIR

Writes WORKDIR/data.csv (1,000,000 rows of (a, b, c); the last 100,000
repeat the first 100,000 exactly), the timed scripts heap.sql, prolly.sql
and dolt.sql (a table without a primary key) and *_keyed.sql (the same
table with one), and merge_pg.sql and merge_dolt.sql: six cases of
changes to the same keyless rows on two branches, merged.
"""

import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent

MERGE_CASES = [
    ("both add a copy of an existing row", ["(1,'x')"],
     ["INSERT INTO t VALUES (1,'x')"], ["INSERT INTO t VALUES (1,'x')"]),
    ("both add the same new row", ["(1,'x')"],
     ["INSERT INTO t VALUES (2,'y')"], ["INSERT INTO t VALUES (2,'y')"]),
    ("each adds a different row", ["(1,'x')"],
     ["INSERT INTO t VALUES (2,'y')"], ["INSERT INTO t VALUES (3,'z')"]),
    ("both delete the same row", ["(1,'x')", "(2,'y')"],
     ["DELETE FROM t WHERE a = 1"], ["DELETE FROM t WHERE a = 1"]),
    ("both update the same row differently", ["(1,'x')"],
     ["UPDATE t SET b = 'ours' WHERE a = 1"], ["UPDATE t SET b = 'theirs' WHERE a = 1"]),
    ("one deletes a copy of two, the other adds one", ["(1,'x')", "(1,'x')"],
     ["DELETE ONE COPY"], ["INSERT INTO t VALUES (1,'x')"]),
]


def write_data(path):
    with open(path, "w") as f:
        for i in range(1, 1000001):
            a = i - 900000 if i > 900000 else i
            f.write(f"{a},v{a % 997},{a % 100}\n")


def keyed(script, dialect):
    if dialect == "dolt":
        script = script.replace(
            "CREATE TABLE t (a int, b varchar(32), c int);",
            "CREATE TABLE t (id bigint AUTO_INCREMENT PRIMARY KEY, a int, b varchar(32), c int);")
        script = script.replace("INTO TABLE t FIELDS TERMINATED BY ','",
                                "INTO TABLE t FIELDS TERMINATED BY ',' (a, b, c)")
    else:
        script = script.replace(
            "CREATE TABLE t (a int, b varchar(32), c int);",
            "CREATE TABLE t (id bigserial PRIMARY KEY, a int, b varchar(32), c int);")
        script = script.replace("COPY t FROM", "COPY t (a, b, c) FROM")
    script = re.sub(r"INSERT INTO t VALUES \(", "INSERT INTO t (a, b, c) VALUES (", script)
    return script.replace("INSERT INTO t SELECT a + 3000000",
                          "INSERT INTO t (a, b, c) SELECT a + 3000000")


def merge_script(dialect):
    call = "SELECT" if dialect == "pg" else "CALL"
    out = []
    if dialect == "pg":
        out += ["CREATE SCHEMA v VERSIONED;", "SET search_path = v, public;"]
    else:
        out += ["SET @@dolt_allow_commit_conflicts = 1;"]
    for n, (name, base, ours, theirs) in enumerate(MERGE_CASES):
        def edit(e):
            if e != "DELETE ONE COPY":
                return e
            return ("DELETE FROM t WHERE ctid = (SELECT min(ctid) FROM t)"
                    if dialect == "pg" else "DELETE FROM t WHERE a = 1 LIMIT 1")
        out.append(f"SELECT '=== {name}' AS case_name;")
        out.append("DROP TABLE IF EXISTS t;")
        out.append("CREATE TABLE t (a int, b varchar(16));")
        out += [f"INSERT INTO t VALUES {r};" for r in base]
        out.append(f"{call} dolt_commit('-Am', 'base {n}');")
        out.append(f"{call} dolt_branch('c{n}');")
        out.append(f"{call} dolt_checkout('c{n}');")
        out += [edit(e) + ";" for e in theirs]
        out.append(f"{call} dolt_commit('-am', 'theirs {n}');")
        out.append(f"{call} dolt_checkout('main');")
        out += [edit(e) + ";" for e in ours]
        out.append(f"{call} dolt_commit('-am', 'ours {n}');")
        out.append(f"{call} dolt_merge('c{n}');")
        out.append("SELECT a, b, count(*) FROM t GROUP BY a, b ORDER BY a, b;")
        out.append("SELECT * FROM dolt_conflicts;")
        if dialect == "pg":
            out.append("SELECT dolt_merge('--abort') WHERE EXISTS (SELECT 1 FROM dolt_conflicts);")
            out.append("SELECT dolt_reset('--hard');")
        else:
            out.append("SELECT * FROM dolt_conflicts_t;")
            out.append("CALL dolt_merge('--abort');")
            out.append("CALL dolt_reset('--hard');")
    return "\n".join(out) + "\n"


def main():
    work = Path(sys.argv[1]).resolve()
    work.mkdir(parents=True, exist_ok=True)
    data = work / "data.csv"
    write_data(data)
    dml = (HERE / "dml.tmpl").read_text()
    vc = (HERE / "vc.tmpl").read_text()

    pg_dml = dml.replace("%LOAD%", f"COPY t FROM '{data}' WITH (FORMAT csv)") \
        .replace("%ANALYZE%", "VACUUM ANALYZE t")
    heap = "@setup\nCREATE TABLE t (a int, b varchar(32), c int);\n" + pg_dml
    prolly = ("@setup\nCREATE SCHEMA v VERSIONED;\nSET search_path = v, public;\n"
              "CREATE TABLE t (a int, b varchar(32), c int);\n") + pg_dml + \
        vc.replace("%CALL%", "SELECT") \
          .replace("%DIFF_WORKING%", "dolt_diff('HEAD', 'WORKING', NULL::t)") \
          .replace("%DIFF_COMMITS%", "dolt_diff('HEAD~1', 'HEAD', NULL::t)")
    dolt_dml = dml.replace("%LOAD%", f"LOAD DATA INFILE '{data}' INTO TABLE t "
                                     "FIELDS TERMINATED BY ','") \
        .replace("%ANALYZE%", "ANALYZE TABLE t")
    dolt = "@setup\nCREATE TABLE t (a int, b varchar(32), c int);\n" + dolt_dml + \
        vc.replace("%CALL%", "CALL") \
          .replace("%DIFF_WORKING%", "dolt_diff('HEAD', 'WORKING', 't')") \
          .replace("%DIFF_COMMITS%", "dolt_diff('HEAD~1', 'HEAD', 't')")

    for name, script, dialect in (("heap", heap, "pg"), ("prolly", prolly, "pg"),
                                  ("dolt", dolt, "dolt")):
        (work / f"{name}.sql").write_text(script)
        (work / f"{name}_keyed.sql").write_text(keyed(script, dialect))
    (work / "merge_pg.sql").write_text(merge_script("pg"))
    (work / "merge_dolt.sql").write_text(merge_script("dolt"))


if __name__ == "__main__":
    main()
