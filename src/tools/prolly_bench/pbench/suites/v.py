"""V: version control, engines with history only, against bounds."""
from pbench.core import suites

suites.register(suites.Suite(
    letter="V", title="version control", layout=suites.VERSION_CONTROL,
    default_config="standard.toml", history_only=True, size_keys=frozenset({"v_rows"}),
    order=2))
