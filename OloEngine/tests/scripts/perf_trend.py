#!/usr/bin/env python3
# =============================================================================
# perf_trend.py
#
# Layer-6 cross-run performance trend analysis for OloEngine.
#
# Reads every `<machine>.tsv` under
#   OloEngine/tests/Rendering/PropertyTests/perf_history/
# and prints, per (machine, benchmark):
#
#   - sample count
#   - min / median / p95 measured_ns for the full series
#   - min / median / p95 for the most recent 30 samples
#   - "REGRESSION" flag when recent-30 median is > 3 sigma above prior-30
#     (reference period) — a loose but robust drift detector that won't
#     trigger on single outliers.
#
# The script fails with exit code 1 if any benchmark crosses the regression
# threshold. CI jobs invoke it after the perf test run to surface drift
# that wouldn't trip the per-run 1.5x warn threshold.
# =============================================================================

import csv
import math
import pathlib
import statistics
import sys
from collections import defaultdict


REPO_ROOT = pathlib.Path(__file__).resolve().parents[3]
HISTORY_DIR = REPO_ROOT / "OloEngine" / "tests" / "Rendering" / "PropertyTests" / "perf_history"


def percentile(values, q):
    """Simple nearest-rank percentile for short lists."""
    if not values:
        return float("nan")
    s = sorted(values)
    k = max(0, min(len(s) - 1, round((q / 100.0) * (len(s) - 1))))
    return s[k]


def analyse_series(samples):
    """Return (summary_dict, regression_flag). samples is a list of ints."""
    out = {
        "count": len(samples),
        "min": min(samples) if samples else 0,
        "median": int(statistics.median(samples)) if samples else 0,
        "p95": int(percentile(samples, 95)),
    }

    # Drift detection: compare median of most-recent 30 vs prior 30.
    # Need at least 60 samples. Uses std-dev of the reference window.
    regression = False
    if len(samples) >= 60:
        recent = samples[-30:]
        reference = samples[-60:-30]
        ref_median = statistics.median(reference)
        ref_stdev = statistics.pstdev(reference)
        recent_median = statistics.median(recent)
        # Floor the denominator at 1 ns so a dead-flat reference window cannot
        # mask a real regression: sigma_delta becomes (recent - ref) / 1ns and
        # still trips the 3.0 threshold when the recent median has drifted.
        epsilon = 1.0
        sigma_delta = (recent_median - ref_median) / max(ref_stdev, epsilon)
        out["recent_median"] = int(recent_median)
        out["ref_median"] = int(ref_median)
        out["sigma_delta"] = round(sigma_delta, 2)
        regression = sigma_delta >= 3.0
        out["regression"] = regression

    return out, regression


def load_tsv(path):
    """Return dict: benchmark_name -> list of measured_ns (chronological)."""
    series = defaultdict(list)
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            if not line or line.startswith("#"):
                continue
            parts = line.rstrip("\n").split("\t")
            if len(parts) < 5:
                continue
            _iso, name, measured_ns, _baseline_ns, _ratio = parts[:5]
            try:
                series[name].append(int(measured_ns))
            except ValueError:
                continue
    return series


def main():
    if not HISTORY_DIR.is_dir():
        print(f"perf history directory missing: {HISTORY_DIR}")
        return 0

    tsvs = sorted(HISTORY_DIR.glob("*.tsv"))
    if not tsvs:
        print("no perf history recorded yet")
        return 0

    any_regression = False

    for tsv in tsvs:
        machine = tsv.stem
        series = load_tsv(tsv)
        print(f"\n== machine: {machine} ({tsv.name}) ==")
        print(
            f"  {'benchmark':<40} {'n':>4} {'min':>10} {'median':>10} {'p95':>10}  flag"
        )
        for name in sorted(series):
            summary, regression = analyse_series(series[name])
            flag = ""
            if regression:
                flag = f"REGRESSION (+{summary['sigma_delta']}sigma)"
                any_regression = True
            elif "sigma_delta" in summary and summary["sigma_delta"] <= -3.0:
                flag = f"improved ({summary['sigma_delta']}sigma)"
            print(
                f"  {name:<40} {summary['count']:>4} "
                f"{summary['min']:>10} {summary['median']:>10} {summary['p95']:>10}  {flag}"
            )

    return 1 if any_regression else 0


if __name__ == "__main__":
    sys.exit(main())
