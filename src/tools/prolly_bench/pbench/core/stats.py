"""Small statistics helpers: latency histograms, medians and spread."""

from __future__ import annotations

import math
import statistics
import random
from typing import Iterable, Optional


class LatencyHistogram:
    """Log-bucketed latency histogram with about 1% relative error.

    Keeps memory bounded however many transactions a run logs.
    """

    _BUCKETS_PER_DOUBLING = 70  # 2 ** (1/70) ~ 1.0100

    def __init__(self) -> None:
        self._counts: dict[int, int] = {}
        self.count = 0
        self.total_us = 0.0
        self.max_us = 0.0

    def _bucket(self, us: float) -> int:
        if us < 1:
            return 0
        return 1 + int(math.log2(us) * self._BUCKETS_PER_DOUBLING)

    def _upper(self, bucket: int) -> float:
        if bucket == 0:
            return 1.0
        return 2 ** (bucket / self._BUCKETS_PER_DOUBLING)

    def add(self, us: float, n: int = 1) -> None:
        """Record n observations of us microseconds."""
        if not math.isfinite(us) or us < 0 or type(n) is not int or n < 0:
            raise ValueError("latencies and observation counts must be finite and nonnegative")
        b = self._bucket(us)
        self._counts[b] = self._counts.get(b, 0) + n
        self.count += n
        self.total_us += us * n
        self.max_us = max(self.max_us, us)

    def percentile(self, p: float) -> Optional[float]:
        """The p-th percentile (0-100) in microseconds."""
        if self.count == 0:
            return None
        rank = math.ceil(p / 100.0 * self.count)
        seen = 0
        for b in sorted(self._counts):
            seen += self._counts[b]
            if seen >= rank:
                return min(self._upper(b), self.max_us)
        return self.max_us

    def mean(self) -> Optional[float]:
        return self.total_us / self.count if self.count else None

    def reported_percentile(self, p: float) -> Optional[float]:
        """Suppress tails with fewer than ten expected observations beyond p.

        This is a minimum sample floor, not a confidence interval.
        """
        minimum = {50: 20, 99: 1000, 99.9: 10000}.get(p, 1)
        return self.percentile(p) if self.count >= minimum else None

    def to_dict(self) -> dict:
        """Bucket upper bounds (us) and counts, for keeping with results."""
        return {"count": self.count, "max_us": self.max_us,
                "mean_us": self.mean(),
                "buckets": [[round(self._upper(b), 3), self._counts[b]]
                            for b in sorted(self._counts)]}


def per(value: Optional[float], count: Optional[float]) -> Optional[float]:
    """value per unit of count, or None if either is missing or zero."""
    return None if value is None or not count else value / count


def counter_delta(before: dict, after: dict) -> dict:
    """Counter resets and unavailable values invalidate a measured interval."""
    if before.keys() != after.keys():
        raise ValueError("counter set changed during measurement")
    delta = {key: after[key] - value for key, value in before.items()}
    if any(not math.isfinite(value) or value < 0 for value in delta.values()):
        raise ValueError("counter reset or invalid value during measurement")
    return delta


def summarize(values: Iterable[Optional[float]]) -> dict[str, Optional[float]]:
    """Median and spread (min/max) of the non-missing values."""
    vals = [v for v in values if v is not None]
    if not vals:
        return {"median": None, "min": None, "max": None, "n": 0}
    return {"median": statistics.median(vals), "min": min(vals),
            "max": max(vals), "n": len(vals)}


def paired_ratio_interval(pairs: list[tuple[float, float]]) -> Optional[tuple[float, float, float]]:
    """Seeded paired-bootstrap 95% interval of the median ratio.

    Requires five independent repetitions, positive finite baselines, and
    matched seeds/conditions (enforced by the caller). Not a tail-latency CI.
    """
    if len(pairs) < 5 or any(not math.isfinite(a) or not math.isfinite(b) or a <= 0 or b <= 0 for a, b in pairs):
        return None
    ratios = [a / b for a, b in pairs]
    rng = random.Random(0)
    estimates = sorted(statistics.median(rng.choices(ratios, k=len(ratios))) for _ in range(2000))
    return statistics.median(ratios), estimates[49], estimates[1949]
