"""S13: BenchBase TPC-H, all 22 analytical queries."""

from __future__ import annotations

from pbench.core import registry
from pbench.core.interfaces import Workload
from pbench.workloads import _benchbase


@registry.workloads.register("S13")
class S13(Workload):
    id, suite, title = "S13", "S", "Analytical queries (BenchBase TPC-H)"

    def variants(self, params):
        modes = params.get("modes", ["serial"])
        if not modes or any(m not in {"serial", "throughput"} for m in modes):
            raise ValueError("S13 modes must contain serial and/or throughput")
        return [(mode, {"mode": mode}) for mode in modes]

    def client_counts(self, configured, variant):
        # Upstream Q15 creates/drops the shared revenue0 view. Multiple
        # workers collide on that name, even in separate transactions.
        return [1]

    def scale(self, ctx):
        return float(ctx.size_params["tpch_scale"])

    def check(self, ctx):
        _benchbase.check(ctx)
        marker = _benchbase._dist(ctx) / "PBENCH_ANSWERS_VERSION"
        if not marker.exists() or marker.read_text().strip() != "1":
            raise RuntimeError("TPC-H requires the answer-capture build; run build_benchbase.sh")

    def prepare(self, ctx):
        _benchbase.load(ctx, "tpch", scalefactor=self.scale(ctx))
        # BenchBase's loader does not gather PostgreSQL planner statistics.
        ctx.cluster.sql("ANALYZE", dbname=ctx.dbname,
                        options=ctx.session_options())

    def warmup(self, ctx):
        # (in either mode, queries for warmup_seconds: a serial pass at a
        # large scale could take hours)
        if ctx.warmup_seconds > 0:
            _benchbase.execute(ctx, "tpch", None, scalefactor=self.scale(ctx),
                               seconds=ctx.warmup_seconds)

    def measure(self, ctx, window):
        serial = ctx.params.get("mode", "serial") == "serial"
        out = _benchbase.execute(ctx, "tpch", window, scalefactor=self.scale(ctx),
                                 serial=serial)
        # Q15 writes catalogs when creating/dropping its view, so WAL and
        # device-write metrics still apply even though user data is read-only.
        out.unit = "query"
        out.metrics["qps"] = out.metrics["tps"]
        if serial:
            # BenchBase runs each query cold, then hot: the hot runs are the
            # samples, and collectors span both, so their denominator (the
            # completed queries) must too.
            out.metrics["hot_queries"] = out.metrics["measured_requests"]
            out.metrics["hot_query_seconds"] = (out.metrics["latency_mean_ms"]
                                                * out.metrics["measured_requests"] / 1000)
            out.metrics["client_process_qps"] = out.transactions / window.seconds
            out.metrics["qps"] = None
            out.metrics["tps"] = None
            out.notes.append("Q1-Q22 in order, each with a cold run then one measured "
                             "run; per-query WAL, CPU and writes are over both; no time "
                             "limit (not a TPC-H certification run)")
        return out

    def tables(self, ctx):
        return ["region", "nation", "supplier", "customer", "part", "partsupp",
                "orders", "lineitem"]

    def verify(self, ctx):
        # Separate serial pass with fixed query parameters, outside every
        # measurement window. Keep measured runs free of digest work.
        config = _benchbase._config(ctx, "tpch", self.scale(ctx), 0, serial=True,
                                    suffix="-correctness")
        _benchbase._run(ctx, "tpch", config, ctx.raw_dir / "benchbase-correctness",
                       create=False, load=False, execute=True, tag="correctness")
        from pbench.core.validation import tpch_answers
        tpch_answers(ctx.raw_dir)
