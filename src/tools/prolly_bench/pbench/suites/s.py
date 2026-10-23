"""S: storage, any table AM against the same build's heap (B0)."""
from pbench.core import suites

suites.register(suites.Suite(
    letter="S", title="storage", layout=suites.STORAGE, default_config="standard.toml",
    size_keys=frozenset({"pgbench_scale", "s4_rows", "s7_rows", "tpcc_warehouses", "tpch_scale",
                         "sysbench_tables", "sysbench_table_size"}),
    # pgbench and sysbench take a transaction count; pgbench an offered rate.
    transaction_workloads=frozenset({"S1", "S2", "S3", "S5", "S6", "S7", "S8", "S10"}),
    rate_workloads=frozenset({"S1", "S2", "S3", "S5", "S6", "S7", "S8"}),
    order=1))
