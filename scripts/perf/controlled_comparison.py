"""Paired run-level gates. Consecutive frames are never independent replicates.

Exit 0: evidence supports the configured tolerance; 1: regression; 2: inconclusive
or unsupported. A controlled job must propagate all three instead of treating a
missing GPU or a wide confidence interval as a pass.
"""

from __future__ import annotations

import math
import statistics


def median_interval(values: list[float], alpha: float) -> tuple[float, float]:
    """Distribution-free, two-sided sign interval for the population median.

    The observations are independent run-pair effects, not individual frames.
    Ties make coverage conservative. No Gaussian or frame independence premise.
    """
    if not values or not 0 < alpha < 1 or any(not math.isfinite(v) for v in values):
        raise ValueError("finite independent effects and 0 < alpha < 1 required")
    ordered = sorted(values)
    n = len(ordered)
    tail = 0.0
    accepted = -1
    for k in range(n // 2):
        tail += math.comb(n, k) / 2**n
        if 2 * tail <= alpha:
            accepted = k
        else:
            break
    if accepted < 0:
        raise ValueError("too few independent pairs for requested confidence")
    return ordered[accepted], ordered[n - accepted - 1]


def compare_pairs(a: list[float], b: list[float], *, tolerance: float,
                  alpha: float, max_interval_width: float, relative: bool = True) -> dict:
    """Compare paired scalar run statistics, using a predeclared noise ceiling.

    Relative effects use log ratios; deadline-miss fractions use absolute deltas
    so zero baseline misses are valid. Tolerance is fractional (0.05 = 5%).
    """
    if len(a) != len(b) or not a:
        raise ValueError("equal, nonempty paired runs required")
    if any(not math.isfinite(v) or v < 0 for v in a + b):
        raise ValueError("run statistics must be finite and nonnegative")
    if not math.isfinite(tolerance) or tolerance < 0:
        raise ValueError("tolerance must be finite and nonnegative")
    if not math.isfinite(max_interval_width) or max_interval_width <= 0:
        raise ValueError("noise ceiling must be finite and positive")
    if relative and any(v <= 0 for v in a + b):
        raise ValueError("zero timing cannot support a relative comparison")
    effects = [math.log(y / x) if relative else y - x for x, y in zip(a, b)]
    lower, upper = median_interval(effects, alpha)
    if relative:
        lower, upper = math.expm1(lower), math.expm1(upper)
        estimate = math.expm1(statistics.median(effects))
    else:
        estimate = statistics.median(effects)
    status = ("inconclusive" if upper - lower > max_interval_width else
              "regression" if lower > tolerance else
              "pass" if upper <= tolerance else "inconclusive")
    return {"status": status, "medianEffect": estimate, "interval": [lower, upper],
            "alpha": alpha, "tolerance": tolerance, "pairs": len(a),
            "maxIntervalWidth": max_interval_width,
            "effect": "relative" if relative else "absolute",
            "rawA": a, "rawB": b,
            "uncertaintyUnit": "independent paired runs"}
