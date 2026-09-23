# Integrated renderer benchmark: integrated-forward

Backend: opengl; runs: 3; deadline: 33.33300018310547 ms.

Ranges below span independent runs. Raw frames and GPU validity remain in each run directory.

| Scenario | Frames/run | p50 ms | p95 ms | p99 ms | max ms | misses | CPU p95 ms | valid GPU p95 ms |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| dolly | 180 | 367.26–396.23 | 396.56–432.31 | 490.07–517.99 | 529.62–541.62 | 180.00–180.00 | 40.33–58.15 | 389.71–430.22 |
| rapid-turn | 180 | 255.04–355.30 | 361.33–391.95 | 376.58–402.38 | 380.85–514.52 | 179.00–179.00 | 23.84–29.58 | 357.98–390.28 |
| stationary | 180 | 356.27–386.03 | 383.47–410.38 | 408.35–432.19 | 409.44–440.21 | 180.00–180.00 | 26.27–27.92 | 386.21–406.52 |

GPU samples require `gpuStatus=valid` and distinct `gpuFrameId`; missing values are never zero.
Production and consumption of requested techniques require separate runtime evidence.
Tracked memory mixes CPU and GPU allocations; post-scene-release values retain asset and renderer caches. Isolated GPU and pool bytes are unknown.
