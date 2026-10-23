"""G and GV: PostGIS.

G is spatial storage: heap against prolly on the same build with the same
PostGIS build, laid out as S is.  GV is spatial version control: prolly
only, against bounds, as V is.  Their builds carry PostGIS
(build_postgres.py --with-postgis), and every database of theirs installs
it.  Their workloads' size keys, parameters and data manifests register
here with them.
"""
from pbench.core import suites

EXTENSIONS = ("postgis", "postgis_raster", "postgis_topology")

suites.register(suites.Suite(
    letter="G", title="PostGIS storage", layout=suites.STORAGE, default_config="standard.toml",
    extensions=EXTENSIONS, order=4))

suites.register(suites.Suite(
    letter="GV", title="PostGIS version control", layout=suites.VERSION_CONTROL,
    default_config="standard.toml", history_only=True, extensions=EXTENSIONS, order=5))
