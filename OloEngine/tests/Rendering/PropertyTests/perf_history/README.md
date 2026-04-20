# Perf History (Layer 6 §6)

Every run of `PerfRegressionTests` appends one TSV row per microbenchmark to
`<machine>.tsv` in this directory. Rows are never pruned — this is the raw
historical series used for trend analysis and cross-machine comparison.

## Schema

```text
iso_date_utc    name    measured_ns    baseline_ns    ratio
```

`baseline_ns` is `0` when no baseline was committed yet; `ratio` is `nan` in
that case.

## Machine tag resolution

1. `OLOENGINE_PERF_MACHINE` env var (canonical, set by CI).
2. `COMPUTERNAME` / `HOSTNAME` env var.
3. Literal `"unknown"`.

## Visualising trends

`OloEngine/tests/scripts/perf_trend.py` reads every TSV in this directory and emits a flat
summary (min / median / p95 per benchmark per machine) plus a 3-sigma
regression flag for the last 30 samples vs the prior 30.

```
python OloEngine/tests/scripts/perf_trend.py
```

## Rebasing the baseline

Setting `OLOENGINE_PERF_REBASE=1` rewrites `perf_baselines.txt` with the
current run's numbers. The history file continues to record every run —
rebases are visible in the TSV as a point where ratio returns to ~1.0.
