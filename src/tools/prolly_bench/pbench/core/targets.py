"""Targets: how a run puts its tables on the table AM under test.

Setting default_table_access_method is enough for every AM except prolly,
whose tables exist only in versioned schemas: with prolly as the default, a
table created anywhere else is quietly a heap table.  A prolly target
therefore creates its tables in a versioned schema, which its databases
put first on their search_path, so every client and every unqualified name
finds them there.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Optional

# Table AMs whose tables must be in a versioned schema.
VERSIONED_ONLY = {"prolly"}

# The versioned schema a target creates its tables in.
VERSIONED_SCHEMA = "versioned"


@dataclass(frozen=True)
class Target:
    table_am: str
    # A versioned schema to create the tables in, or None for the default
    # (public).
    schema: Optional[str] = None


    def setup_sql(self, dbname: str) -> str:
        """SQL to run in a new database before any tables are created."""
        if not self.schema:
            return ""
        return (f"CREATE SCHEMA {self.schema} VERSIONED;\n"
                f"ALTER DATABASE {dbname} SET search_path = {self.schema}, public;")

    def wrong_am(self, cluster, dbname: str, tables: list[str]) -> list[str]:
        """Those of tables (that exist) not on this target's AM, as
        "table:am"."""
        out = []
        for t in tables:
            am = cluster.scalar(
                "SELECT a.amname FROM pg_class c JOIN pg_am a ON a.oid = c.relam "
                f"WHERE c.oid = to_regclass('{t}')", dbname=dbname)
            if am != self.table_am:
                out.append(f"{t}:{am or 'missing'}")
        return out

    def verify(self, cluster, dbname: str) -> None:
        cluster.sql("CREATE TABLE pbench_target_canary (id int PRIMARY KEY, payload text)",
                    dbname=dbname, options={"default_table_access_method": self.table_am})
        try:
            wrong = self.wrong_am(cluster, dbname, ["pbench_target_canary"])
            if wrong:
                raise RuntimeError(f"target canary failed: {wrong}")
            if self.table_am == "prolly":
                count = cluster.scalar("""SELECT count(*) FROM pg_class WHERE
                    (oid = 'pbench_target_canary'::regclass OR oid = 'pbench_target_canary_pkey'::regclass
                     OR oid = (SELECT reltoastrelid FROM pg_class WHERE oid = 'pbench_target_canary'::regclass))
                    AND (pg_stat_file(pg_relation_filepath(oid) || '_bmap', true)).size IS NOT NULL""",
                    dbname=dbname)
                if count != "3":
                    raise RuntimeError("table, index or TOAST target is not block-mapped")
        finally:
            cluster.sql("DROP TABLE pbench_target_canary", dbname=dbname)


def for_am(table_am: str) -> Target:
    return Target(table_am, VERSIONED_SCHEMA if table_am in VERSIONED_ONLY else None)
