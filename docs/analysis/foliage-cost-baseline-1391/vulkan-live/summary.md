| subject | pose | path | shadows | frame GPU | foliage share of the frame | main-view cull | shadow-view culls | ShadowPass Δ (casters + culls) | forward draw (+prepass) | G-buffer share (ScenePass Δ) | GPU culling off: frame Δ | density LOD off: frame Δ | null GPU samples |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Traversal | Step0 | forward | CSM | 10.65 | 9.04 | 0.13 | 1.01 | 7.27 | 1.39 | -0.01 | 266.55 | 0.28 | 0 |
| Traversal | Step4 | forward | CSM | 13.23 | 11.07 | 0.13 | 1.08 | 9.10 | 1.78 | -0.02 | 264.55 | 0.26 | 0 |
| Traversal | Step8 | forward | CSM | 9.25 | 7.34 | 0.13 | 1.09 | 5.92 | 1.06 | 0.00 | 268.46 | -0.17 | 1 |
| Traversal | Step0 | forward | VSM | 37.39 | 35.22 | 0.13 | 2.32 | 32.97 | 1.55 | -0.01 | 811.86 | 0.82 | 0 |
| Traversal | Step4 | forward | VSM | 42.72 | 39.80 | 0.13 | 2.43 | 37.25 | 1.91 | -0.02 | 869.75 | 1.15 | 0 |
| Traversal | Step8 | forward | VSM | 23.04 | 20.54 | 0.12 | 2.62 | 19.03 | 1.16 | 0.00 | 914.78 | 1.21 | 0 |
| Traversal | Step0 | forwardplus | CSM | 11.06 | 8.82 | 0.13 | 1.01 | 7.78 | 1.40 | -0.01 | 267.60 | -0.10 | 1 |
| Traversal | Step4 | forwardplus | CSM | 13.29 | 11.35 | 0.14 | 1.09 | 9.29 | 1.80 | -0.02 | 264.94 | 0.26 | 0 |
| Traversal | Step8 | forwardplus | CSM | 9.18 | 6.82 | 0.13 | 1.09 | 5.69 | 1.06 | 0.00 | 269.18 | 0.17 | 2 |
| Traversal | Step0 | forwardplus | VSM | 37.51 | 34.28 | 0.14 | 2.33 | 33.30 | 1.56 | -0.01 | 855.05 | 0.40 | 1 |
| Traversal | Step4 | forwardplus | VSM | 42.61 | 39.07 | 0.14 | 2.76 | 36.88 | 1.92 | -0.01 | 860.35 | 0.77 | 3 |
| Traversal | Step8 | forwardplus | VSM | 23.04 | 20.01 | 0.13 | 2.73 | 19.10 | 1.15 | 0.00 | 866.30 | 0.59 | 2 |
| Traversal | Step0 | deferred | CSM | 10.83 | 7.67 | 0.13 | 1.05 | 7.58 | 0.00 | 1.16 | 266.08 | 0.38 | 0 |
| Traversal | Step4 | deferred | CSM | 13.69 | 9.80 | 0.14 | 1.13 | 9.16 | 0.00 | 1.66 | 264.60 | 0.05 | 0 |
| Traversal | Step8 | deferred | CSM | 9.26 | 5.44 | 0.13 | 1.11 | 5.64 | 0.00 | 0.86 | 267.25 | 0.40 | 0 |
| Traversal | Step0 | deferred | VSM | 37.45 | 34.25 | 0.13 | 2.34 | 33.33 | 0.00 | 1.20 | 844.70 | 0.75 | 0 |
| Traversal | Step4 | deferred | VSM | 42.41 | 38.97 | 0.14 | 2.73 | 37.54 | 0.00 | 1.79 | 838.90 | 1.21 | 0 |
| Traversal | Step8 | deferred | VSM | 23.09 | 19.62 | 0.13 | 2.72 | 19.09 | 0.00 | 0.88 | 850.48 | 0.43 | 0 |
| Meadow | frontal | forward | CSM | 21.16 | 17.39 | 0.09 | 0.58 | 15.99 | 1.90 | -0.03 | 171.88 | 0.20 | 1 |
| Meadow | frontal | forward | VSM | 55.07 | 51.64 | 0.09 | 1.65 | 48.85 | 1.97 | 0.00 | 530.08 | -0.18 | 0 |
| Meadow | frontal | forwardplus | CSM | 21.23 | 17.38 | 0.09 | 0.57 | 15.99 | 1.88 | -0.01 | 172.28 | 0.02 | 0 |
| Meadow | frontal | forwardplus | VSM | 54.39 | 49.93 | 0.09 | 1.65 | 48.39 | 1.97 | -0.01 | 527.41 | 1.17 | 0 |
| Meadow | frontal | deferred | CSM | 21.10 | 16.44 | 0.09 | 0.58 | 16.07 | 0.00 | 1.86 | 171.58 | 0.26 | 0 |
| Meadow | frontal | deferred | VSM | 53.31 | 49.77 | 0.09 | 1.62 | 48.15 | 0.00 | 1.78 | 504.24 | 0.20 | 0 |
| Woodland | frontal | forward | CSM | 21.94 | 19.37 | 0.09 | 0.57 | 16.64 | 2.26 | -0.01 | 136.05 | -0.05 | 0 |
| Woodland | frontal | forward | VSM | 76.49 | 73.65 | 0.08 | 1.36 | 70.73 | 2.31 | -0.02 | 369.26 | 1.69 | 0 |
| Woodland | frontal | forwardplus | CSM | 21.72 | 19.17 | 0.08 | 0.52 | 16.97 | 2.25 | -0.02 | 129.91 | 0.71 | 0 |
| Woodland | frontal | forwardplus | VSM | 75.34 | 72.48 | 0.08 | 1.36 | 69.63 | 2.32 | -0.02 | 368.56 | 1.07 | 0 |
| Woodland | frontal | deferred | CSM | 21.57 | 19.36 | 0.08 | 0.52 | 16.81 | 0.00 | 1.96 | 130.15 | -0.30 | 0 |
| Woodland | frontal | deferred | VSM | 75.27 | 72.47 | 0.08 | 1.35 | 69.65 | 0.00 | 1.98 | 370.90 | 0.68 | 0 |

| pose | path | frames | frame time p50 | p95 | p99 | max | misses (16.67 ms) | GPU p50 | GPU p99 |
|---|---|---|---|---|---|---|---|---|---|
| Step0 | forward | 1024 | 10.75 | 20.45 | 71.98 | 176.70 | 73 | 10.64 | 62.05 |
| Step4 | forward | 1024 | 13.05 | 13.88 | 14.50 | 20.77 | 4 | 12.96 | 13.93 |
| Step8 | forward | 1024 | 9.04 | 10.61 | 13.63 | 92.76 | 3 | 8.92 | 10.14 |
| Step0 | forwardplus | 1024 | 10.61 | 11.61 | 12.35 | 36.45 | 3 | 10.51 | 11.97 |
| Step4 | forwardplus | 1024 | 13.08 | 14.37 | 18.17 | 35.38 | 15 | 13.03 | 14.25 |
| Step8 | forwardplus | 1024 | 9.48 | 16.12 | 25.93 | 42.11 | 46 | 8.96 | 19.43 |
| Step0 | deferred | 1024 | 10.82 | 16.50 | 22.66 | 35.40 | 51 | 10.76 | 15.08 |
| Step4 | deferred | 1024 | 13.33 | 19.80 | 27.12 | 38.87 | 110 | 13.28 | 17.14 |
| Step8 | deferred | 1024 | 9.10 | 13.83 | 21.31 | 37.00 | 28 | 8.98 | 15.65 |
