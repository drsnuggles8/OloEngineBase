# Integrated renderer benchmark: integrated-forward-plus

Backend: opengl; runs: 3; deadline: 33.33300018310547 ms.

Ranges below span independent runs. Raw frames and GPU validity remain in each run directory.

| Scenario | Frames/run | p50 ms | p95 ms | p99 ms | max ms | misses | CPU p95 ms | valid GPU p95 ms |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| dolly | 180 | 344.93–432.05 | 367.88–463.70 | 381.23–500.31 | 396.86–512.73 | 180.00–180.00 | 31.54–76.07 | 362.82–457.41 |
| rapid-turn | 180 | 212.65–398.27 | 355.11–476.12 | 365.31–554.95 | 366.84–658.13 | 179.00–180.00 | 30.19–40.42 | 350.73–473.04 |
| stationary | 180 | 339.14–424.62 | 359.22–457.96 | 380.50–486.25 | 406.36–632.40 | 180.00–180.00 | 27.10–50.28 | 356.75–453.14 |

GPU samples require `gpuStatus=valid` and distinct `gpuFrameId`; missing values are never zero.
Production and consumption of requested techniques require separate runtime evidence.
Tracked memory mixes CPU and GPU allocations; post-scene-release values retain asset and renderer caches. Isolated GPU and pool bytes are unknown.
