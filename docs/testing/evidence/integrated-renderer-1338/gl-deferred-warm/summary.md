# Integrated renderer benchmark: integrated-deferred

Backend: opengl; runs: 3; deadline: 33.33300018310547 ms.

Ranges below span independent runs. Raw frames and GPU validity remain in each run directory.

| Scenario | Frames/run | p50 ms | p95 ms | p99 ms | max ms | misses | CPU p95 ms | valid GPU p95 ms |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| dolly | 180 | 386.96–439.67 | 405.47–477.05 | 443.17–483.12 | 485.22–513.80 | 180.00–180.00 | 30.43–34.39 | 399.77–474.21 |
| rapid-turn | 180 | 279.93–404.13 | 409.31–438.34 | 432.96–446.02 | 438.83–491.75 | 179.00–179.00 | 31.94–51.41 | 402.80–435.56 |
| stationary | 180 | 374.54–422.96 | 430.27–548.41 | 496.57–812.86 | 501.05–1713.33 | 180.00–180.00 | 41.15–159.84 | 390.33–445.40 |

GPU samples require `gpuStatus=valid` and distinct `gpuFrameId`; missing values are never zero.
Production and consumption of requested techniques require separate runtime evidence.
Tracked memory mixes CPU and GPU allocations; post-scene-release values retain asset and renderer caches. Isolated GPU and pool bytes are unknown.
