"""C: SQL correctness (sqllogictest), any table AM, answers compared with heap.

It measures answers, not speed, and never crashes the server on purpose,
so it may run without the durability settings, and the environment's
performance warnings don't apply to it.
"""
from pbench.core import suites

suites.register(suites.Suite(
    letter="C", title="SQL correctness", layout=suites.CORRECTNESS,
    default_config="c_standard.toml", durable=False, measures_performance=False,
    order=3))
