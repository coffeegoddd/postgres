"""G and GV: PostGIS.

G is spatial storage: heap against prolly on the same build with the same
PostGIS build, laid out as S is.  GV is spatial version control: prolly
only, against bounds, as V is.  Their builds carry PostGIS
(build_postgres.py --with-postgis), and every database of theirs installs
it.  Their workloads (pbench/workloads/g_suite.py), size keys and
parameters register here with them.
"""
from pbench.core import suites
from pbench.workloads import g_suite

EXTENSIONS = ("postgis", "postgis_raster", "postgis_topology")


def _params(letter: str) -> dict[str, frozenset[str]]:
    return {wid: frozenset(keys) for wid, keys in g_suite.PARAMS.items()
            if suites.workload_number(wid, letter) is not None}


suites.register(suites.Suite(
    letter="G", title="PostGIS storage", layout=suites.STORAGE, default_config="standard.toml",
    extensions=EXTENSIONS, size_keys=g_suite.SIZE_KEYS, workload_params=_params("G"),
    transaction_workloads=g_suite.PGBENCH, rate_workloads=g_suite.PGBENCH, order=4))

suites.register(suites.Suite(
    letter="GV", title="PostGIS version control", layout=suites.VERSION_CONTROL,
    default_config="standard.toml", history_only=True, extensions=EXTENSIONS,
    size_keys=g_suite.SIZE_KEYS, workload_params=_params("GV"), order=5))
