# Integrated AAA acceptance gate (#1259)

Overall: **FAIL**. Gate: p95 frame time <= 16.667 ms in every scenario of every counted run; 3 counted runs required per cell.

GPU: NVIDIA GeForce RTX 4090; driver 621.14. CPU: Intel(R) Core(TM) i7-14700KF. OS: 10.0.26300.

Build (exe): Release / clang-cl, commit a737d3655c7a9215ac5c5499cd777e1e7683d690, dirty diff a7ced43c5ee9, 0 untracked files, binary d1eafa7b0875.
Build (editorExe): Release / clang-cl, commit a737d3655c7a9215ac5c5499cd777e1e7683d690, dirty diff a7ced43c5ee9, 0 untracked files, binary 4c55dd215077.
Asset hash: full-tree.

Ranges are min–max over counted runs. Frame time is a different quantity per host (see each cell); hosts are never pooled.


## gl-forward: FAIL

run 1 coats-closeup p95 86.314 ms > 16.667 ms; run 1 head-backlit p95 150.457 ms > 16.667 ms; run 1 head-closeup p95 144.273 ms > 16.667 ms; run 1 population-dolly p95 100.766 ms > 16.667 ms; run 1 rapid-turn p95 63.485 ms > 16.667 ms; run 2 coats-closeup p95 93.648 ms > 16.667 ms; run 2 head-backlit p95 141.121 ms > 16.667 ms; run 2 head-closeup p95 147.289 ms > 16.667 ms; run 2 population-dolly p95 101.704 ms > 16.667 ms; run 2 rapid-turn p95 61.637 ms > 16.667 ms; run 3 coats-closeup p95 88.698 ms > 16.667 ms; run 3 head-backlit p95 138.502 ms > 16.667 ms; run 3 head-closeup p95 154.745 ms > 16.667 ms; run 3 population-dolly p95 103.969 ms > 16.667 ms; run 3 rapid-turn p95 60.018 ms > 16.667 ms. Counted runs: 3 of 3.

Host: test-binary / opengl. Frame time = wall-clock Scene::OnUpdateEditor call, excluding attachment readback.
Internal 1920x1080, display 1920x1080 (native); upscaler resolved/fallback: {"fallback": "none", "resolved": "native"}.
Selected: lightingTap=None, msaaSamples=1, path=Forward, rayTracedShadowsRequested=False, skinDiffusion=True, upscaleMode=0, upscaleTechnique=0, virtualShadowMaps=True; VSM selected: True; pinned by manifest: MSAASampleCount, Path, RayTracedShadowsEnabled, Upscale, UpscaleTechnique, VirtualShadowMaps.
Selected settings are what the renderer was set to, not proof each technique produced or consumed work.

### coats-closeup (frames/run 300)

| channel | p50 ms | p95 ms | p99 ms | max ms | gate misses | samples |
|---|---:|---:|---:|---:|---:|---:|
| frame time | 67.72–70.17 | 86.31–93.65 | 105.83–116.67 | 390.35–428.72 | 300 | 300 |
| CPU | 66.60–69.07 | 79.87–89.35 | 104.89–115.71 | 386.51–426.27 | 300 | 300 |
| GPU total (valid) | 65.64–66.61 | 77.09–86.71 | 96.31–107.15 | 330.22–373.49 | 296 | 296 |
| fence wait | 0.00 | 8.52–12.09 | 11.76–14.47 | 37.15–40.66 | 2–3 | 300 |
| present wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording wall | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording join wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |

Excluded total-GPU samples (all counted runs): duplicate 12.

Groom deformation upload per frame, MiB (range over counted runs): p50 11.737, p95 11.943, max 22.323.

Top passes by p50 (valid samples only):

| pass | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---:|---:|---:|---:|---:|
| ShadowPass | 26.271–26.453 | 28.742–29.689 | 31.993–32.265 | 296 | 3 |
| FoliagePass | 24.912–25.076 | 25.981–26.145 | 26.191–26.542 | 296 | 3 |
| GroomPass | 5.984–6.177 | 15.970–23.732 | 33.246–42.453 | 296 | 3 |
| ScenePass | 3.194–3.251 | 3.482–3.622 | 3.532–3.858 | 296 | 3 |
| ScenePrepassPass | 2.922–2.937 | 3.186–3.298 | 3.206–3.398 | 296 | 3 |
| FoliageCull | 0.502–0.609 | 1.850–2.170 | 2.412–3.394 | 296 | 3 |
| VirtualShadowMapMarkPass | 0.287–0.294 | 0.329–0.332 | 0.536–0.578 | 296 | 3 |
| TAAPass | 0.159–0.160 | 0.170–0.173 | 0.379–0.410 | 296 | 3 |
| DiffusionHandoffSnapshotPass | 0.038 | 0.038 | 0.038 | 296 | 3 |
| GroomSceneEntitySnapshotPass | 0.030 | 0.031 | 0.031–0.032 | 296 | 3 |
| DiffusionDepthSnapshotPass | 0.029 | 0.030 | 0.030–0.031 | 296 | 3 |
| GroomSceneDepthSnapshotPass | 0.029 | 0.030 | 0.030–0.031 | 296 | 3 |
| SkinDiffusionPass | 0.027 | 0.031–0.032 | 0.032–0.034 | 296 | 3 |
| ToneMapPass | 0.015 | 0.019 | 0.023–0.029 | 296 | 3 |
| UICompositePass | 0.009 | 0.010 | 0.012–0.015 | 296 | 3 |

Top sub-passes by p50 (valid samples only):

| sub-pass | parent | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---|---:|---:|---:|---:|---:|
| ShadowPass/VsmSun | ShadowPass | 15.933–16.049 | 16.213–17.392 | 17.505–17.841 | 296 | 3 |
| ShadowPass/VsmSun/FoliageCasters | ShadowPass | 8.676–8.783 | 8.822–9.309 | 8.902–9.612 | 296 | 3 |
| ShadowPass/VsmLocal | ShadowPass | 5.439–5.523 | 6.288–6.432 | 6.703–6.923 | 296 | 3 |
| ShadowPass/VsmSun/GroomCasters | ShadowPass | 4.101–4.115 | 4.484–4.914 | 5.669–5.916 | 296 | 3 |
| ScenePass/Color | ScenePass | 3.163–3.207 | 3.308–3.471 | 3.514–3.684 | 296 | 3 |
| ScenePrepassPass/DepthPrepass | ScenePrepassPass | 2.904–2.915 | 2.980–3.161 | 3.181–3.287 | 296 | 3 |
| GroomPass/StrandShade | GroomPass | 2.643 | 2.754–3.015 | 3.160–3.387 | 296 | 3 |
| ShadowPass/VsmLocal/GroomCasters | ShadowPass | 2.478–2.490 | 3.227–3.252 | 3.833–3.856 | 296 | 3 |
| ShadowPass/VsmLocal/FoliageCasters | ShadowPass | 2.290–2.313 | 2.574–2.599 | 2.598–2.773 | 296 | 3 |
| ShadowPass/VsmSun/FoliageCull | ShadowPass | 2.046–2.059 | 2.305–2.357 | 2.322–2.530 | 296 | 3 |

### head-backlit (frames/run 300)

| channel | p50 ms | p95 ms | p99 ms | max ms | gate misses | samples |
|---|---:|---:|---:|---:|---:|---:|
| frame time | 41.67–42.72 | 138.50–150.46 | 150.85–165.23 | 154.72–173.15 | 300 | 300 |
| CPU | 19.51–19.90 | 134.99–148.29 | 146.79–163.96 | 153.60–172.03 | 289–300 | 300 |
| GPU total (valid) | 41.25–42.92 | 134.62–143.36 | 147.23–165.47 | 153.37–172.08 | 296 | 296 |
| fence wait | 19.49–20.57 | 21.95–24.08 | 22.59–24.52 | 23.27–25.08 | 167 | 300 |
| present wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording wall | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording join wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |

Excluded total-GPU samples (all counted runs): duplicate 12.

Groom deformation upload per frame, MiB (range over counted runs): p50 4.595, p95 4.696, max 4.696.

Top passes by p50 (valid samples only):

| pass | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---:|---:|---:|---:|---:|
| ShadowPass | 30.971–32.129 | 32.605–33.476 | 32.953–35.298 | 296 | 3 |
| GroomPass | 4.144–4.169 | 4.466–5.030 | 6.112–6.657 | 296 | 3 |
| ScenePass | 2.095–2.101 | 2.369–2.659 | 2.390–3.122 | 296 | 3 |
| FoliagePass | 1.518–1.543 | 86.159–98.362 | 103.640–110.835 | 296 | 3 |
| ScenePrepassPass | 0.929–0.933 | 0.957–1.275 | 1.205–1.508 | 296 | 3 |
| VirtualShadowMapMarkPass | 0.162–0.167 | 78.350–84.464 | 91.600–108.324 | 296 | 3 |
| SkinDiffusionPass | 0.140 | 0.145–0.147 | 0.150–0.374 | 296 | 3 |
| FoliageCull | 0.132–0.136 | 4.147–4.404 | 4.771–5.026 | 296 | 3 |
| TAAPass | 0.128–0.131 | 0.135–0.138 | 0.145–0.436 | 296 | 3 |
| DiffusionHandoffSnapshotPass | 0.040 | 0.041 | 0.041–0.263 | 296 | 3 |
| DiffusionDepthSnapshotPass | 0.029 | 0.029–0.030 | 0.030 | 296 | 3 |
| GroomSceneDepthSnapshotPass | 0.029 | 0.030 | 0.030–0.031 | 296 | 3 |
| GroomSceneEntitySnapshotPass | 0.023 | 0.024 | 0.024–0.298 | 296 | 3 |
| ToneMapPass | 0.015 | 0.018–0.019 | 0.022–0.264 | 296 | 3 |
| UICompositePass | 0.009–0.010 | 0.010 | 0.010–0.011 | 296 | 3 |

Top sub-passes by p50 (valid samples only):

| sub-pass | parent | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---|---:|---:|---:|---:|---:|
| ShadowPass/VsmLocal | ShadowPass | 24.186–25.453 | 25.507–26.423 | 25.783–26.704 | 296 | 3 |
| ShadowPass/VsmLocal/GroomCasters | ShadowPass | 23.103–24.399 | 24.399–25.327 | 24.529–25.429 | 296 | 3 |
| ShadowPass/VsmSun | ShadowPass | 5.431–5.480 | 5.749–6.422 | 5.791–6.838 | 296 | 3 |
| GroomPass/StrandShade | GroomPass | 2.422–2.428 | 2.505–2.511 | 2.519–2.524 | 296 | 3 |
| ScenePass/Color | ScenePass | 2.092–2.098 | 2.365–2.656 | 2.387–3.119 | 296 | 3 |
| ShadowPass/VsmSun/FoliageCull | ShadowPass | 2.011–2.022 | 2.271–2.521 | 2.287–2.922 | 296 | 3 |
| GroomPass/StrandPrepass | GroomPass | 1.649–1.654 | 1.907–2.278 | 1.931–2.603 | 296 | 3 |
| ShadowPass/VsmSun/GroomCasters | ShadowPass | 1.417–1.446 | 1.676–1.985 | 1.898–2.426 | 296 | 3 |
| ShadowPass/VsmSun/FoliageCasters | ShadowPass | 0.928–0.949 | 1.189–1.368 | 1.213–1.665 | 296 | 3 |
| ScenePrepassPass/DepthPrepass | ScenePrepassPass | 0.916–0.922 | 0.944–1.263 | 1.193–1.495 | 296 | 3 |

### head-closeup (frames/run 300)

| channel | p50 ms | p95 ms | p99 ms | max ms | gate misses | samples |
|---|---:|---:|---:|---:|---:|---:|
| frame time | 54.03–54.56 | 144.27–154.75 | 161.60–181.56 | 180.39–301.88 | 300 | 300 |
| CPU | 27.08–27.90 | 137.09–152.10 | 158.51–180.61 | 179.04–278.70 | 300 | 300 |
| GPU total (valid) | 55.40–56.55 | 136.82–152.78 | 155.54–179.97 | 161.37–269.03 | 292–293 | 292–293 |
| fence wait | 0.01 | 26.54–28.40 | 27.46–29.21 | 28.04–29.62 | 132–142 | 300 |
| present wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording wall | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording join wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |

Excluded total-GPU samples (all counted runs): duplicate 23.

Groom deformation upload per frame, MiB (range over counted runs): p50 8.600, p95 8.600, max 8.600.

Top passes by p50 (valid samples only):

| pass | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---:|---:|---:|---:|---:|
| ShadowPass | 30.552–30.934 | 32.355–33.346 | 32.622–46.544 | 292–293 | 3 |
| GroomPass | 12.530–12.769 | 92.054–106.042 | 110.693–126.679 | 292–293 | 3 |
| ScenePass | 3.757–3.772 | 4.030–4.383 | 4.066–4.599 | 292–293 | 3 |
| FoliagePass | 2.778–2.807 | 3.070–3.193 | 3.141–3.421 | 292–293 | 3 |
| ScenePrepassPass | 2.482–2.484 | 2.749–2.861 | 2.778–3.189 | 292–293 | 3 |
| VirtualShadowMapMarkPass | 0.167–0.181 | 0.170–0.191 | 0.171–0.195 | 292–293 | 3 |
| FoliageCull | 0.138–0.142 | 5.358–6.449 | 6.024–15.062 | 292–293 | 3 |
| SkinDiffusionPass | 0.138–0.139 | 0.148–0.150 | 0.151–0.370 | 292–293 | 3 |
| TAAPass | 0.136 | 0.145–0.146 | 0.151–0.383 | 292–293 | 3 |
| DiffusionHandoffSnapshotPass | 0.040–0.041 | 0.041 | 0.041 | 292–293 | 3 |
| DiffusionDepthSnapshotPass | 0.029 | 0.030 | 0.030 | 292–293 | 3 |
| GroomSceneDepthSnapshotPass | 0.029 | 0.030–0.034 | 0.033–0.035 | 292–293 | 3 |
| GroomSceneEntitySnapshotPass | 0.023–0.024 | 0.031–0.032 | 0.032–0.037 | 292–293 | 3 |
| ToneMapPass | 0.015 | 0.019 | 0.020–0.025 | 292–293 | 3 |
| UICompositePass | 0.009 | 0.010 | 0.010–0.015 | 292–293 | 3 |

Top sub-passes by p50 (valid samples only):

| sub-pass | parent | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---|---:|---:|---:|---:|---:|
| ShadowPass/VsmLocal | ShadowPass | 22.973–23.283 | 24.660–24.855 | 25.021–25.165 | 292–293 | 3 |
| ShadowPass/VsmLocal/GroomCasters | ShadowPass | 20.997–21.538 | 22.730–23.072 | 23.006–23.332 | 292–293 | 3 |
| ShadowPass/VsmSun | ShadowPass | 6.127–6.161 | 6.422–7.002 | 6.454–7.343 | 292–293 | 3 |
| GroomPass/StrandShade | GroomPass | 4.896–4.933 | 5.549–5.565 | 5.581–5.768 | 292–293 | 3 |
| GroomPass/StrandPrepass | GroomPass | 3.969–4.041 | 4.134–4.550 | 4.207–4.775 | 292–293 | 3 |
| ScenePass/Color | ScenePass | 3.753–3.765 | 4.027–4.375 | 4.064–4.588 | 292–293 | 3 |
| GroomPass/RootFrames | GroomPass | 2.998–3.084 | 81.245–95.374 | 100.727–116.680 | 292–293 | 3 |
| GroomPass/RootFrames/Dispatch | GroomPass | 2.658–2.763 | 81.086–95.219 | 100.576–116.521 | 292–293 | 3 |
| ScenePrepassPass/DepthPrepass | ScenePrepassPass | 2.469–2.472 | 2.739–2.849 | 2.765–3.176 | 292–293 | 3 |
| ShadowPass/VsmSun/FoliageCull | ShadowPass | 1.997–2.013 | 2.257–2.501 | 2.263–2.700 | 292–293 | 3 |

### population-dolly (frames/run 300)

| channel | p50 ms | p95 ms | p99 ms | max ms | gate misses | samples |
|---|---:|---:|---:|---:|---:|---:|
| frame time | 52.44–52.81 | 100.77–103.97 | 180.30–184.82 | 211.66–220.61 | 298–300 | 300 |
| CPU | 19.21–19.53 | 65.46–67.79 | 140.60–145.05 | 181.41–188.83 | 279–294 | 300 |
| GPU total (valid) | 54.04–54.63 | 68.33–68.66 | 82.15–87.71 | 135.23–143.32 | 300 | 300 |
| fence wait | 32.13–33.00 | 42.85–44.05 | 45.83–46.86 | 47.06–48.25 | 256–258 | 300 |
| present wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording wall | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording join wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |

Groom deformation upload per frame, MiB (range over counted runs): p50 5.397, p95 7.990, max 25.930.

Top passes by p50 (valid samples only):

| pass | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---:|---:|---:|---:|---:|
| ShadowPass | 20.917–21.019 | 27.588–27.931 | 29.136–29.477 | 300 | 3 |
| FoliagePass | 16.701–16.788 | 18.604–18.647 | 18.765–18.869 | 300 | 3 |
| GroomPass | 8.601–8.792 | 14.475–14.607 | 27.168–33.863 | 300 | 3 |
| ScenePass | 3.329–3.340 | 3.622–3.778 | 3.862–3.895 | 300 | 3 |
| ScenePrepassPass | 3.289–3.326 | 3.624–3.781 | 3.807–3.966 | 300 | 3 |
| FoliageCull | 0.148 | 0.182–0.187 | 5.852–6.664 | 300 | 3 |
| TAAPass | 0.142–0.143 | 0.155–0.158 | 0.159–0.390 | 300 | 3 |
| VirtualShadowMapMarkPass | 0.053 | 0.058–0.059 | 0.060–0.084 | 300 | 3 |
| DiffusionHandoffSnapshotPass | 0.038 | 0.038 | 0.038–0.039 | 300 | 3 |
| DiffusionDepthSnapshotPass | 0.029 | 0.030 | 0.030 | 300 | 3 |
| GroomSceneDepthSnapshotPass | 0.029 | 0.030 | 0.030 | 300 | 3 |
| SkinDiffusionPass | 0.027 | 0.032 | 0.035–0.036 | 300 | 3 |
| GroomSceneEntitySnapshotPass | 0.023 | 0.024–0.029 | 0.031 | 300 | 3 |
| ToneMapPass | 0.016 | 0.019–0.020 | 0.020–0.024 | 300 | 3 |
| UICompositePass | 0.009–0.010 | 0.011 | 0.014–0.015 | 300 | 3 |

Top sub-passes by p50 (valid samples only):

| sub-pass | parent | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---|---:|---:|---:|---:|---:|
| ShadowPass/VsmSun | ShadowPass | 19.328–19.449 | 25.831–26.071 | 26.124–26.488 | 300 | 3 |
| ShadowPass/VsmSun/FoliageCasters | ShadowPass | 11.128–11.382 | 11.920–12.125 | 12.784–12.885 | 300 | 3 |
| ShadowPass/VsmSun/GroomCasters | ShadowPass | 4.726–4.798 | 11.607–11.618 | 11.881–11.909 | 300 | 3 |
| GroomPass/StrandShade | GroomPass | 3.630–3.649 | 6.517–6.577 | 6.952–7.169 | 300 | 3 |
| ScenePass/Color | ScenePass | 3.314–3.335 | 3.615–3.773 | 3.858–3.892 | 300 | 3 |
| ScenePrepassPass/DepthPrepass | ScenePrepassPass | 3.270–3.304 | 3.611–3.766 | 3.795–3.953 | 300 | 3 |
| GroomPass/StrandPrepass | GroomPass | 3.034–3.099 | 5.096–5.226 | 5.493–5.644 | 300 | 3 |
| ShadowPass/VsmSun/FoliageCull | ShadowPass | 2.119–2.131 | 2.371–2.394 | 2.396–2.598 | 300 | 3 |
| GroomPass/RootFrames | GroomPass | 1.919–1.932 | 2.298–2.443 | 3.677–3.740 | 300 | 3 |
| GroomPass/RootFrames/Dispatch | GroomPass | 1.631–1.688 | 1.939–2.062 | 2.247–2.297 | 300 | 3 |

### rapid-turn (frames/run 300)

| channel | p50 ms | p95 ms | p99 ms | max ms | gate misses | samples |
|---|---:|---:|---:|---:|---:|---:|
| frame time | 38.24–38.35 | 60.02–63.48 | 80.87–83.38 | 134.72–137.72 | 289–290 | 300 |
| CPU | 12.80–13.18 | 22.32–25.24 | 54.99–55.41 | 112.78–123.07 | 50–67 | 300 |
| GPU total (valid) | 38.39–38.71 | 56.64–60.11 | 57.46–60.75 | 61.87–76.01 | 300 | 300 |
| fence wait | 24.82–25.19 | 37.65–41.49 | 39.17–43.08 | 39.83–43.92 | 280–281 | 300 |
| present wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording wall | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording join wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |

Groom deformation upload per frame, MiB (range over counted runs): p50 2.501, p95 4.877, max 8.161.

Top passes by p50 (valid samples only):

| pass | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---:|---:|---:|---:|---:|
| ShadowPass | 18.785–19.031 | 26.814–30.092 | 26.972–30.180 | 300 | 3 |
| FoliagePass | 13.539–13.551 | 16.777–16.855 | 17.300–17.415 | 300 | 3 |
| GroomPass | 2.014–2.075 | 5.724–5.773 | 6.007–6.187 | 300 | 3 |
| ScenePass | 1.569–1.582 | 3.560–3.595 | 3.696–3.826 | 300 | 3 |
| ScenePrepassPass | 1.478–1.497 | 3.522–3.564 | 3.681–3.815 | 300 | 3 |
| FoliageCull | 0.132 | 0.155–0.156 | 0.174–0.404 | 300 | 3 |
| TAAPass | 0.130–0.131 | 0.138–0.140 | 0.151–0.380 | 300 | 3 |
| VirtualShadowMapMarkPass | 0.044–0.047 | 0.055–0.056 | 0.057–0.060 | 300 | 3 |
| DiffusionHandoffSnapshotPass | 0.038 | 0.038 | 0.039 | 300 | 3 |
| DiffusionDepthSnapshotPass | 0.029 | 0.030 | 0.033–0.034 | 300 | 3 |
| GroomSceneDepthSnapshotPass | 0.029 | 0.030 | 0.030 | 300 | 3 |
| SkinDiffusionPass | 0.027 | 0.032 | 0.035 | 300 | 3 |
| GroomSceneEntitySnapshotPass | 0.023 | 0.023 | 0.032 | 300 | 3 |
| ToneMapPass | 0.015 | 0.019 | 0.019–0.020 | 300 | 3 |
| UICompositePass | 0.009–0.010 | 0.011–0.012 | 0.013–0.015 | 300 | 3 |

Top sub-passes by p50 (valid samples only):

| sub-pass | parent | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---|---:|---:|---:|---:|---:|
| ShadowPass/VsmSun | ShadowPass | 16.564–16.812 | 17.136–17.455 | 17.194–17.590 | 300 | 3 |
| ShadowPass/VsmSun/FoliageCasters | ShadowPass | 12.415–12.622 | 12.751–13.074 | 12.820–13.156 | 300 | 3 |
| ShadowPass/VsmSun/FoliageCull | ShadowPass | 2.136–2.138 | 2.386–2.401 | 2.406–2.601 | 300 | 3 |
| ScenePass/Color | ScenePass | 1.554–1.562 | 3.557–3.593 | 3.674–3.820 | 300 | 3 |
| ScenePrepassPass/DepthPrepass | ScenePrepassPass | 1.459–1.473 | 3.507–3.551 | 3.668–3.803 | 300 | 3 |
| GroomPass/StrandShade | GroomPass | 0.991–0.993 | 1.942–1.944 | 1.976–2.015 | 300 | 3 |
| ShadowPass/VsmLocal | ShadowPass | 0.965–0.969 | 8.236–11.514 | 8.282–11.540 | 300 | 3 |
| ShadowPass/VsmSun/GroomCasters | ShadowPass | 0.894–0.896 | 1.140–1.175 | 1.310–1.398 | 300 | 3 |
| ShadowPass/VsmSun/TerrainCasters | ShadowPass | 0.876–0.887 | 0.890–0.900 | 0.907–0.926 | 300 | 3 |
| GroomPass/StrandPrepass | GroomPass | 0.674–0.676 | 1.809–1.816 | 2.033–2.245 | 300 | 3 |

Excluded per-pass samples (all counted runs): duplicate-frame-pass 2044.

Memory: peak GPU resident 2079.6–2083.2 MiB, live GPU 1784.4–1788.0 MiB, peak CPU tracked 93.2 MiB, GPU after scene release 1784.4–1788.0 MiB.
Report: GPU peak 3123.1–3123.7 MiB / live 1781.9–1785.4 MiB; CPU peak 93.2 MiB / live 93.2 MiB; reconciliation notObservable; residency unknown.
Top GPU owners (first counted run): ShadowPass 602.5 MiB, GroomPass 408.2 MiB, Unattributed 177.6 MiB, TransientPool 174.0 MiB, RendererInit 144.3 MiB, Foliage pinned representations 129.0 MiB, ShadowMap 80.0 MiB, TemporalHistory 35.6 MiB, Texture2D 21.3 MiB, EnvironmentMap 4.1 MiB.

## gl-forward-repeat: FAIL

run 1 coats-closeup p95 87.383 ms > 16.667 ms; run 1 head-backlit p95 133.073 ms > 16.667 ms; run 1 head-closeup p95 148.408 ms > 16.667 ms; run 1 population-dolly p95 105.959 ms > 16.667 ms; run 1 rapid-turn p95 62.728 ms > 16.667 ms; run 2 coats-closeup p95 95.055 ms > 16.667 ms; run 2 head-backlit p95 195.600 ms > 16.667 ms; run 2 head-closeup p95 207.786 ms > 16.667 ms; run 2 population-dolly p95 103.043 ms > 16.667 ms; run 2 rapid-turn p95 68.293 ms > 16.667 ms; run 3 coats-closeup p95 85.992 ms > 16.667 ms; run 3 head-backlit p95 141.010 ms > 16.667 ms; run 3 head-closeup p95 150.422 ms > 16.667 ms; run 3 population-dolly p95 101.545 ms > 16.667 ms; run 3 rapid-turn p95 63.386 ms > 16.667 ms. Counted runs: 3 of 3.

Host: test-binary / opengl. Frame time = wall-clock Scene::OnUpdateEditor call, excluding attachment readback.
Internal 1920x1080, display 1920x1080 (native); upscaler resolved/fallback: {"fallback": "none", "resolved": "native"}.
Selected: lightingTap=None, msaaSamples=1, path=Forward, rayTracedShadowsRequested=False, skinDiffusion=True, upscaleMode=0, upscaleTechnique=0, virtualShadowMaps=True; VSM selected: True; pinned by manifest: MSAASampleCount, Path, RayTracedShadowsEnabled, Upscale, UpscaleTechnique, VirtualShadowMaps.
Selected settings are what the renderer was set to, not proof each technique produced or consumed work.

### coats-closeup (frames/run 300)

| channel | p50 ms | p95 ms | p99 ms | max ms | gate misses | samples |
|---|---:|---:|---:|---:|---:|---:|
| frame time | 67.48–79.67 | 85.99–95.05 | 102.08–140.41 | 366.97–414.52 | 300 | 300 |
| CPU | 66.47–78.43 | 80.85–93.68 | 101.19–135.94 | 366.09–413.55 | 300 | 300 |
| GPU total (valid) | 64.89–73.27 | 76.14–87.63 | 92.15–126.10 | 312.11–385.27 | 293–298 | 293–298 |
| fence wait | 0.00 | 3.46–12.60 | 7.43–15.29 | 37.47–109.70 | 2 | 300 |
| present wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording wall | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording join wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |

Excluded total-GPU samples (all counted runs): duplicate 12.

Groom deformation upload per frame, MiB (range over counted runs): p50 11.737, p95 11.943, max 22.323.

Top passes by p50 (valid samples only):

| pass | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---:|---:|---:|---:|---:|
| ShadowPass | 25.440–27.872 | 28.674–33.132 | 32.110–37.385 | 293–298 | 3 |
| FoliagePass | 24.836–25.158 | 25.954–26.366 | 26.172–26.851 | 293–298 | 3 |
| GroomPass | 5.978–10.742 | 14.940–23.278 | 28.789–59.714 | 293–298 | 3 |
| ScenePass | 3.168–3.253 | 3.508–3.635 | 3.534–4.025 | 293–298 | 3 |
| ScenePrepassPass | 2.914–2.933 | 3.173–3.337 | 3.228–3.706 | 293–298 | 3 |
| FoliageCull | 0.459–0.602 | 1.816–2.498 | 2.281–3.881 | 293–298 | 3 |
| VirtualShadowMapMarkPass | 0.275–0.286 | 0.325–0.335 | 0.501–0.510 | 293–298 | 3 |
| TAAPass | 0.160–0.161 | 0.167–0.173 | 0.188–0.409 | 293–298 | 3 |
| DiffusionHandoffSnapshotPass | 0.038 | 0.038 | 0.038–0.039 | 293–298 | 3 |
| GroomSceneEntitySnapshotPass | 0.030 | 0.031 | 0.031–0.032 | 293–298 | 3 |
| DiffusionDepthSnapshotPass | 0.029 | 0.030 | 0.030–0.031 | 293–298 | 3 |
| GroomSceneDepthSnapshotPass | 0.029 | 0.030 | 0.030–0.341 | 293–298 | 3 |
| SkinDiffusionPass | 0.027 | 0.031 | 0.032–0.039 | 293–298 | 3 |
| ToneMapPass | 0.015–0.016 | 0.019 | 0.020–0.023 | 293–298 | 3 |
| UICompositePass | 0.009–0.010 | 0.010 | 0.011–0.015 | 293–298 | 3 |

Top sub-passes by p50 (valid samples only):

| sub-pass | parent | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---|---:|---:|---:|---:|---:|
| ShadowPass/VsmSun | ShadowPass | 15.866–16.121 | 16.237–17.270 | 17.511–18.188 | 293–298 | 3 |
| ShadowPass/VsmSun/FoliageCasters | ShadowPass | 8.613–8.794 | 8.826–9.339 | 8.905–9.912 | 293–298 | 3 |
| ShadowPass/VsmLocal | ShadowPass | 5.409–5.596 | 6.315–6.666 | 6.693–7.183 | 293–298 | 3 |
| GroomPass/RootFrames | GroomPass | 1.015–5.368 | 8.769–17.710 | 22.892–53.914 | 293–298 | 3 |
| GroomPass/RootFrames/Dispatch | GroomPass | 0.899–5.177 | 8.718–17.652 | 22.828–53.857 | 293–298 | 3 |
| ShadowPass/VsmSun/GroomCasters | ShadowPass | 4.085–4.124 | 4.490–4.855 | 5.707–5.899 | 293–298 | 3 |
| ScenePass/Color | ScenePass | 3.158–3.170 | 3.328–3.527 | 3.525–3.832 | 293–298 | 3 |
| ScenePrepassPass/DepthPrepass | ScenePrepassPass | 2.898–2.911 | 2.948–3.163 | 3.170–3.493 | 293–298 | 3 |
| GroomPass/StrandShade | GroomPass | 2.642–2.643 | 2.771–2.937 | 3.155–3.186 | 293–298 | 3 |
| ShadowPass/VsmLocal/GroomCasters | ShadowPass | 2.326–2.580 | 3.255–3.393 | 3.831–4.037 | 293–298 | 3 |

### head-backlit (frames/run 300)

| channel | p50 ms | p95 ms | p99 ms | max ms | gate misses | samples |
|---|---:|---:|---:|---:|---:|---:|
| frame time | 40.78–44.42 | 133.07–195.60 | 147.04–205.68 | 156.87–208.57 | 300 | 300 |
| CPU | 19.29–32.74 | 130.19–193.38 | 142.12–203.23 | 155.97–207.07 | 296–300 | 300 |
| GPU total (valid) | 40.80–42.15 | 131.90–191.54 | 147.14–201.77 | 151.43–207.62 | 291–296 | 291–296 |
| fence wait | 0.01–20.18 | 17.12–22.71 | 18.28–23.42 | 18.95–23.69 | 27–167 | 300 |
| present wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording wall | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording join wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |

Excluded total-GPU samples (all counted runs): duplicate 17.

Groom deformation upload per frame, MiB (range over counted runs): p50 4.595, p95 4.696, max 4.696.

Top passes by p50 (valid samples only):

| pass | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---:|---:|---:|---:|---:|
| ShadowPass | 30.091–31.653 | 32.386–40.349 | 33.776–42.573 | 291–296 | 3 |
| GroomPass | 4.145–4.163 | 4.528–6.141 | 6.224–7.579 | 291–296 | 3 |
| ScenePass | 2.093–2.099 | 2.367–2.975 | 2.385–3.425 | 291–296 | 3 |
| FoliagePass | 1.514–1.521 | 87.523–132.785 | 99.170–144.209 | 291–296 | 3 |
| ScenePrepassPass | 0.927–0.933 | 1.140–1.288 | 1.208–1.444 | 291–296 | 3 |
| VirtualShadowMapMarkPass | 0.154–0.162 | 61.587–120.311 | 78.227–142.676 | 291–296 | 3 |
| SkinDiffusionPass | 0.140 | 0.146–0.196 | 0.150–0.223 | 291–296 | 3 |
| FoliageCull | 0.134–0.136 | 3.966–7.244 | 4.403–8.996 | 291–296 | 3 |
| TAAPass | 0.130–0.131 | 0.137–0.179 | 0.145–0.200 | 291–296 | 3 |
| DiffusionHandoffSnapshotPass | 0.040 | 0.041–0.050 | 0.041–0.353 | 291–296 | 3 |
| DiffusionDepthSnapshotPass | 0.029 | 0.029–0.037 | 0.030–0.041 | 291–296 | 3 |
| GroomSceneDepthSnapshotPass | 0.029 | 0.030–0.038 | 0.030–0.041 | 291–296 | 3 |
| GroomSceneEntitySnapshotPass | 0.023 | 0.024–0.029 | 0.024–0.031 | 291–296 | 3 |
| ToneMapPass | 0.015 | 0.018–0.022 | 0.022–0.025 | 291–296 | 3 |
| UICompositePass | 0.009–0.010 | 0.010–0.014 | 0.011–0.015 | 291–296 | 3 |

Top sub-passes by p50 (valid samples only):

| sub-pass | parent | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---|---:|---:|---:|---:|---:|
| ShadowPass/VsmLocal | ShadowPass | 23.557–24.985 | 25.223–31.039 | 26.122–32.191 | 291–296 | 3 |
| ShadowPass/VsmLocal/GroomCasters | ShadowPass | 22.463–23.893 | 23.932–29.599 | 24.948–30.374 | 291–296 | 3 |
| ShadowPass/VsmSun | ShadowPass | 5.334–5.468 | 5.776–7.342 | 5.807–8.314 | 291–296 | 3 |
| GroomPass/StrandShade | GroomPass | 2.426–2.442 | 2.507–3.144 | 2.520–3.629 | 291–296 | 3 |
| ScenePass/Color | ScenePass | 2.090–2.097 | 2.364–2.972 | 2.382–3.421 | 291–296 | 3 |
| ShadowPass/VsmSun/FoliageCull | ShadowPass | 2.009–2.019 | 2.273–2.643 | 2.292–3.095 | 291–296 | 3 |
| GroomPass/StrandPrepass | GroomPass | 1.641–1.661 | 1.916–2.230 | 1.946–2.636 | 291–296 | 3 |
| ShadowPass/VsmSun/GroomCasters | ShadowPass | 1.437–1.443 | 1.677–2.228 | 1.917–2.706 | 291–296 | 3 |
| ShadowPass/VsmSun/FoliageCasters | ShadowPass | 0.889–0.948 | 1.102–1.202 | 1.220–1.630 | 291–296 | 3 |
| ScenePrepassPass/DepthPrepass | ScenePrepassPass | 0.914–0.922 | 1.128–1.276 | 1.196–1.430 | 291–296 | 3 |

### head-closeup (frames/run 300)

| channel | p50 ms | p95 ms | p99 ms | max ms | gate misses | samples |
|---|---:|---:|---:|---:|---:|---:|
| frame time | 53.16–56.63 | 148.41–207.79 | 163.75–223.53 | 172.46–268.68 | 300 | 300 |
| CPU | 25.73–39.74 | 143.97–205.52 | 162.87–222.29 | 171.32–266.63 | 300 | 300 |
| GPU total (valid) | 54.43–57.40 | 141.53–206.51 | 161.18–221.33 | 171.52–241.19 | 292–294 | 292–294 |
| fence wait | 0.01 | 20.60–27.83 | 22.10–28.48 | 23.03–29.17 | 77–143 | 300 |
| present wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording wall | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording join wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |

Excluded total-GPU samples (all counted runs): duplicate 22.

Groom deformation upload per frame, MiB (range over counted runs): p50 8.600, p95 8.600, max 8.600.

Top passes by p50 (valid samples only):

| pass | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---:|---:|---:|---:|---:|
| ShadowPass | 29.526–30.277 | 32.079–39.635 | 32.657–39.945 | 292–294 | 3 |
| GroomPass | 12.341–12.800 | 96.780–150.571 | 117.436–169.633 | 292–294 | 3 |
| ScenePass | 3.758–3.771 | 4.034–5.255 | 4.051–5.679 | 292–294 | 3 |
| FoliagePass | 2.769–2.793 | 3.027–4.043 | 3.141–4.616 | 292–294 | 3 |
| ScenePrepassPass | 2.482–2.493 | 2.743–3.390 | 2.766–3.635 | 292–294 | 3 |
| VirtualShadowMapMarkPass | 0.165–0.175 | 0.178–0.194 | 0.181–0.198 | 292–294 | 3 |
| FoliageCull | 0.139–0.144 | 4.939–11.026 | 6.085–13.833 | 292–294 | 3 |
| SkinDiffusionPass | 0.139–0.140 | 0.150–0.205 | 0.155–0.389 | 292–294 | 3 |
| TAAPass | 0.136–0.138 | 0.146–0.188 | 0.153–0.461 | 292–294 | 3 |
| DiffusionHandoffSnapshotPass | 0.040 | 0.041–0.051 | 0.041–0.053 | 292–294 | 3 |
| DiffusionDepthSnapshotPass | 0.029 | 0.030–0.038 | 0.030–0.041 | 292–294 | 3 |
| GroomSceneDepthSnapshotPass | 0.029 | 0.033–0.038 | 0.035–0.040 | 292–294 | 3 |
| GroomSceneEntitySnapshotPass | 0.024 | 0.031–0.038 | 0.032–0.039 | 292–294 | 3 |
| ToneMapPass | 0.015 | 0.019–0.023 | 0.022–0.028 | 292–294 | 3 |
| UICompositePass | 0.009–0.010 | 0.010–0.014 | 0.010–0.015 | 292–294 | 3 |

Top sub-passes by p50 (valid samples only):

| sub-pass | parent | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---|---:|---:|---:|---:|---:|
| ShadowPass/VsmLocal | ShadowPass | 22.026–22.723 | 24.297–29.296 | 24.581–29.751 | 292–294 | 3 |
| ShadowPass/VsmLocal/GroomCasters | ShadowPass | 20.289–20.942 | 22.484–27.077 | 22.831–27.526 | 292–294 | 3 |
| ShadowPass/VsmSun | ShadowPass | 6.044–6.154 | 6.431–8.275 | 6.472–8.699 | 292–294 | 3 |
| GroomPass/StrandShade | GroomPass | 4.890–5.289 | 5.546–6.836 | 5.583–7.401 | 292–294 | 3 |
| GroomPass/StrandPrepass | GroomPass | 3.882–3.971 | 4.135–5.398 | 4.200–5.799 | 292–294 | 3 |
| ScenePass/Color | ScenePass | 3.754–3.768 | 4.030–5.250 | 4.048–5.676 | 292–294 | 3 |
| GroomPass/RootFrames | GroomPass | 2.825–3.006 | 86.031–136.816 | 107.059–155.524 | 292–294 | 3 |
| GroomPass/RootFrames/Dispatch | GroomPass | 2.642–2.664 | 85.868–136.655 | 106.897–155.351 | 292–294 | 3 |
| ScenePrepassPass/DepthPrepass | ScenePrepassPass | 2.470–2.481 | 2.730–3.372 | 2.753–3.617 | 292–294 | 3 |
| ShadowPass/VsmSun/FoliageCull | ShadowPass | 1.982–1.999 | 2.263–2.634 | 2.268–2.781 | 292–294 | 3 |

### population-dolly (frames/run 300)

| channel | p50 ms | p95 ms | p99 ms | max ms | gate misses | samples |
|---|---:|---:|---:|---:|---:|---:|
| frame time | 52.66–53.95 | 101.55–105.96 | 182.90–186.01 | 218.94–239.54 | 300 | 300 |
| CPU | 19.48–19.79 | 67.32–69.28 | 141.04–144.64 | 181.37–208.39 | 292–297 | 300 |
| GPU total (valid) | 54.17–55.60 | 68.27–70.96 | 82.36–87.72 | 133.59–162.67 | 300 | 300 |
| fence wait | 32.09–33.59 | 42.81–45.63 | 46.39–47.70 | 46.95–50.01 | 255–256 | 300 |
| present wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording wall | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording join wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |

Groom deformation upload per frame, MiB (range over counted runs): p50 5.397, p95 7.990, max 25.930.

Top passes by p50 (valid samples only):

| pass | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---:|---:|---:|---:|---:|
| ShadowPass | 20.855–21.294 | 27.623–28.616 | 29.135–30.152 | 300 | 3 |
| FoliagePass | 16.709–17.176 | 18.554–19.135 | 18.727–19.706 | 300 | 3 |
| GroomPass | 8.600–8.868 | 14.465–15.156 | 25.207–34.336 | 300 | 3 |
| ScenePass | 3.328–3.366 | 3.739–4.270 | 3.876–4.557 | 300 | 3 |
| ScenePrepassPass | 3.309–3.352 | 3.627–4.138 | 3.784–4.315 | 300 | 3 |
| FoliageCull | 0.147–0.150 | 0.182–0.186 | 6.105–6.236 | 300 | 3 |
| TAAPass | 0.143–0.144 | 0.154–0.158 | 0.164–0.468 | 300 | 3 |
| VirtualShadowMapMarkPass | 0.054–0.055 | 0.060–0.062 | 0.062–0.065 | 300 | 3 |
| DiffusionHandoffSnapshotPass | 0.038 | 0.038–0.039 | 0.039 | 300 | 3 |
| DiffusionDepthSnapshotPass | 0.029 | 0.030 | 0.030 | 300 | 3 |
| GroomSceneDepthSnapshotPass | 0.029 | 0.030 | 0.030–0.243 | 300 | 3 |
| SkinDiffusionPass | 0.027 | 0.032 | 0.034 | 300 | 3 |
| GroomSceneEntitySnapshotPass | 0.023 | 0.023–0.030 | 0.031–0.038 | 300 | 3 |
| ToneMapPass | 0.015–0.016 | 0.019–0.020 | 0.023–0.025 | 300 | 3 |
| UICompositePass | 0.009–0.010 | 0.011–0.012 | 0.013–0.014 | 300 | 3 |

Top sub-passes by p50 (valid samples only):

| sub-pass | parent | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---|---:|---:|---:|---:|---:|
| ShadowPass/VsmSun | ShadowPass | 19.304–19.688 | 25.937–26.626 | 26.156–27.492 | 300 | 3 |
| ShadowPass/VsmSun/FoliageCasters | ShadowPass | 11.136–11.689 | 11.921–12.634 | 12.809–13.256 | 300 | 3 |
| ShadowPass/VsmSun/GroomCasters | ShadowPass | 4.712–4.787 | 11.581–11.648 | 11.902–12.454 | 300 | 3 |
| GroomPass/StrandShade | GroomPass | 3.627–3.658 | 6.530–6.711 | 6.964–7.014 | 300 | 3 |
| ScenePass/Color | ScenePass | 3.320–3.359 | 3.737–4.254 | 3.873–4.550 | 300 | 3 |
| ScenePrepassPass/DepthPrepass | ScenePrepassPass | 3.288–3.336 | 3.613–4.125 | 3.770–4.298 | 300 | 3 |
| GroomPass/StrandPrepass | GroomPass | 3.037–3.165 | 5.098–5.563 | 5.522–6.330 | 300 | 3 |
| ShadowPass/VsmSun/FoliageCull | ShadowPass | 2.122–2.132 | 2.388–2.541 | 2.409–2.789 | 300 | 3 |
| GroomPass/RootFrames | GroomPass | 1.927–1.939 | 2.333–2.677 | 3.700–3.984 | 300 | 3 |
| GroomPass/RootFrames/Dispatch | GroomPass | 1.625–1.694 | 2.070–2.318 | 2.141–2.702 | 300 | 3 |

### rapid-turn (frames/run 300)

| channel | p50 ms | p95 ms | p99 ms | max ms | gate misses | samples |
|---|---:|---:|---:|---:|---:|---:|
| frame time | 38.03–39.09 | 62.73–68.29 | 83.88–96.76 | 134.07–161.64 | 285–288 | 300 |
| CPU | 13.04–13.33 | 23.86–24.50 | 54.91–60.70 | 113.24–119.84 | 59–67 | 300 |
| GPU total (valid) | 38.25–39.36 | 59.52–60.53 | 60.37–63.03 | 62.40–162.21 | 300 | 300 |
| fence wait | 24.63–25.36 | 40.62–42.02 | 42.28–43.60 | 43.40–141.47 | 281 | 300 |
| present wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording wall | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording join wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |

Groom deformation upload per frame, MiB (range over counted runs): p50 2.501, p95 4.877, max 8.161.

Top passes by p50 (valid samples only):

| pass | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---:|---:|---:|---:|---:|
| ShadowPass | 18.722–19.279 | 29.110–30.879 | 29.434–31.743 | 300 | 3 |
| FoliagePass | 13.493–13.816 | 16.848–17.157 | 17.443–17.544 | 300 | 3 |
| GroomPass | 2.003–2.113 | 5.682–5.912 | 5.977–6.410 | 300 | 3 |
| ScenePass | 1.582–1.596 | 3.540–3.586 | 3.662–3.880 | 300 | 3 |
| ScenePrepassPass | 1.480–1.555 | 3.538–3.591 | 3.744–4.011 | 300 | 3 |
| FoliageCull | 0.132–0.134 | 0.155–0.157 | 0.398–0.485 | 300 | 3 |
| TAAPass | 0.131 | 0.137–0.141 | 0.140–0.382 | 300 | 3 |
| VirtualShadowMapMarkPass | 0.046–0.050 | 0.054–0.061 | 0.057–0.063 | 300 | 3 |
| DiffusionHandoffSnapshotPass | 0.038 | 0.038 | 0.039 | 300 | 3 |
| DiffusionDepthSnapshotPass | 0.029 | 0.030 | 0.033–0.034 | 300 | 3 |
| GroomSceneDepthSnapshotPass | 0.029 | 0.030 | 0.030–0.031 | 300 | 3 |
| SkinDiffusionPass | 0.027 | 0.032 | 0.034–0.048 | 300 | 3 |
| GroomSceneEntitySnapshotPass | 0.023 | 0.024 | 0.032 | 300 | 3 |
| ToneMapPass | 0.015 | 0.019 | 0.020–0.022 | 300 | 3 |
| UICompositePass | 0.009–0.010 | 0.010–0.011 | 0.013 | 300 | 3 |

Top sub-passes by p50 (valid samples only):

| sub-pass | parent | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---|---:|---:|---:|---:|---:|
| ShadowPass/VsmSun | ShadowPass | 16.537–16.978 | 17.151–17.638 | 17.218–18.520 | 300 | 3 |
| ShadowPass/VsmSun/FoliageCasters | ShadowPass | 12.411–12.741 | 12.788–13.177 | 12.843–13.502 | 300 | 3 |
| ShadowPass/VsmSun/FoliageCull | ShadowPass | 2.136–2.140 | 2.397–2.676 | 2.419–2.883 | 300 | 3 |
| ScenePass/Color | ScenePass | 1.558–1.566 | 3.536–3.583 | 3.659–3.814 | 300 | 3 |
| ScenePrepassPass/DepthPrepass | ScenePrepassPass | 1.451–1.477 | 3.526–3.572 | 3.729–3.985 | 300 | 3 |
| GroomPass/StrandShade | GroomPass | 0.992–1.001 | 1.945–1.950 | 1.997–2.019 | 300 | 3 |
| ShadowPass/VsmLocal | ShadowPass | 0.966–0.970 | 10.128–12.090 | 10.351–12.490 | 300 | 3 |
| ShadowPass/VsmSun/GroomCasters | ShadowPass | 0.889–0.896 | 1.046–1.267 | 1.183–1.629 | 300 | 3 |
| ShadowPass/VsmSun/TerrainCasters | ShadowPass | 0.889–0.895 | 0.900–0.918 | 0.930–0.934 | 300 | 3 |
| GroomPass/StrandPrepass | GroomPass | 0.669–0.680 | 1.792–1.834 | 2.046–2.375 | 300 | 3 |

Excluded per-pass samples (all counted runs): duplicate-frame-pass 2210.

Memory: peak GPU resident 2083.2 MiB, live GPU 1788.0 MiB, peak CPU tracked 93.2 MiB, GPU after scene release 1788.0 MiB.
Report: GPU peak 3123.1 MiB / live 1785.4 MiB; CPU peak 93.2 MiB / live 93.2 MiB; reconciliation notObservable; residency unknown.
Top GPU owners (first counted run): ShadowPass 602.5 MiB, GroomPass 408.2 MiB, Unattributed 177.6 MiB, TransientPool 174.0 MiB, RendererInit 144.3 MiB, Foliage pinned representations 129.0 MiB, ShadowMap 80.0 MiB, TemporalHistory 35.6 MiB, Texture2D 25.3 MiB, EnvironmentMap 4.1 MiB.

## gl-forward-plus: FAIL

run 1 coats-closeup p95 88.930 ms > 16.667 ms; run 1 head-backlit p95 140.385 ms > 16.667 ms; run 1 head-closeup p95 157.650 ms > 16.667 ms; run 1 population-dolly p95 106.312 ms > 16.667 ms; run 1 rapid-turn p95 64.797 ms > 16.667 ms; run 2 coats-closeup p95 92.176 ms > 16.667 ms; run 2 head-backlit p95 136.066 ms > 16.667 ms; run 2 head-closeup p95 148.657 ms > 16.667 ms; run 2 population-dolly p95 103.120 ms > 16.667 ms; run 2 rapid-turn p95 64.190 ms > 16.667 ms; run 3 coats-closeup p95 87.322 ms > 16.667 ms; run 3 head-backlit p95 139.820 ms > 16.667 ms; run 3 head-closeup p95 149.240 ms > 16.667 ms; run 3 population-dolly p95 102.922 ms > 16.667 ms; run 3 rapid-turn p95 63.004 ms > 16.667 ms. Counted runs: 3 of 3.

Host: test-binary / opengl. Frame time = wall-clock Scene::OnUpdateEditor call, excluding attachment readback.
Internal 1920x1080, display 1920x1080 (native); upscaler resolved/fallback: {"fallback": "none", "resolved": "native"}.
Selected: lightingTap=None, msaaSamples=1, path=ForwardPlus, rayTracedShadowsRequested=False, skinDiffusion=True, upscaleMode=0, upscaleTechnique=0, virtualShadowMaps=True; VSM selected: True; pinned by manifest: MSAASampleCount, Path, RayTracedShadowsEnabled, Upscale, UpscaleTechnique, VirtualShadowMaps.
Selected settings are what the renderer was set to, not proof each technique produced or consumed work.

### coats-closeup (frames/run 300)

| channel | p50 ms | p95 ms | p99 ms | max ms | gate misses | samples |
|---|---:|---:|---:|---:|---:|---:|
| frame time | 67.86–69.05 | 87.32–92.18 | 112.36–126.88 | 386.08–393.93 | 300 | 300 |
| CPU | 66.73–67.76 | 81.19–88.97 | 105.83–126.00 | 378.66–383.12 | 300 | 300 |
| GPU total (valid) | 65.59–67.22 | 78.64–83.20 | 100.24–150.40 | 323.03–327.77 | 297–298 | 297–298 |
| fence wait | 0.00 | 11.02–12.48 | 13.09–36.50 | 37.34–134.75 | 2–4 | 300 |
| present wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording wall | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording join wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |

Excluded total-GPU samples (all counted runs): duplicate 7.

Groom deformation upload per frame, MiB (range over counted runs): p50 11.737, p95 11.943, max 22.323.

Top passes by p50 (valid samples only):

| pass | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---:|---:|---:|---:|---:|
| ShadowPass | 26.135–26.788 | 29.497–30.929 | 32.457–34.190 | 297–298 | 3 |
| FoliagePass | 24.961–25.306 | 25.996–26.680 | 26.169–27.100 | 297–298 | 3 |
| GroomPass | 6.002–6.213 | 18.045–20.423 | 36.830–55.416 | 297–298 | 3 |
| ScenePass | 3.125–3.164 | 3.405–3.859 | 3.468–4.000 | 297–298 | 3 |
| ScenePrepassPass | 2.918–2.955 | 3.180–3.558 | 3.204–3.722 | 297–298 | 3 |
| FoliageCull | 0.529–0.574 | 1.931–2.209 | 2.216–2.989 | 297–298 | 3 |
| VirtualShadowMapMarkPass | 0.283–0.310 | 0.326–0.355 | 0.584–1.052 | 297–298 | 3 |
| TAAPass | 0.160–0.161 | 0.172–0.175 | 0.411–0.592 | 297–298 | 3 |
| DiffusionHandoffSnapshotPass | 0.038 | 0.038 | 0.038–0.254 | 297–298 | 3 |
| GroomSceneEntitySnapshotPass | 0.030 | 0.031 | 0.031–0.032 | 297–298 | 3 |
| DiffusionDepthSnapshotPass | 0.029 | 0.030 | 0.030 | 297–298 | 3 |
| GroomSceneDepthSnapshotPass | 0.029 | 0.030 | 0.030 | 297–298 | 3 |
| SkinDiffusionPass | 0.027 | 0.032 | 0.033–0.035 | 297–298 | 3 |
| ToneMapPass | 0.015–0.016 | 0.019 | 0.020–0.220 | 297–298 | 3 |
| UICompositePass | 0.009 | 0.010 | 0.013 | 297–298 | 3 |

Top sub-passes by p50 (valid samples only):

| sub-pass | parent | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---|---:|---:|---:|---:|---:|
| ShadowPass/VsmSun | ShadowPass | 15.977–16.333 | 16.292–17.281 | 17.541–18.306 | 297–298 | 3 |
| ShadowPass/VsmSun/FoliageCasters | ShadowPass | 8.723–8.977 | 8.866–9.459 | 8.897–9.840 | 297–298 | 3 |
| ShadowPass/VsmLocal | ShadowPass | 5.368–5.817 | 6.292–6.710 | 6.882–7.064 | 297–298 | 3 |
| ShadowPass/VsmSun/GroomCasters | ShadowPass | 4.089–4.111 | 4.487–5.226 | 5.717–5.950 | 297–298 | 3 |
| ScenePass/Color | ScenePass | 3.068–3.084 | 3.266–3.548 | 3.417–3.852 | 297–298 | 3 |
| ScenePrepassPass/DepthPrepass | ScenePrepassPass | 2.902–2.937 | 3.094–3.183 | 3.189–3.517 | 297–298 | 3 |
| ShadowPass/VsmLocal/GroomCasters | ShadowPass | 2.472–2.673 | 3.239–3.617 | 3.855–3.858 | 297–298 | 3 |
| GroomPass/StrandShade | GroomPass | 2.642–2.654 | 2.776–3.119 | 3.162–3.599 | 297–298 | 3 |
| ShadowPass/VsmLocal/FoliageCasters | ShadowPass | 2.293–2.314 | 2.589–2.803 | 2.598–3.196 | 297–298 | 3 |
| ShadowPass/VsmSun/FoliageCull | ShadowPass | 2.052–2.063 | 2.309–2.548 | 2.328–2.822 | 297–298 | 3 |

### head-backlit (frames/run 300)

| channel | p50 ms | p95 ms | p99 ms | max ms | gate misses | samples |
|---|---:|---:|---:|---:|---:|---:|
| frame time | 42.13–42.61 | 136.07–140.38 | 149.37–152.73 | 150.98–161.41 | 300 | 300 |
| CPU | 19.53–20.16 | 134.39–136.84 | 144.97–148.11 | 150.09–152.85 | 296–300 | 300 |
| GPU total (valid) | 41.71–42.57 | 132.68–136.81 | 145.32–153.23 | 149.75–153.70 | 296 | 296 |
| fence wait | 19.01–20.10 | 22.14–23.31 | 22.75–23.78 | 22.97–24.34 | 164–167 | 300 |
| present wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording wall | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording join wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |

Excluded total-GPU samples (all counted runs): duplicate 12.

Groom deformation upload per frame, MiB (range over counted runs): p50 4.595, p95 4.696, max 4.696.

Top passes by p50 (valid samples only):

| pass | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---:|---:|---:|---:|---:|
| ShadowPass | 31.380–31.790 | 33.954–43.037 | 34.569–44.543 | 296 | 3 |
| GroomPass | 4.171–4.243 | 4.563–6.667 | 5.275–7.413 | 296 | 3 |
| ScenePass | 1.961–1.972 | 77.117–80.718 | 94.105–108.416 | 296 | 3 |
| FoliagePass | 1.519–1.548 | 85.012–86.607 | 97.596–99.343 | 296 | 3 |
| ScenePrepassPass | 0.926–0.932 | 0.952–1.435 | 1.206–2.004 | 296 | 3 |
| VirtualShadowMapMarkPass | 0.162–0.175 | 0.165–0.211 | 0.166–0.217 | 296 | 3 |
| SkinDiffusionPass | 0.140–0.141 | 0.147–0.225 | 0.153–0.389 | 296 | 3 |
| FoliageCull | 0.131–0.135 | 3.809–4.047 | 4.278–4.799 | 296 | 3 |
| TAAPass | 0.128–0.132 | 0.134–0.204 | 0.136–0.604 | 296 | 3 |
| DiffusionHandoffSnapshotPass | 0.040 | 0.041–0.053 | 0.041–0.371 | 296 | 3 |
| DiffusionDepthSnapshotPass | 0.029 | 0.030–0.040 | 0.030–0.042 | 296 | 3 |
| GroomSceneDepthSnapshotPass | 0.029 | 0.030–0.041 | 0.030–0.042 | 296 | 3 |
| GroomSceneEntitySnapshotPass | 0.023 | 0.024–0.030 | 0.024–0.237 | 296 | 3 |
| ToneMapPass | 0.015 | 0.018–0.025 | 0.019–0.029 | 296 | 3 |
| UICompositePass | 0.009 | 0.010–0.014 | 0.010–0.015 | 296 | 3 |

Top sub-passes by p50 (valid samples only):

| sub-pass | parent | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---|---:|---:|---:|---:|---:|
| ShadowPass/VsmLocal | ShadowPass | 24.561–25.134 | 26.907–33.320 | 27.038–34.532 | 296 | 3 |
| ShadowPass/VsmLocal/GroomCasters | ShadowPass | 23.434–24.072 | 25.786–31.701 | 25.874–33.141 | 296 | 3 |
| ShadowPass/VsmSun | ShadowPass | 5.291–5.520 | 5.732–8.129 | 5.785–8.593 | 296 | 3 |
| GroomPass/StrandShade | GroomPass | 2.481–2.512 | 2.558–3.663 | 2.575–4.087 | 296 | 3 |
| ShadowPass/VsmSun/FoliageCull | ShadowPass | 2.010–2.014 | 2.076–2.853 | 2.286–3.441 | 296 | 3 |
| ScenePass/Color | ScenePass | 1.915–1.924 | 2.153–3.060 | 3.793–76.089 | 296 | 3 |
| GroomPass/StrandPrepass | GroomPass | 1.622–1.663 | 1.914–2.790 | 1.935–3.331 | 296 | 3 |
| ShadowPass/VsmSun/GroomCasters | ShadowPass | 1.416–1.610 | 1.619–2.421 | 1.882–2.984 | 296 | 3 |
| ShadowPass/VsmSun/FoliageCasters | ShadowPass | 0.903–0.934 | 0.932–1.572 | 1.215–2.245 | 296 | 3 |
| ScenePrepassPass/DepthPrepass | ScenePrepassPass | 0.914–0.921 | 0.939–1.416 | 1.194–1.986 | 296 | 3 |

### head-closeup (frames/run 300)

| channel | p50 ms | p95 ms | p99 ms | max ms | gate misses | samples |
|---|---:|---:|---:|---:|---:|---:|
| frame time | 53.91–54.42 | 148.66–157.65 | 159.01–167.27 | 179.63–181.59 | 300 | 300 |
| CPU | 27.78–28.34 | 143.84–150.91 | 155.85–162.67 | 159.21–179.67 | 300 | 300 |
| GPU total (valid) | 54.57–55.90 | 141.91–149.21 | 155.56–166.03 | 160.02–180.99 | 292 | 292 |
| fence wait | 0.01 | 26.21–26.65 | 27.19–28.23 | 29.29–29.90 | 141–143 | 300 |
| present wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording wall | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording join wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |

Excluded total-GPU samples (all counted runs): duplicate 24.

Groom deformation upload per frame, MiB (range over counted runs): p50 8.600, p95 8.600, max 8.600.

Top passes by p50 (valid samples only):

| pass | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---:|---:|---:|---:|---:|
| ShadowPass | 30.638–31.109 | 32.670–40.321 | 32.853–42.373 | 292 | 3 |
| GroomPass | 12.383–12.825 | 96.757–98.820 | 110.546–121.029 | 292 | 3 |
| ScenePass | 3.461–3.480 | 3.506–4.633 | 3.770–5.286 | 292 | 3 |
| FoliagePass | 2.743–2.847 | 2.895–3.818 | 2.993–4.558 | 292 | 3 |
| ScenePrepassPass | 2.479–2.489 | 2.516–3.309 | 2.778–3.756 | 292 | 3 |
| VirtualShadowMapMarkPass | 0.171–0.178 | 0.176–0.206 | 0.180–0.395 | 292 | 3 |
| FoliageCull | 0.142 | 4.855–5.874 | 5.951–6.667 | 292 | 3 |
| SkinDiffusionPass | 0.139–0.142 | 0.150–0.187 | 0.152–0.354 | 292 | 3 |
| TAAPass | 0.135–0.140 | 0.144–0.181 | 0.150–0.193 | 292 | 3 |
| DiffusionHandoffSnapshotPass | 0.040 | 0.041–0.050 | 0.041–0.051 | 292 | 3 |
| DiffusionDepthSnapshotPass | 0.029 | 0.030–0.036 | 0.030–0.038 | 292 | 3 |
| GroomSceneDepthSnapshotPass | 0.029 | 0.030–0.036 | 0.030–0.037 | 292 | 3 |
| GroomSceneEntitySnapshotPass | 0.024 | 0.032–0.037 | 0.032–0.039 | 292 | 3 |
| ToneMapPass | 0.015–0.016 | 0.019–0.022 | 0.022–0.026 | 292 | 3 |
| UICompositePass | 0.009 | 0.010–0.012 | 0.011–0.014 | 292 | 3 |

Top sub-passes by p50 (valid samples only):

| sub-pass | parent | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---|---:|---:|---:|---:|---:|
| ShadowPass/VsmLocal | ShadowPass | 23.172–23.635 | 25.080–29.965 | 25.228–32.735 | 292 | 3 |
| ShadowPass/VsmLocal/GroomCasters | ShadowPass | 21.267–21.887 | 23.336–27.698 | 23.475–30.609 | 292 | 3 |
| ShadowPass/VsmSun | ShadowPass | 6.073–6.158 | 6.164–8.360 | 6.427–9.117 | 292 | 3 |
| GroomPass/StrandShade | GroomPass | 4.950–5.553 | 5.613–6.400 | 5.803–6.637 | 292 | 3 |
| GroomPass/StrandPrepass | GroomPass | 3.832–4.163 | 4.091–5.502 | 4.366–6.072 | 292 | 3 |
| ScenePass/Color | ScenePass | 3.418–3.437 | 3.463–4.570 | 3.727–5.233 | 292 | 3 |
| GroomPass/RootFrames | GroomPass | 2.812–3.012 | 86.353–88.003 | 99.777–108.258 | 292 | 3 |
| GroomPass/RootFrames/Dispatch | GroomPass | 2.473–2.668 | 86.197–87.850 | 99.614–108.086 | 292 | 3 |
| ScenePrepassPass/DepthPrepass | ScenePrepassPass | 2.467–2.476 | 2.501–3.291 | 2.769–3.745 | 292 | 3 |
| ShadowPass/VsmSun/FoliageCull | ShadowPass | 1.999–2.004 | 2.020–2.737 | 2.275–3.294 | 292 | 3 |

### population-dolly (frames/run 300)

| channel | p50 ms | p95 ms | p99 ms | max ms | gate misses | samples |
|---|---:|---:|---:|---:|---:|---:|
| frame time | 52.74–54.78 | 102.92–106.31 | 187.66–197.25 | 215.25–242.07 | 300 | 300 |
| CPU | 19.54–21.44 | 67.39–88.54 | 148.39–161.08 | 184.72–217.43 | 285–300 | 300 |
| GPU total (valid) | 54.11–56.05 | 68.24–70.14 | 92.03–116.89 | 132.62–169.57 | 299–300 | 299–300 |
| fence wait | 31.23–33.37 | 43.47–44.77 | 45.18–47.17 | 47.30–48.66 | 230–257 | 300 |
| present wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording wall | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording join wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |

Excluded total-GPU samples (all counted runs): duplicate 1.

Groom deformation upload per frame, MiB (range over counted runs): p50 5.397, p95 7.990, max 25.930.

Top passes by p50 (valid samples only):

| pass | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---:|---:|---:|---:|---:|
| ShadowPass | 20.908–21.622 | 27.790–28.604 | 28.965–30.035 | 299–300 | 3 |
| FoliagePass | 16.731–17.133 | 18.625–19.063 | 18.831–19.539 | 299–300 | 3 |
| GroomPass | 8.744–8.929 | 14.471–15.076 | 38.417–71.556 | 299–300 | 3 |
| ScenePrepassPass | 3.304–3.351 | 3.622–4.066 | 3.814–4.405 | 299–300 | 3 |
| ScenePass | 3.238–3.263 | 3.575–4.055 | 3.785–4.332 | 299–300 | 3 |
| FoliageCull | 0.148–0.150 | 0.184–1.097 | 6.068–9.521 | 299–300 | 3 |
| TAAPass | 0.143 | 0.153–0.158 | 0.364–0.478 | 299–300 | 3 |
| VirtualShadowMapMarkPass | 0.053–0.054 | 0.058–0.061 | 0.060–0.090 | 299–300 | 3 |
| DiffusionHandoffSnapshotPass | 0.038 | 0.038–0.039 | 0.039 | 299–300 | 3 |
| GroomSceneDepthSnapshotPass | 0.029–0.030 | 0.030 | 0.030–0.031 | 299–300 | 3 |
| DiffusionDepthSnapshotPass | 0.029 | 0.030 | 0.030 | 299–300 | 3 |
| SkinDiffusionPass | 0.027 | 0.033–0.034 | 0.036 | 299–300 | 3 |
| GroomSceneEntitySnapshotPass | 0.023 | 0.029–0.030 | 0.031–0.038 | 299–300 | 3 |
| ToneMapPass | 0.016 | 0.019–0.020 | 0.023–0.024 | 299–300 | 3 |
| UICompositePass | 0.009–0.010 | 0.011 | 0.013 | 299–300 | 3 |

Top sub-passes by p50 (valid samples only):

| sub-pass | parent | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---|---:|---:|---:|---:|---:|
| ShadowPass/VsmSun | ShadowPass | 19.399–19.670 | 25.823–26.456 | 26.250–27.124 | 299–300 | 3 |
| ShadowPass/VsmSun/FoliageCasters | ShadowPass | 11.177–11.544 | 11.916–12.532 | 12.803–13.298 | 299–300 | 3 |
| ShadowPass/VsmSun/GroomCasters | ShadowPass | 4.761–4.786 | 11.600–11.771 | 11.907–12.446 | 299–300 | 3 |
| GroomPass/StrandShade | GroomPass | 3.638–3.665 | 6.502–6.681 | 6.947–7.607 | 299–300 | 3 |
| ScenePrepassPass/DepthPrepass | ScenePrepassPass | 3.284–3.326 | 3.610–4.053 | 3.805–4.338 | 299–300 | 3 |
| ScenePass/Color | ScenePass | 3.186–3.213 | 3.533–4.014 | 3.744–4.291 | 299–300 | 3 |
| GroomPass/StrandPrepass | GroomPass | 3.045–3.212 | 5.146–5.445 | 5.511–5.945 | 299–300 | 3 |
| ShadowPass/VsmSun/FoliageCull | ShadowPass | 2.130–2.135 | 2.391–2.818 | 2.405–2.914 | 299–300 | 3 |
| GroomPass/RootFrames | GroomPass | 1.935–1.944 | 2.331–2.598 | 3.651–24.848 | 299–300 | 3 |
| GroomPass/RootFrames/Dispatch | GroomPass | 1.687–1.697 | 1.998–2.281 | 2.130–11.418 | 299–300 | 3 |

### rapid-turn (frames/run 300)

| channel | p50 ms | p95 ms | p99 ms | max ms | gate misses | samples |
|---|---:|---:|---:|---:|---:|---:|
| frame time | 37.67–38.77 | 63.00–64.80 | 80.50–86.78 | 121.12–141.53 | 289 | 300 |
| CPU | 12.85–13.21 | 23.05–25.02 | 54.83–56.47 | 116.23–120.57 | 59–64 | 300 |
| GPU total (valid) | 38.12–39.10 | 59.74–61.63 | 60.28–62.72 | 64.51–75.34 | 300 | 300 |
| fence wait | 24.47–25.55 | 40.70–42.47 | 42.88–43.43 | 43.15–43.86 | 280–281 | 300 |
| present wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording wall | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording join wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |

Groom deformation upload per frame, MiB (range over counted runs): p50 2.501, p95 4.877, max 8.161.

Top passes by p50 (valid samples only):

| pass | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---:|---:|---:|---:|---:|
| ShadowPass | 18.556–19.154 | 30.594–31.269 | 30.940–31.572 | 300 | 3 |
| FoliagePass | 13.429–13.804 | 16.613–17.118 | 17.280–17.591 | 300 | 3 |
| GroomPass | 2.024–2.115 | 5.675–5.920 | 5.902–6.505 | 300 | 3 |
| ScenePass | 1.531–1.611 | 3.388–3.421 | 3.455–3.942 | 300 | 3 |
| ScenePrepassPass | 1.493–1.498 | 3.503–3.574 | 3.579–4.045 | 300 | 3 |
| FoliageCull | 0.132–0.133 | 0.154–0.155 | 0.177–0.594 | 300 | 3 |
| TAAPass | 0.130–0.131 | 0.137–0.139 | 0.142–0.350 | 300 | 3 |
| VirtualShadowMapMarkPass | 0.045–0.049 | 0.056–0.068 | 0.060–0.072 | 300 | 3 |
| DiffusionHandoffSnapshotPass | 0.038 | 0.038 | 0.039 | 300 | 3 |
| DiffusionDepthSnapshotPass | 0.029 | 0.030 | 0.030 | 300 | 3 |
| GroomSceneDepthSnapshotPass | 0.029 | 0.030 | 0.030–0.031 | 300 | 3 |
| SkinDiffusionPass | 0.027 | 0.032 | 0.033–0.034 | 300 | 3 |
| GroomSceneEntitySnapshotPass | 0.023 | 0.023–0.024 | 0.031–0.032 | 300 | 3 |
| ToneMapPass | 0.015 | 0.018–0.019 | 0.019–0.024 | 300 | 3 |
| UICompositePass | 0.009–0.010 | 0.011 | 0.013–0.015 | 300 | 3 |

Top sub-passes by p50 (valid samples only):

| sub-pass | parent | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---|---:|---:|---:|---:|---:|
| ShadowPass/VsmSun | ShadowPass | 16.471–16.902 | 17.139–17.569 | 17.233–17.788 | 300 | 3 |
| ShadowPass/VsmSun/FoliageCasters | ShadowPass | 12.212–12.657 | 12.769–13.109 | 12.831–13.359 | 300 | 3 |
| ShadowPass/VsmSun/FoliageCull | ShadowPass | 2.135–2.137 | 2.395–2.605 | 2.417–2.869 | 300 | 3 |
| ScenePass/Color | ScenePass | 1.479–1.486 | 3.304–3.381 | 3.413–3.880 | 300 | 3 |
| ScenePrepassPass/DepthPrepass | ScenePrepassPass | 1.467–1.477 | 3.491–3.560 | 3.567–4.033 | 300 | 3 |
| GroomPass/StrandShade | GroomPass | 0.986–0.990 | 1.936–1.940 | 1.976–1.988 | 300 | 3 |
| ShadowPass/VsmLocal | ShadowPass | 0.964–0.966 | 11.995–12.303 | 12.451–12.601 | 300 | 3 |
| ShadowPass/VsmSun/TerrainCasters | ShadowPass | 0.886–0.900 | 0.897–0.913 | 0.916–0.933 | 300 | 3 |
| ShadowPass/VsmSun/GroomCasters | ShadowPass | 0.891–0.897 | 1.050–1.116 | 1.316–1.619 | 300 | 3 |
| GroomPass/StrandPrepass | GroomPass | 0.668–0.677 | 1.787–1.858 | 1.859–2.451 | 300 | 3 |

Excluded per-pass samples (all counted runs): duplicate-frame-pass 1953.

Memory: peak GPU resident 2083.2 MiB, live GPU 1788.0 MiB, peak CPU tracked 93.2 MiB, GPU after scene release 1788.0 MiB.
Report: GPU peak 3123.1 MiB / live 1785.4 MiB; CPU peak 93.2 MiB / live 93.2 MiB; reconciliation notObservable; residency unknown.
Top GPU owners (first counted run): ShadowPass 602.5 MiB, GroomPass 408.2 MiB, Unattributed 177.6 MiB, TransientPool 174.0 MiB, RendererInit 144.3 MiB, Foliage pinned representations 129.0 MiB, ShadowMap 80.0 MiB, TemporalHistory 35.6 MiB, Texture2D 25.3 MiB, EnvironmentMap 4.1 MiB.

## gl-deferred: FAIL

run 1 coats-closeup p95 88.311 ms > 16.667 ms; run 1 head-backlit p95 142.810 ms > 16.667 ms; run 1 head-closeup p95 171.995 ms > 16.667 ms; run 1 population-dolly p95 89.728 ms > 16.667 ms; run 1 rapid-turn p95 54.566 ms > 16.667 ms; run 2 coats-closeup p95 78.099 ms > 16.667 ms; run 2 head-backlit p95 138.616 ms > 16.667 ms; run 2 head-closeup p95 151.670 ms > 16.667 ms; run 2 population-dolly p95 89.381 ms > 16.667 ms; run 2 rapid-turn p95 50.199 ms > 16.667 ms; run 3 coats-closeup p95 79.278 ms > 16.667 ms; run 3 head-backlit p95 137.435 ms > 16.667 ms; run 3 head-closeup p95 147.171 ms > 16.667 ms; run 3 population-dolly p95 85.521 ms > 16.667 ms; run 3 rapid-turn p95 49.308 ms > 16.667 ms. Counted runs: 3 of 3.

Host: test-binary / opengl. Frame time = wall-clock Scene::OnUpdateEditor call, excluding attachment readback.
Internal 1920x1080, display 1920x1080 (native); upscaler resolved/fallback: {"fallback": "none", "resolved": "native"}.
Selected: lightingTap=None, msaaSamples=1, path=Deferred, rayTracedShadowsRequested=False, skinDiffusion=True, upscaleMode=0, upscaleTechnique=0, virtualShadowMaps=True; VSM selected: True; pinned by manifest: MSAASampleCount, Path, RayTracedShadowsEnabled, Upscale, UpscaleTechnique, VirtualShadowMaps.
Selected settings are what the renderer was set to, not proof each technique produced or consumed work.

### coats-closeup (frames/run 300)

| channel | p50 ms | p95 ms | p99 ms | max ms | gate misses | samples |
|---|---:|---:|---:|---:|---:|---:|
| frame time | 66.84–69.31 | 78.10–88.31 | 101.92–105.97 | 369.91–397.34 | 300 | 300 |
| CPU | 66.38–68.65 | 77.64–87.68 | 101.37–105.09 | 369.47–396.89 | 300 | 300 |
| GPU total (valid) | 66.54–68.60 | 76.05–86.67 | 93.46–97.99 | 104.75–381.54 | 299–300 | 299–300 |
| fence wait | 0.00 | 0.00 | 0.00 | 0.01 | 0 | 300 |
| present wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording wall | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording join wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |

Excluded total-GPU samples (all counted runs): duplicate 2.

Groom deformation upload per frame, MiB (range over counted runs): p50 11.737, p95 11.943, max 22.323.

Top passes by p50 (valid samples only):

| pass | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---:|---:|---:|---:|---:|
| ShadowPass | 28.353–28.502 | 30.786–31.634 | 33.890–35.035 | 299–300 | 3 |
| GroomPass | 23.494–25.199 | 32.170–42.904 | 46.691–49.587 | 299–300 | 3 |
| ScenePass | 7.991–8.011 | 8.248–8.675 | 8.376–8.993 | 299–300 | 3 |
| FoliageCull | 2.879–3.034 | 4.214–4.438 | 4.777–4.975 | 299–300 | 3 |
| DeferredLightingPass | 2.153–2.159 | 2.362–2.430 | 2.438–2.655 | 299–300 | 3 |
| VirtualShadowMapMarkPass | 0.303–0.319 | 0.362–0.537 | 0.579–0.680 | 299–300 | 3 |
| TAAPass | 0.161–0.162 | 0.170–0.174 | 0.178–0.624 | 299–300 | 3 |
| SceneVelocitySeedPass | 0.047–0.048 | 0.048–0.049 | 0.049 | 299–300 | 3 |
| DiffusionHandoffSnapshotPass | 0.038 | 0.038–0.039 | 0.038–0.039 | 299–300 | 3 |
| GroomSceneEntitySnapshotPass | 0.031 | 0.032 | 0.036–0.038 | 299–300 | 3 |
| GroomSceneDepthSnapshotPass | 0.029–0.030 | 0.030 | 0.030–0.284 | 299–300 | 3 |
| DiffusionDepthSnapshotPass | 0.029 | 0.030 | 0.030 | 299–300 | 3 |
| SkinDiffusionPass | 0.027 | 0.032–0.033 | 0.036–0.037 | 299–300 | 3 |
| ToneMapPass | 0.016 | 0.019 | 0.022–0.023 | 299–300 | 3 |
| UICompositePass | 0.009–0.010 | 0.010 | 0.012–0.015 | 299–300 | 3 |

Top sub-passes by p50 (valid samples only):

| sub-pass | parent | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---|---:|---:|---:|---:|---:|
| ShadowPass/VsmSun | ShadowPass | 17.875–17.946 | 18.461–19.003 | 19.643–20.301 | 299–300 | 3 |
| ShadowPass/VsmSun/FoliageCasters | ShadowPass | 10.516–10.574 | 10.756–11.372 | 10.893–11.574 | 299–300 | 3 |
| ShadowPass/VsmLocal | ShadowPass | 5.418–5.723 | 6.338–6.535 | 6.724–6.892 | 299–300 | 3 |
| ScenePass/DepthPrepass | ScenePass | 4.105–4.155 | 4.376–4.727 | 4.443–5.036 | 299–300 | 3 |
| ShadowPass/VsmSun/GroomCasters | ShadowPass | 4.079–4.103 | 4.473–4.738 | 5.676–6.701 | 299–300 | 3 |
| ScenePass/Color | ScenePass | 3.764–3.768 | 4.042–4.251 | 4.104–4.518 | 299–300 | 3 |
| GroomPass/StrandShade | GroomPass | 3.262–3.279 | 5.221–5.597 | 8.371–13.674 | 299–300 | 3 |
| GroomPass/StrandPrepass | GroomPass | 2.428–2.479 | 3.101–3.126 | 19.323–19.962 | 299–300 | 3 |
| ShadowPass/VsmLocal/FoliageCasters | ShadowPass | 2.308–2.394 | 2.564–2.905 | 2.642–3.209 | 299–300 | 3 |
| ShadowPass/VsmLocal/GroomCasters | ShadowPass | 2.316–2.339 | 2.978–3.277 | 3.818–3.859 | 299–300 | 3 |

### head-backlit (frames/run 300)

| channel | p50 ms | p95 ms | p99 ms | max ms | gate misses | samples |
|---|---:|---:|---:|---:|---:|---:|
| frame time | 40.02–41.07 | 137.44–142.81 | 148.57–151.12 | 159.41–175.58 | 300 | 300 |
| CPU | 19.53–20.45 | 135.88–137.86 | 145.46–150.20 | 148.79–174.68 | 297–299 | 300 |
| GPU total (valid) | 39.44–40.84 | 135.09–138.86 | 147.61–153.54 | 149.72–175.40 | 296 | 296 |
| fence wait | 17.09–18.23 | 20.54–21.47 | 21.31–22.06 | 22.25–22.32 | 158–164 | 300 |
| present wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording wall | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording join wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |

Excluded total-GPU samples (all counted runs): duplicate 12.

Groom deformation upload per frame, MiB (range over counted runs): p50 4.595, p95 4.696, max 4.696.

Top passes by p50 (valid samples only):

| pass | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---:|---:|---:|---:|---:|
| ShadowPass | 30.246–31.570 | 32.156–43.013 | 33.277–44.290 | 296 | 3 |
| GroomPass | 5.119–5.217 | 5.587–7.739 | 6.020–8.693 | 296 | 3 |
| DeferredLightingPass | 1.663–1.673 | 93.534–95.134 | 107.908–111.387 | 296 | 3 |
| ScenePass | 0.894–0.907 | 1.025–1.405 | 60.206–68.191 | 296 | 3 |
| VirtualShadowMapMarkPass | 0.176–0.197 | 0.187–0.215 | 0.198–0.556 | 296 | 3 |
| SkinDiffusionPass | 0.139–0.140 | 0.150–0.236 | 0.166–0.256 | 296 | 3 |
| FoliageCull | 0.133–0.135 | 3.040–3.438 | 4.622–4.848 | 296 | 3 |
| TAAPass | 0.130–0.131 | 0.137–0.232 | 0.156–0.392 | 296 | 3 |
| SceneVelocitySeedPass | 0.041–0.042 | 0.044–0.056 | 0.049–0.295 | 296 | 3 |
| DiffusionHandoffSnapshotPass | 0.040 | 0.042–0.056 | 0.046–0.275 | 296 | 3 |
| GroomSceneDepthSnapshotPass | 0.030 | 0.031–0.042 | 0.033–0.046 | 296 | 3 |
| DiffusionDepthSnapshotPass | 0.029 | 0.031–0.042 | 0.033–0.046 | 296 | 3 |
| GroomSceneEntitySnapshotPass | 0.023 | 0.024–0.031 | 0.026–0.032 | 296 | 3 |
| ToneMapPass | 0.015 | 0.017–0.026 | 0.022–0.033 | 296 | 3 |
| UICompositePass | 0.010 | 0.012–0.015 | 0.016–0.020 | 296 | 3 |

Top sub-passes by p50 (valid samples only):

| sub-pass | parent | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---|---:|---:|---:|---:|---:|
| ShadowPass/VsmLocal | ShadowPass | 23.508–24.832 | 24.991–33.250 | 25.279–34.078 | 296 | 3 |
| ShadowPass/VsmLocal/GroomCasters | ShadowPass | 22.429–23.577 | 23.893–31.470 | 24.053–32.412 | 296 | 3 |
| ShadowPass/VsmSun | ShadowPass | 5.445–5.452 | 5.762–7.982 | 6.041–8.941 | 296 | 3 |
| GroomPass/StrandPrepass | GroomPass | 2.580–2.649 | 2.884–3.630 | 3.006–3.754 | 296 | 3 |
| GroomPass/StrandShade | GroomPass | 2.477–2.500 | 2.554–3.969 | 2.572–4.743 | 296 | 3 |
| ShadowPass/VsmSun/FoliageCull | ShadowPass | 2.010–2.022 | 2.282–3.095 | 2.383–3.278 | 296 | 3 |
| ShadowPass/VsmSun/GroomCasters | ShadowPass | 1.430–1.467 | 1.634–2.418 | 1.887–2.974 | 296 | 3 |
| ShadowPass/VsmSun/FoliageCasters | ShadowPass | 0.920–0.943 | 1.199–1.258 | 1.220–1.876 | 296 | 3 |
| ShadowPass/VsmLocal/FoliageCull | ShadowPass | 0.495–0.694 | 0.742–1.042 | 0.805–1.316 | 296 | 3 |
| ShadowPass/VsmSun/TerrainCasters | ShadowPass | 0.547–0.556 | 0.582–0.696 | 0.626–0.743 | 296 | 3 |

### head-closeup (frames/run 300)

| channel | p50 ms | p95 ms | p99 ms | max ms | gate misses | samples |
|---|---:|---:|---:|---:|---:|---:|
| frame time | 49.27–49.76 | 147.17–172.00 | 162.11–247.96 | 174.45–394.89 | 300 | 300 |
| CPU | 27.64–29.24 | 143.71–170.97 | 153.69–237.78 | 167.05–387.09 | 300 | 300 |
| GPU total (valid) | 49.79–50.11 | 140.53–173.91 | 154.56–243.88 | 162.35–353.01 | 292 | 292 |
| fence wait | 0.01 | 20.94–22.26 | 22.06–23.19 | 23.43–23.98 | 140–141 | 300 |
| present wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording wall | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording join wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |

Excluded total-GPU samples (all counted runs): duplicate 24.

Groom deformation upload per frame, MiB (range over counted runs): p50 8.600, p95 8.600, max 8.600.

Top passes by p50 (valid samples only):

| pass | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---:|---:|---:|---:|---:|
| ShadowPass | 29.749–29.917 | 32.072–43.604 | 33.041–47.579 | 292 | 3 |
| GroomPass | 14.212–14.635 | 103.849–119.858 | 115.422–181.503 | 292 | 3 |
| ScenePass | 1.871–1.900 | 1.927–3.200 | 2.162–3.764 | 292 | 3 |
| DeferredLightingPass | 1.742–1.755 | 1.784–3.570 | 2.022–4.356 | 292 | 3 |
| VirtualShadowMapMarkPass | 0.173–0.225 | 0.182–0.266 | 0.238–0.550 | 292 | 3 |
| FoliageCull | 0.140–0.143 | 2.793–3.327 | 3.985–17.800 | 292 | 3 |
| SkinDiffusionPass | 0.139–0.143 | 0.150–0.272 | 0.154–0.325 | 292 | 3 |
| TAAPass | 0.136–0.141 | 0.145–0.292 | 0.152–0.468 | 292 | 3 |
| SceneVelocitySeedPass | 0.043 | 0.043–0.065 | 0.043–0.072 | 292 | 3 |
| DiffusionHandoffSnapshotPass | 0.040 | 0.041–0.058 | 0.041–0.065 | 292 | 3 |
| GroomSceneDepthSnapshotPass | 0.029–0.030 | 0.030–0.046 | 0.030–0.053 | 292 | 3 |
| DiffusionDepthSnapshotPass | 0.029 | 0.030–0.048 | 0.030–0.054 | 292 | 3 |
| GroomSceneEntitySnapshotPass | 0.023–0.024 | 0.031–0.043 | 0.032–0.047 | 292 | 3 |
| ToneMapPass | 0.015 | 0.019–0.031 | 0.023–0.038 | 292 | 3 |
| UICompositePass | 0.009–0.010 | 0.013–0.020 | 0.013–0.025 | 292 | 3 |

Top sub-passes by p50 (valid samples only):

| sub-pass | parent | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---|---:|---:|---:|---:|---:|
| ShadowPass/VsmLocal | ShadowPass | 22.178–22.216 | 24.083–30.909 | 24.327–32.195 | 292 | 3 |
| ShadowPass/VsmLocal/GroomCasters | ShadowPass | 20.360–20.442 | 22.332–28.243 | 22.562–29.519 | 292 | 3 |
| GroomPass/StrandShade | GroomPass | 6.082–6.713 | 6.824–12.729 | 6.864–15.843 | 292 | 3 |
| ShadowPass/VsmSun | ShadowPass | 6.115–6.151 | 6.411–10.364 | 6.431–12.123 | 292 | 3 |
| GroomPass/StrandPrepass | GroomPass | 4.542–4.965 | 4.846–7.683 | 4.887–9.809 | 292 | 3 |
| GroomPass/RootFrames | GroomPass | 2.820–3.007 | 90.795–105.972 | 103.163–155.434 | 292 | 3 |
| GroomPass/RootFrames/Dispatch | GroomPass | 2.655–2.678 | 90.633–105.810 | 103.006–155.248 | 292 | 3 |
| ShadowPass/VsmSun/FoliageCull | ShadowPass | 2.000–2.015 | 2.237–3.243 | 2.274–3.748 | 292 | 3 |
| ShadowPass/VsmSun/GroomCasters | ShadowPass | 1.953–1.977 | 2.002–3.761 | 2.263–4.794 | 292 | 3 |
| ShadowPass/VsmLocal/FoliageCull | ShadowPass | 1.070–1.091 | 1.310–1.804 | 1.329–2.390 | 292 | 3 |

### population-dolly (frames/run 300)

| channel | p50 ms | p95 ms | p99 ms | max ms | gate misses | samples |
|---|---:|---:|---:|---:|---:|---:|
| frame time | 43.02–43.62 | 85.52–89.73 | 168.51–177.23 | 201.74–218.68 | 299–300 | 300 |
| CPU | 19.00–19.55 | 63.54–70.48 | 141.86–151.83 | 178.34–194.58 | 279–296 | 300 |
| GPU total (valid) | 44.30–45.08 | 57.10–58.66 | 93.12–109.29 | 141.73–157.30 | 300 | 300 |
| fence wait | 23.80–23.89 | 32.00–32.29 | 34.52–35.14 | 36.38–37.25 | 239–243 | 300 |
| present wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording wall | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording join wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |

Groom deformation upload per frame, MiB (range over counted runs): p50 5.397, p95 7.990, max 25.930.

Top passes by p50 (valid samples only):

| pass | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---:|---:|---:|---:|---:|
| ShadowPass | 21.573–22.069 | 28.176–28.777 | 29.590–30.153 | 300 | 3 |
| ScenePass | 10.540–10.592 | 10.802–11.268 | 11.078–11.527 | 300 | 3 |
| GroomPass | 9.710–9.786 | 15.735–16.052 | 52.468–68.033 | 300 | 3 |
| DeferredLightingPass | 1.264–1.273 | 1.340–1.489 | 1.527–1.569 | 300 | 3 |
| FoliageCull | 0.148–0.150 | 0.188–0.417 | 5.917–6.408 | 300 | 3 |
| TAAPass | 0.142–0.143 | 0.148–0.154 | 0.151–0.166 | 300 | 3 |
| VirtualShadowMapMarkPass | 0.076–0.092 | 0.089–0.102 | 0.119–0.133 | 300 | 3 |
| SceneVelocitySeedPass | 0.044 | 0.045 | 0.045 | 300 | 3 |
| DiffusionHandoffSnapshotPass | 0.038 | 0.038 | 0.039 | 300 | 3 |
| GroomSceneDepthSnapshotPass | 0.030 | 0.030 | 0.030–0.031 | 300 | 3 |
| DiffusionDepthSnapshotPass | 0.029 | 0.030 | 0.030 | 300 | 3 |
| SkinDiffusionPass | 0.027 | 0.032 | 0.035–0.036 | 300 | 3 |
| GroomSceneEntitySnapshotPass | 0.024 | 0.031 | 0.032–0.038 | 300 | 3 |
| ToneMapPass | 0.016 | 0.019–0.020 | 0.020–0.022 | 300 | 3 |
| UICompositePass | 0.009–0.010 | 0.010–0.011 | 0.013–0.015 | 300 | 3 |

Top sub-passes by p50 (valid samples only):

| sub-pass | parent | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---|---:|---:|---:|---:|---:|
| ShadowPass/VsmSun | ShadowPass | 19.953–20.302 | 26.416–26.754 | 26.550–27.284 | 300 | 3 |
| ShadowPass/VsmSun/FoliageCasters | ShadowPass | 11.923–12.184 | 13.387–13.750 | 13.468–14.280 | 300 | 3 |
| ScenePass/DepthPrepass | ScenePass | 5.377–5.401 | 5.519–5.908 | 5.703–6.085 | 300 | 3 |
| ScenePass/Color | ScenePass | 5.079–5.104 | 5.279–5.606 | 5.470–5.819 | 300 | 3 |
| ShadowPass/VsmSun/GroomCasters | ShadowPass | 4.690–4.732 | 11.347–11.601 | 11.606–12.112 | 300 | 3 |
| GroomPass/StrandShade | GroomPass | 4.711–4.722 | 7.643–7.885 | 8.223–8.525 | 300 | 3 |
| GroomPass/StrandPrepass | GroomPass | 3.041–3.070 | 4.991–5.280 | 5.340–14.245 | 300 | 3 |
| ShadowPass/VsmSun/FoliageCull | ShadowPass | 2.125–2.133 | 2.158–2.394 | 2.414–2.612 | 300 | 3 |
| GroomPass/RootFrames | GroomPass | 1.880–1.929 | 2.171–2.341 | 3.678–3.715 | 300 | 3 |
| GroomPass/RootFrames/Dispatch | GroomPass | 1.622–1.630 | 1.900–2.068 | 2.098–2.303 | 300 | 3 |

### rapid-turn (frames/run 300)

| channel | p50 ms | p95 ms | p99 ms | max ms | gate misses | samples |
|---|---:|---:|---:|---:|---:|---:|
| frame time | 32.86–32.98 | 49.31–54.57 | 74.80–82.48 | 113.80–121.02 | 282–284 | 300 |
| CPU | 12.75–13.69 | 24.36–26.43 | 54.42–58.69 | 113.36–120.55 | 55–75 | 300 |
| GPU total (valid) | 32.87–33.04 | 46.67–50.18 | 47.96–51.19 | 80.22–88.30 | 300 | 300 |
| fence wait | 19.33–19.83 | 27.15–30.24 | 29.57–33.14 | 30.30–33.55 | 247–268 | 300 |
| present wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording wall | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording join wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |

Groom deformation upload per frame, MiB (range over counted runs): p50 2.501, p95 4.877, max 8.161.

Top passes by p50 (valid samples only):

| pass | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---:|---:|---:|---:|---:|
| ShadowPass | 19.343–19.573 | 26.367–29.751 | 26.941–29.958 | 300 | 3 |
| ScenePass | 8.961–9.137 | 11.444–11.576 | 11.699–11.924 | 300 | 3 |
| GroomPass | 2.490–2.538 | 6.941–7.121 | 7.250–7.559 | 300 | 3 |
| DeferredLightingPass | 0.933–0.945 | 1.293–1.312 | 1.314–1.520 | 300 | 3 |
| FoliageCull | 0.133–0.134 | 0.153–0.158 | 0.345–1.749 | 300 | 3 |
| TAAPass | 0.132 | 0.138–0.141 | 0.143–0.148 | 300 | 3 |
| VirtualShadowMapMarkPass | 0.057–0.062 | 0.080–0.088 | 0.085–0.090 | 300 | 3 |
| SceneVelocitySeedPass | 0.042 | 0.044 | 0.044 | 300 | 3 |
| DiffusionHandoffSnapshotPass | 0.038 | 0.038 | 0.038–0.039 | 300 | 3 |
| DiffusionDepthSnapshotPass | 0.029 | 0.030 | 0.030 | 300 | 3 |
| GroomSceneDepthSnapshotPass | 0.029 | 0.030 | 0.030 | 300 | 3 |
| SkinDiffusionPass | 0.027 | 0.031–0.032 | 0.034–0.035 | 300 | 3 |
| GroomSceneEntitySnapshotPass | 0.023–0.024 | 0.024–0.027 | 0.033 | 300 | 3 |
| ToneMapPass | 0.015 | 0.019 | 0.020 | 300 | 3 |
| UICompositePass | 0.010 | 0.013 | 0.013–0.016 | 300 | 3 |

Top sub-passes by p50 (valid samples only):

| sub-pass | parent | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---|---:|---:|---:|---:|---:|
| ShadowPass/VsmSun | ShadowPass | 17.155–17.339 | 17.993–18.006 | 18.219–18.643 | 300 | 3 |
| ShadowPass/VsmSun/FoliageCasters | ShadowPass | 12.928–13.126 | 13.557–13.736 | 13.668–14.135 | 300 | 3 |
| ScenePass/DepthPrepass | ScenePass | 4.550–4.627 | 5.811–6.063 | 5.980–6.266 | 300 | 3 |
| ScenePass/Color | ScenePass | 4.354–4.397 | 5.529–5.587 | 5.717–5.874 | 300 | 3 |
| ShadowPass/VsmSun/FoliageCull | ShadowPass | 2.126–2.143 | 2.365–2.405 | 2.418–2.603 | 300 | 3 |
| GroomPass/StrandShade | GroomPass | 1.510–1.522 | 3.133–3.149 | 3.371–3.390 | 300 | 3 |
| ShadowPass/VsmLocal | ShadowPass | 0.961–0.962 | 7.031–10.471 | 7.051–10.692 | 300 | 3 |
| ShadowPass/VsmSun/TerrainCasters | ShadowPass | 0.929–0.940 | 0.942–0.949 | 0.962–0.970 | 300 | 3 |
| ShadowPass/VsmSun/GroomCasters | ShadowPass | 0.885–0.894 | 1.044–1.126 | 1.226–1.406 | 300 | 3 |
| GroomPass/StrandPrepass | GroomPass | 0.642–0.675 | 1.796–1.851 | 2.029–2.220 | 300 | 3 |

Excluded per-pass samples (all counted runs): duplicate-frame-pass 1762.

Memory: peak GPU resident 2170.2 MiB, live GPU 1874.4–1874.9 MiB, peak CPU tracked 93.2 MiB, GPU after scene release 1874.4–1874.9 MiB.
Report: GPU peak 3210.2 MiB / live 1871.9–1872.3 MiB; CPU peak 93.2 MiB / live 93.2 MiB; reconciliation notObservable; residency unknown.
Top GPU owners (first counted run): ShadowPass 602.5 MiB, GroomPass 408.2 MiB, Unattributed 177.6 MiB, TransientPool 174.0 MiB, RendererInit 144.3 MiB, Foliage pinned representations 128.7 MiB, GBuffer 87.0 MiB, ShadowMap 80.0 MiB, TemporalHistory 35.6 MiB, Texture2D 25.3 MiB.

## gl-forward-csm: FAIL

run 1 coats-closeup p95 123.259 ms > 16.667 ms; run 1 head-backlit p95 145.218 ms > 16.667 ms; run 1 head-closeup p95 160.454 ms > 16.667 ms; run 1 population-dolly p95 89.448 ms > 16.667 ms; run 1 rapid-turn p95 48.036 ms > 16.667 ms; run 2 coats-closeup p95 100.440 ms > 16.667 ms; run 2 head-backlit p95 135.715 ms > 16.667 ms; run 2 head-closeup p95 141.791 ms > 16.667 ms; run 2 population-dolly p95 84.507 ms > 16.667 ms; run 2 rapid-turn p95 45.458 ms > 16.667 ms; run 3 coats-closeup p95 79.770 ms > 16.667 ms; run 3 head-backlit p95 136.701 ms > 16.667 ms; run 3 head-closeup p95 145.711 ms > 16.667 ms; run 3 population-dolly p95 86.470 ms > 16.667 ms; run 3 rapid-turn p95 45.025 ms > 16.667 ms. Counted runs: 3 of 3.

Host: test-binary / opengl. Frame time = wall-clock Scene::OnUpdateEditor call, excluding attachment readback.
Internal 1920x1080, display 1920x1080 (native); upscaler resolved/fallback: {"fallback": "none", "resolved": "native"}.
Selected: lightingTap=None, msaaSamples=1, path=Forward, rayTracedShadowsRequested=False, skinDiffusion=True, upscaleMode=0, upscaleTechnique=0, virtualShadowMaps=False; VSM selected: False; pinned by manifest: MSAASampleCount, Path, RayTracedShadowsEnabled, Upscale, UpscaleTechnique, VirtualShadowMaps.
Selected settings are what the renderer was set to, not proof each technique produced or consumed work.

### coats-closeup (frames/run 300)

| channel | p50 ms | p95 ms | p99 ms | max ms | gate misses | samples |
|---|---:|---:|---:|---:|---:|---:|
| frame time | 66.06–72.69 | 79.77–123.26 | 113.08–149.01 | 375.46–422.34 | 300 | 300 |
| CPU | 65.50–72.17 | 78.61–122.42 | 112.20–148.10 | 374.96–421.87 | 300 | 300 |
| GPU total (valid) | 64.07–69.96 | 75.53–119.82 | 98.26–142.73 | 210.01–359.07 | 299–300 | 299–300 |
| fence wait | 0.00 | 0.00 | 0.01 | 0.01–70.90 | 0–1 | 300 |
| present wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording wall | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording join wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |

Excluded total-GPU samples (all counted runs): duplicate 1.

Groom deformation upload per frame, MiB (range over counted runs): p50 11.737, p95 11.943, max 22.323.

Top passes by p50 (valid samples only):

| pass | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---:|---:|---:|---:|---:|
| GroomPass | 19.891–23.554 | 31.301–65.390 | 52.318–81.822 | 299–300 | 3 |
| ShadowPass | 18.508–19.841 | 20.948–23.818 | 23.209–31.631 | 299–300 | 3 |
| FoliagePass | 17.067–17.278 | 17.565–18.262 | 17.757–20.082 | 299–300 | 3 |
| FoliageCull | 1.550–3.501 | 3.014–6.170 | 4.668–8.319 | 299–300 | 3 |
| ScenePass | 2.502–2.515 | 2.545–2.822 | 2.738–3.236 | 299–300 | 3 |
| ScenePrepassPass | 2.296–2.301 | 2.329–2.591 | 2.520–3.175 | 299–300 | 3 |
| TAAPass | 0.160–0.161 | 0.166–0.176 | 0.168–0.500 | 299–300 | 3 |
| DiffusionHandoffSnapshotPass | 0.038 | 0.038 | 0.038–0.043 | 299–300 | 3 |
| GroomSceneEntitySnapshotPass | 0.031 | 0.031–0.032 | 0.032–0.037 | 299–300 | 3 |
| DiffusionDepthSnapshotPass | 0.029 | 0.030 | 0.030–0.032 | 299–300 | 3 |
| GroomSceneDepthSnapshotPass | 0.029 | 0.030 | 0.030–0.033 | 299–300 | 3 |
| SkinDiffusionPass | 0.027 | 0.031–0.032 | 0.033–0.036 | 299–300 | 3 |
| ToneMapPass | 0.015–0.016 | 0.019 | 0.020 | 299–300 | 3 |
| UICompositePass | 0.009–0.010 | 0.010 | 0.012–0.013 | 299–300 | 3 |
| ParticlePass | 0.006 | 0.013 | 0.014–0.015 | 299–300 | 3 |

Top sub-passes by p50 (valid samples only):

| sub-pass | parent | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---|---:|---:|---:|---:|---:|
| GroomPass/RootFrames | GroomPass | 14.768–18.359 | 25.506–58.897 | 46.393–76.850 | 299–300 | 3 |
| GroomPass/RootFrames/Dispatch | GroomPass | 14.713–18.308 | 25.449–58.836 | 46.333–76.793 | 299–300 | 3 |
| ShadowPass/Csm | ShadowPass | 9.143–9.633 | 9.669–9.901 | 9.811–11.006 | 299–300 | 3 |
| ShadowPass/Csm/FoliageCasters | ShadowPass | 6.985–7.489 | 7.459–7.730 | 7.505–8.545 | 299–300 | 3 |
| ShadowPass/Atlas | ShadowPass | 5.073–5.301 | 5.814–5.992 | 6.183–6.422 | 299–300 | 3 |
| GroomPass/StrandShade | GroomPass | 2.626–2.630 | 2.739–3.117 | 3.150–19.607 | 299–300 | 3 |
| ScenePass/Color | ScenePass | 2.498–2.502 | 2.536–2.623 | 2.561–3.072 | 299–300 | 3 |
| ShadowPass/Atlas/GroomCasters | ShadowPass | 2.104–2.353 | 2.668–2.913 | 3.487–3.495 | 299–300 | 3 |
| ShadowPass/Atlas/FoliageCasters | ShadowPass | 2.059–2.290 | 2.321–2.355 | 2.393–2.737 | 299–300 | 3 |
| ScenePrepassPass/DepthPrepass | ScenePrepassPass | 2.284–2.289 | 2.312–2.407 | 2.330–3.099 | 299–300 | 3 |

### head-backlit (frames/run 300)

| channel | p50 ms | p95 ms | p99 ms | max ms | gate misses | samples |
|---|---:|---:|---:|---:|---:|---:|
| frame time | 19.85–20.87 | 135.72–145.22 | 142.13–162.96 | 142.68–180.00 | 290–300 | 300 |
| CPU | 18.54–19.73 | 134.45–142.72 | 140.73–158.76 | 142.25–179.15 | 219–279 | 300 |
| GPU total (valid) | 19.72–21.71 | 134.02–143.43 | 141.51–159.15 | 143.44–185.18 | 295–300 | 295–300 |
| fence wait | 0.00 | 1.03–4.09 | 9.17–11.38 | 10.64–12.22 | 0 | 300 |
| present wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording wall | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording join wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |

Excluded total-GPU samples (all counted runs): duplicate 5.

Groom deformation upload per frame, MiB (range over counted runs): p50 4.595, p95 4.696, max 4.696.

Top passes by p50 (valid samples only):

| pass | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---:|---:|---:|---:|---:|
| ShadowPass | 9.433–9.667 | 18.063–18.799 | 19.022–19.118 | 295–300 | 3 |
| GroomPass | 4.122–4.155 | 7.708–8.647 | 8.709–9.356 | 295–300 | 3 |
| ScenePass | 1.793–1.800 | 93.631–106.250 | 114.380–123.723 | 295–300 | 3 |
| FoliagePass | 1.279–1.280 | 2.835–3.166 | 3.044–86.783 | 295–300 | 3 |
| ScenePrepassPass | 0.878–0.879 | 102.274–108.205 | 112.565–119.156 | 295–300 | 3 |
| FoliageCull | 0.138–0.198 | 3.384–4.328 | 4.486–6.193 | 295–300 | 3 |
| SkinDiffusionPass | 0.141 | 0.283–0.317 | 0.304–0.338 | 295–300 | 3 |
| TAAPass | 0.131–0.132 | 0.250–0.285 | 0.282–0.303 | 295–300 | 3 |
| DiffusionHandoffSnapshotPass | 0.040 | 0.059–0.061 | 0.061–0.063 | 295–300 | 3 |
| GroomSceneDepthSnapshotPass | 0.030 | 0.048–0.051 | 0.051–0.054 | 295–300 | 3 |
| DiffusionDepthSnapshotPass | 0.029 | 0.048–0.051 | 0.051–0.054 | 295–300 | 3 |
| GroomSceneEntitySnapshotPass | 0.024 | 0.034–0.036 | 0.036–0.038 | 295–300 | 3 |
| ToneMapPass | 0.015 | 0.031–0.035 | 0.034–0.039 | 295–300 | 3 |
| UICompositePass | 0.010 | 0.018–0.020 | 0.020–0.023 | 295–300 | 3 |
| FinalPass | 0.005 | 0.007–0.008 | 0.010–0.011 | 295–300 | 3 |

Top sub-passes by p50 (valid samples only):

| sub-pass | parent | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---|---:|---:|---:|---:|---:|
| ShadowPass/Atlas | ShadowPass | 7.018–7.098 | 14.255–14.736 | 14.345–14.925 | 295–300 | 3 |
| ShadowPass/Atlas/GroomCasters | ShadowPass | 6.190–6.230 | 12.961–13.108 | 12.978–13.150 | 295–300 | 3 |
| GroomPass/StrandShade | GroomPass | 2.425–2.431 | 4.607–4.900 | 5.270–5.646 | 295–300 | 3 |
| ScenePass/Color | ScenePass | 1.782–1.787 | 3.929–78.051 | 104.110–113.592 | 295–300 | 3 |
| GroomPass/StrandPrepass | GroomPass | 1.664–1.675 | 3.021–3.384 | 3.246–3.751 | 295–300 | 3 |
| ShadowPass/Csm | ShadowPass | 1.566–1.577 | 2.505–2.706 | 2.685–2.824 | 295–300 | 3 |
| ScenePrepassPass/DepthPrepass | ScenePrepassPass | 0.867 | 96.042–101.986 | 112.466–119.146 | 295–300 | 3 |
| ShadowPass/Csm/FoliageCasters | ShadowPass | 0.577–0.588 | 0.828–0.879 | 0.901–0.918 | 295–300 | 3 |
| ShadowPass/Atlas/FoliageCull | ShadowPass | 0.511–0.513 | 0.836–0.894 | 0.889–1.247 | 295–300 | 3 |
| ShadowPass/Csm/FoliageCull | ShadowPass | 0.500–0.505 | 0.861–0.924 | 0.920–1.016 | 295–300 | 3 |

### head-closeup (frames/run 300)

| channel | p50 ms | p95 ms | p99 ms | max ms | gate misses | samples |
|---|---:|---:|---:|---:|---:|---:|
| frame time | 33.26–34.52 | 141.79–160.45 | 152.41–191.68 | 156.51–199.12 | 300 | 300 |
| CPU | 26.31–30.28 | 141.32–159.74 | 151.20–191.04 | 156.03–198.30 | 300 | 300 |
| GPU total (valid) | 32.69–35.12 | 140.65–160.40 | 147.68–191.64 | 152.52–196.43 | 295–300 | 295–300 |
| fence wait | 0.00 | 5.67–7.44 | 6.78–7.85 | 6.85–8.44 | 0 | 300 |
| present wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording wall | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording join wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |

Excluded total-GPU samples (all counted runs): duplicate 5.

Groom deformation upload per frame, MiB (range over counted runs): p50 8.600, p95 8.600, max 8.600.

Top passes by p50 (valid samples only):

| pass | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---:|---:|---:|---:|---:|
| GroomPass | 12.014–12.233 | 115.473–130.075 | 123.113–157.823 | 295–300 | 3 |
| ShadowPass | 11.046–11.339 | 14.743–19.669 | 19.139–20.701 | 295–300 | 3 |
| ScenePass | 3.346–3.367 | 4.128–4.558 | 4.300–5.347 | 295–300 | 3 |
| FoliagePass | 2.441–2.461 | 2.987–3.639 | 3.173–4.376 | 295–300 | 3 |
| ScenePrepassPass | 2.333–2.345 | 2.858–3.109 | 2.922–3.617 | 295–300 | 3 |
| FoliageCull | 0.143–0.144 | 5.116–5.263 | 6.129–8.343 | 295–300 | 3 |
| SkinDiffusionPass | 0.142–0.143 | 0.171–0.201 | 0.184–0.236 | 295–300 | 3 |
| TAAPass | 0.140–0.141 | 0.168–0.187 | 0.181–0.400 | 295–300 | 3 |
| DiffusionHandoffSnapshotPass | 0.040–0.041 | 0.048–0.051 | 0.049–0.054 | 295–300 | 3 |
| GroomSceneDepthSnapshotPass | 0.029–0.030 | 0.035–0.038 | 0.036–0.042 | 295–300 | 3 |
| DiffusionDepthSnapshotPass | 0.029 | 0.035–0.038 | 0.036–0.041 | 295–300 | 3 |
| GroomSceneEntitySnapshotPass | 0.024–0.026 | 0.036–0.038 | 0.037–0.040 | 295–300 | 3 |
| ToneMapPass | 0.015–0.016 | 0.020–0.023 | 0.025–0.027 | 295–300 | 3 |
| UICompositePass | 0.010 | 0.013–0.014 | 0.014–0.015 | 295–300 | 3 |
| ParticlePass | 0.006 | 0.014 | 0.016–0.017 | 295–300 | 3 |

Top sub-passes by p50 (valid samples only):

| sub-pass | parent | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---|---:|---:|---:|---:|---:|
| ShadowPass/Atlas | ShadowPass | 7.903–8.165 | 8.682–15.376 | 15.279–15.860 | 295–300 | 3 |
| ShadowPass/Atlas/GroomCasters | ShadowPass | 6.277–6.320 | 6.648–13.085 | 13.048–13.357 | 295–300 | 3 |
| GroomPass/StrandShade | GroomPass | 4.993–5.149 | 5.736–6.423 | 6.160–7.690 | 295–300 | 3 |
| GroomPass/StrandPrepass | GroomPass | 3.897–4.019 | 5.050–6.999 | 5.897–97.564 | 295–300 | 3 |
| ScenePass/Color | ScenePass | 3.343–3.362 | 4.124–4.555 | 4.297–5.343 | 295–300 | 3 |
| GroomPass/RootFrames | GroomPass | 2.820–3.032 | 103.734–119.105 | 112.571–144.765 | 295–300 | 3 |
| GroomPass/RootFrames/Dispatch | GroomPass | 2.652–2.687 | 103.569–118.940 | 112.402–144.607 | 295–300 | 3 |
| ScenePrepassPass/DepthPrepass | ScenePrepassPass | 2.321–2.335 | 2.847–3.097 | 2.911–3.601 | 295–300 | 3 |
| ShadowPass/Csm | ShadowPass | 2.127–2.145 | 2.663–2.889 | 2.780–3.347 | 295–300 | 3 |
| ShadowPass/Atlas/FoliageCull | ShadowPass | 1.071–1.074 | 1.315–1.440 | 1.419–1.538 | 295–300 | 3 |

### population-dolly (frames/run 300)

| channel | p50 ms | p95 ms | p99 ms | max ms | gate misses | samples |
|---|---:|---:|---:|---:|---:|---:|
| frame time | 41.29–42.43 | 84.51–89.45 | 168.53–178.54 | 177.78–225.65 | 295–298 | 300 |
| CPU | 18.02–20.38 | 61.75–75.80 | 145.03–166.40 | 177.31–225.16 | 224–262 | 300 |
| GPU total (valid) | 42.14–42.99 | 55.05–60.12 | 100.83–122.61 | 145.31–192.57 | 300 | 300 |
| fence wait | 21.29–24.15 | 28.43–29.18 | 29.43–30.18 | 30.36–30.50 | 213–244 | 300 |
| present wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording wall | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording join wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |

Groom deformation upload per frame, MiB (range over counted runs): p50 5.397, p95 7.990, max 25.930.

Top passes by p50 (valid samples only):

| pass | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---:|---:|---:|---:|---:|
| ShadowPass | 13.533–14.327 | 14.896–15.744 | 16.445–17.417 | 300 | 3 |
| FoliagePass | 13.155–13.380 | 14.636–14.863 | 14.803–14.975 | 300 | 3 |
| GroomPass | 8.566–8.714 | 14.434–14.727 | 64.181–85.624 | 300 | 3 |
| ScenePass | 2.970–3.003 | 3.226–3.337 | 3.287–3.491 | 300 | 3 |
| ScenePrepassPass | 2.938–2.979 | 3.204–3.285 | 3.284–3.478 | 300 | 3 |
| FoliageCull | 0.148–0.150 | 0.626–2.711 | 6.015–7.443 | 300 | 3 |
| TAAPass | 0.141–0.142 | 0.147–0.151 | 0.150–0.163 | 300 | 3 |
| DiffusionHandoffSnapshotPass | 0.038 | 0.038 | 0.038–0.039 | 300 | 3 |
| DiffusionDepthSnapshotPass | 0.029 | 0.030 | 0.030 | 300 | 3 |
| GroomSceneDepthSnapshotPass | 0.029 | 0.030 | 0.030 | 300 | 3 |
| SkinDiffusionPass | 0.027 | 0.033 | 0.036–0.037 | 300 | 3 |
| GroomSceneEntitySnapshotPass | 0.023 | 0.030–0.031 | 0.031–0.037 | 300 | 3 |
| ToneMapPass | 0.015–0.016 | 0.019–0.020 | 0.020–0.023 | 300 | 3 |
| UICompositePass | 0.009–0.010 | 0.011 | 0.013 | 300 | 3 |
| ParticlePass | 0.006 | 0.014 | 0.016–0.017 | 300 | 3 |

Top sub-passes by p50 (valid samples only):

| sub-pass | parent | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---|---:|---:|---:|---:|---:|
| ShadowPass/Csm | ShadowPass | 12.628–13.357 | 12.801–13.699 | 13.161–14.123 | 300 | 3 |
| ShadowPass/Csm/FoliageCasters | ShadowPass | 10.474–11.190 | 10.915–11.774 | 10.968–11.860 | 300 | 3 |
| GroomPass/StrandShade | GroomPass | 3.620–3.624 | 6.541–6.562 | 6.956–7.024 | 300 | 3 |
| GroomPass/StrandPrepass | GroomPass | 2.987–3.036 | 5.091–5.236 | 5.254–13.825 | 300 | 3 |
| ScenePass/Color | ScenePass | 2.966–2.994 | 3.224–3.335 | 3.284–3.488 | 300 | 3 |
| ScenePrepassPass/DepthPrepass | ScenePrepassPass | 2.927–2.967 | 3.191–3.275 | 3.272–3.464 | 300 | 3 |
| GroomPass/RootFrames | GroomPass | 1.860–1.924 | 2.152–2.323 | 12.030–14.345 | 300 | 3 |
| GroomPass/RootFrames/Dispatch | GroomPass | 1.621–1.682 | 1.890–2.070 | 2.076–2.279 | 300 | 3 |
| ShadowPass/Csm/SkinnedCasters | ShadowPass | 0.914–0.927 | 0.996–1.015 | 1.002–1.221 | 300 | 3 |
| ShadowPass/Csm/FoliageCull | ShadowPass | 0.555–0.558 | 0.568–0.817 | 0.605–0.949 | 300 | 3 |

### rapid-turn (frames/run 300)

| channel | p50 ms | p95 ms | p99 ms | max ms | gate misses | samples |
|---|---:|---:|---:|---:|---:|---:|
| frame time | 30.13–31.03 | 45.02–48.04 | 74.29–79.97 | 114.09–144.23 | 282–287 | 300 |
| CPU | 11.34–13.89 | 21.79–27.80 | 52.99–59.78 | 113.57–143.76 | 36–98 | 300 |
| GPU total (valid) | 30.53–31.46 | 40.82–42.39 | 41.41–45.19 | 80.86–113.41 | 300 | 300 |
| fence wait | 17.20–18.73 | 22.15–24.34 | 23.56–25.58 | 25.36–26.41 | 167–245 | 300 |
| present wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording wall | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording join wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |

Groom deformation upload per frame, MiB (range over counted runs): p50 2.501, p95 4.877, max 8.161.

Top passes by p50 (valid samples only):

| pass | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---:|---:|---:|---:|---:|
| ShadowPass | 13.938–14.865 | 14.796–15.703 | 15.348–16.644 | 300 | 3 |
| FoliagePass | 10.749–10.949 | 14.039–14.112 | 14.355–14.517 | 300 | 3 |
| GroomPass | 1.993–2.029 | 5.664–5.736 | 5.882–6.125 | 300 | 3 |
| ScenePass | 1.416–1.448 | 3.162–3.256 | 3.254–3.511 | 300 | 3 |
| ScenePrepassPass | 1.347–1.362 | 3.163–3.235 | 3.256–3.413 | 300 | 3 |
| FoliageCull | 0.133 | 0.155–0.161 | 1.836–5.812 | 300 | 3 |
| TAAPass | 0.130–0.131 | 0.136–0.139 | 0.140–0.146 | 300 | 3 |
| DiffusionHandoffSnapshotPass | 0.038 | 0.038 | 0.038 | 300 | 3 |
| DiffusionDepthSnapshotPass | 0.029 | 0.030 | 0.030 | 300 | 3 |
| GroomSceneDepthSnapshotPass | 0.029 | 0.030 | 0.030 | 300 | 3 |
| SkinDiffusionPass | 0.027 | 0.031–0.032 | 0.035 | 300 | 3 |
| GroomSceneEntitySnapshotPass | 0.023 | 0.024–0.031 | 0.032–0.033 | 300 | 3 |
| ToneMapPass | 0.015 | 0.019 | 0.020–0.023 | 300 | 3 |
| UICompositePass | 0.009–0.010 | 0.010–0.011 | 0.013 | 300 | 3 |
| ParticlePass | 0.006 | 0.013 | 0.014–0.015 | 300 | 3 |

Top sub-passes by p50 (valid samples only):

| sub-pass | parent | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---|---:|---:|---:|---:|---:|
| ShadowPass/Csm | ShadowPass | 12.270–13.177 | 13.020–13.621 | 13.201–13.655 | 300 | 3 |
| ShadowPass/Csm/FoliageCasters | ShadowPass | 10.845–11.774 | 11.391–11.812 | 11.496–11.988 | 300 | 3 |
| ScenePass/Color | ScenePass | 1.409–1.427 | 3.159–3.252 | 3.252–3.509 | 300 | 3 |
| ScenePrepassPass/DepthPrepass | ScenePrepassPass | 1.334–1.341 | 3.151–3.224 | 3.245–3.400 | 300 | 3 |
| ShadowPass/Atlas | ShadowPass | 1.150–1.175 | 1.252–1.423 | 1.280–1.502 | 203 | 3 |
| GroomPass/StrandShade | GroomPass | 0.979–0.986 | 1.927–1.940 | 1.970–2.004 | 300 | 3 |
| GroomPass/StrandPrepass | GroomPass | 0.655–0.673 | 1.784–1.803 | 1.850–2.073 | 300 | 3 |
| ShadowPass/Atlas/FoliageCull | ShadowPass | 0.602–0.603 | 0.612–0.616 | 0.618–0.863 | 203 | 3 |
| ShadowPass/Csm/FoliageCull | ShadowPass | 0.555–0.557 | 0.565–0.659 | 0.574–0.823 | 300 | 3 |
| ShadowPass/Csm/SkinnedCasters | ShadowPass | 0.408–0.409 | 0.808–0.814 | 0.840–0.848 | 300 | 3 |

Excluded per-pass samples (all counted runs): duplicate-frame-pass 463.

Memory: peak GPU resident 2220.0 MiB, live GPU 1926.6 MiB, peak CPU tracked 92.6 MiB, GPU after scene release 1926.6 MiB.
Report: GPU peak 3338.8 MiB / live 1924.0 MiB; CPU peak 92.6 MiB / live 92.6 MiB; reconciliation notObservable; residency unknown.
Top GPU owners (first counted run): ShadowPass 602.8 MiB, GroomPass 408.2 MiB, Foliage pinned representations 201.8 MiB, TransientPool 174.0 MiB, Unattributed 162.9 MiB, ShadowMap 160.0 MiB, RendererInit 144.3 MiB, TemporalHistory 35.6 MiB, Texture2D 25.3 MiB, EnvironmentMap 4.1 MiB.

## vk-forward: FAIL

run 1 coats-closeup p95 72.325 ms > 16.667 ms; run 1 head-backlit p95 121.902 ms > 16.667 ms; run 1 head-closeup p95 127.882 ms > 16.667 ms; run 1 population-dolly p95 34.225 ms > 16.667 ms; run 1 rapid-turn p95 32.183 ms > 16.667 ms; run 2 coats-closeup p95 79.911 ms > 16.667 ms; run 2 head-backlit p95 114.278 ms > 16.667 ms; run 2 head-closeup p95 124.433 ms > 16.667 ms; run 2 population-dolly p95 31.984 ms > 16.667 ms; run 2 rapid-turn p95 31.846 ms > 16.667 ms; run 3 coats-closeup p95 71.399 ms > 16.667 ms; run 3 head-backlit p95 119.387 ms > 16.667 ms; run 3 head-closeup p95 127.193 ms > 16.667 ms; run 3 population-dolly p95 32.298 ms > 16.667 ms; run 3 rapid-turn p95 32.716 ms > 16.667 ms. Counted runs: 3 of 3.

Host: editor-mcp / vulkan. Frame time = completed editor frame interval; sampling marshals may perturb it.
Internal 1920x1080, display 1920x1080 (native); upscaler resolved/fallback: {"fallback": "none", "resolved": "native"}.
Selected: lightingTap=None, msaaSamples=1, path=Forward, rayTracedShadowsRequested=False, skinDiffusion=True, upscaleMode=0, upscaleTechnique=0, virtualShadowMaps=True; VSM selected: True; pinned by manifest: MSAASampleCount, Path, RayTracedShadowsEnabled, Upscale, UpscaleTechnique, VirtualShadowMaps.
Selected settings are what the renderer was set to, not proof each technique produced or consumed work.

### coats-closeup (frames/run 600)

| channel | p50 ms | p95 ms | p99 ms | max ms | gate misses | samples |
|---|---:|---:|---:|---:|---:|---:|
| frame time | 42.75–44.85 | 71.40–79.91 | 95.37–96.73 | 334.57–343.21 | 600 | 600 |
| CPU | 33.64–35.11 | 70.11–78.75 | 94.24–95.59 | 333.41–342.04 | 600 | 600 |
| GPU total (valid) | 37.26–38.75 | 52.51–60.01 | 74.57–75.73 | 99.76–110.93 | 599 | 599 |
| fence wait | 0.00 | 0.00 | 0.00 | 0.00–0.02 | 0 | 600 |
| present wait | 38.63–40.24 | 71.99–82.15 | 95.92–98.16 | 334.50–343.15 | 600 | 600 |
| recording wall | 0.70 | 0.76–0.79 | 0.81–0.92 | 1.02–1.11 | 0 | 600 |
| recording join wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 600 |

Excluded total-GPU samples (all counted runs): duplicate 3.

Groom deformation upload per frame, MiB (range over counted runs): p50 11.645, p95 11.851, max 22.231.

Top passes by p50 (valid samples only):

| pass | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---:|---:|---:|---:|---:|
| ShadowPass | 23.942–24.652 | 37.244–38.065 | 40.829–41.443 | 599 | 3 |
| FoliagePass | 4.199–4.232 | 4.562–4.613 | 4.656–4.722 | 599 | 3 |
| GroomPass | 4.014–4.037 | 4.458–4.506 | 4.661–4.741 | 599 | 3 |
| FoliagePrepassPass | 1.388–1.391 | 1.612–1.657 | 1.684–1.730 | 599 | 3 |
| ScenePass | 0.544–0.551 | 0.563–0.569 | 0.571–0.578 | 599 | 3 |
| ScenePrepassPass | 0.518–0.529 | 0.547–0.745 | 0.793–0.895 | 599 | 3 |
| GTAOPass | 0.425 | 0.427–0.428 | 0.429–0.759 | 599 | 3 |
| VirtualShadowMapMarkPass | 0.229–0.240 | 0.353–0.402 | 0.410–0.545 | 599 | 3 |
| FoliageCull | 0.173–0.175 | 0.193–0.419 | 0.404–0.520 | 599 | 3 |
| TAAPass | 0.150–0.151 | 0.154–0.155 | 0.156–0.159 | 599 | 3 |
| BloomPass | 0.084 | 0.086 | 0.087–0.089 | 599 | 3 |
| RayTracingScenePass | 0.082 | 0.349–0.352 | 0.563–0.587 | 599 | 3 |
| FXAAPass | 0.033–0.034 | 0.034 | 0.034 | 599 | 3 |
| SkinDiffusionPass | 0.032 | 0.033 | 0.033–0.038 | 599 | 3 |
| GroomSceneDepthSnapshotPass | 0.024 | 0.025–0.026 | 0.026 | 599 | 3 |

Top sub-passes by p50 (valid samples only):

| sub-pass | parent | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---|---:|---:|---:|---:|---:|
| ShadowPass/VsmSun | ShadowPass | 15.471–15.616 | 16.092–16.397 | 16.906–17.536 | 599 | 3 |
| ShadowPass/VsmSun/FoliageCasters | ShadowPass | 8.376–8.624 | 8.865–9.169 | 8.989–9.412 | 599 | 3 |
| ShadowPass/VsmLocal | ShadowPass | 7.246–7.570 | 20.795–21.200 | 24.328–24.477 | 599 | 3 |
| ShadowPass/VsmLocal/FoliageCasters | ShadowPass | 3.745–3.924 | 6.693–6.909 | 7.248–7.399 | 599 | 3 |
| ShadowPass/VsmSun/GroomCasters | ShadowPass | 3.266–3.273 | 3.712–3.735 | 4.370–4.595 | 599 | 3 |
| ShadowPass/VsmSun/FoliageCull | ShadowPass | 2.694–2.730 | 3.101–3.359 | 3.340–3.524 | 599 | 3 |
| ShadowPass/VsmLocal/GroomCasters | ShadowPass | 2.550–2.574 | 13.015–13.446 | 16.426–16.449 | 599 | 3 |
| GroomPass/StrandShade | GroomPass | 1.833–1.838 | 2.088–2.141 | 2.243–2.326 | 599 | 3 |
| GroomPass/StrandPrepass | GroomPass | 1.405–1.412 | 1.630–1.670 | 1.789–1.883 | 599 | 3 |
| ShadowPass/VsmLocal/FoliageCull | ShadowPass | 0.840–0.904 | 1.178–1.203 | 1.264–1.456 | 599 | 3 |

### head-backlit (frames/run 600)

| channel | p50 ms | p95 ms | p99 ms | max ms | gate misses | samples |
|---|---:|---:|---:|---:|---:|---:|
| frame time | 31.37–32.33 | 114.28–121.90 | 120.64–185.29 | 130.96–243.46 | 600 | 600 |
| CPU | 18.22–18.90 | 113.21–120.69 | 119.57–183.44 | 129.89–241.46 | 578–595 | 600 |
| GPU total (valid) | 31.39–32.47 | 94.03–99.48 | 100.45–162.90 | 111.09–214.97 | 600 | 600 |
| fence wait | 0.00 | 0.00 | 0.00 | 0.01 | 0 | 600 |
| present wait | 31.28–32.27 | 116.61–124.86 | 125.20–185.14 | 133.32–243.50 | 600 | 600 |
| recording wall | 0.23 | 0.25–0.30 | 0.28–0.50 | 0.38–0.59 | 0 | 600 |
| recording join wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 600 |

Groom deformation upload per frame, MiB (range over counted runs): p50 4.595, p95 4.696, max 4.696.

Top passes by p50 (valid samples only):

| pass | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---:|---:|---:|---:|---:|
| ShadowPass | 25.055–26.225 | 26.024–29.169 | 26.257–35.211 | 600 | 3 |
| GroomPass | 2.952–2.967 | 3.298–3.455 | 3.382–4.390 | 600 | 3 |
| ScenePass | 0.489–0.500 | 0.499–0.586 | 0.501–0.788 | 600 | 3 |
| GTAOPass | 0.362–0.364 | 0.366–0.426 | 0.367–0.586 | 600 | 3 |
| ScenePrepassPass | 0.229–0.243 | 0.245–0.525 | 0.474–0.605 | 600 | 3 |
| VirtualShadowMapMarkPass | 0.164–0.221 | 0.193–0.495 | 0.432–0.578 | 600 | 3 |
| FoliagePass | 0.213–0.216 | 0.225–0.269 | 0.229–0.370 | 600 | 3 |
| FoliageCull | 0.170–0.186 | 0.413–0.416 | 0.558–0.638 | 600 | 3 |
| SkinDiffusionPass | 0.153 | 0.156–0.185 | 0.157–0.257 | 600 | 3 |
| TAAPass | 0.120–0.121 | 0.123–0.130 | 0.124–0.189 | 600 | 3 |
| FoliagePrepassPass | 0.084–0.099 | 0.319–0.366 | 0.426–0.522 | 600 | 3 |
| RayTracingScenePass | 0.082–0.084 | 0.339–0.348 | 0.365–0.573 | 600 | 3 |
| BloomPass | 0.075 | 0.078–0.087 | 0.078–0.116 | 600 | 3 |
| FXAAPass | 0.025 | 0.026–0.030 | 0.026–0.040 | 600 | 3 |
| GroomSceneDepthSnapshotPass | 0.023 | 0.023–0.027 | 0.024–0.030 | 600 | 3 |

Top sub-passes by p50 (valid samples only):

| sub-pass | parent | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---|---:|---:|---:|---:|---:|
| ShadowPass/VsmLocal | ShadowPass | 18.443–19.558 | 19.279–21.938 | 19.440–26.408 | 600 | 3 |
| ShadowPass/VsmLocal/GroomCasters | ShadowPass | 17.031–18.403 | 17.880–20.585 | 17.960–24.922 | 600 | 3 |
| ShadowPass/VsmSun | ShadowPass | 5.604–5.656 | 6.158–6.646 | 6.355–7.691 | 600 | 3 |
| ShadowPass/VsmSun/FoliageCull | ShadowPass | 2.661–2.666 | 3.123–3.412 | 3.459–3.654 | 600 | 3 |
| GroomPass/StrandShade | GroomPass | 1.806–1.819 | 2.045–2.117 | 2.186–2.567 | 600 | 3 |
| GroomPass/StrandPrepass | GroomPass | 1.153–1.165 | 1.438–1.458 | 1.487–1.778 | 600 | 3 |
| ShadowPass/VsmSun/GroomCasters | ShadowPass | 1.053–1.083 | 1.322–1.419 | 1.457–1.636 | 600 | 3 |
| ShadowPass/VsmLocal/FoliageCull | ShadowPass | 0.900–0.995 | 1.041–1.286 | 1.146–1.365 | 600 | 3 |
| ShadowPass/VsmSun/FoliageCasters | ShadowPass | 0.807–0.809 | 1.033–1.083 | 1.155–1.254 | 600 | 3 |
| ShadowPass/VsmPages | ShadowPass | 0.430–0.622 | 0.746–0.834 | 0.823–0.908 | 600 | 3 |

### head-closeup (frames/run 599–600)

| channel | p50 ms | p95 ms | p99 ms | max ms | gate misses | samples |
|---|---:|---:|---:|---:|---:|---:|
| frame time | 37.99–39.36 | 124.43–127.88 | 130.19–137.87 | 135.30–166.10 | 599–600 | 599–600 |
| CPU | 26.05–26.57 | 123.26–126.72 | 129.05–136.72 | 134.18–164.89 | 599–600 | 599–600 |
| GPU total (valid) | 38.06–39.51 | 102.66–107.17 | 110.58–118.85 | 115.32–196.17 | 599–600 | 599–600 |
| fence wait | 0.00 | 0.00 | 0.00 | 0.01–0.03 | 0 | 599–600 |
| present wait | 37.75–38.75 | 127.42–131.93 | 135.29–148.63 | 142.54–195.58 | 599–600 | 599–600 |
| recording wall | 0.99 | 1.09–1.10 | 1.16–1.26 | 1.49–1.68 | 0 | 599–600 |
| recording join wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 599–600 |

Groom deformation upload per frame, MiB (range over counted runs): p50 8.600, p95 8.600, max 8.600.

Top passes by p50 (valid samples only):

| pass | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---:|---:|---:|---:|---:|
| ShadowPass | 25.871–27.033 | 26.989–28.442 | 27.274–29.316 | 599–600 | 3 |
| GroomPass | 7.941–8.114 | 8.774–8.965 | 8.968–9.232 | 599–600 | 3 |
| ScenePass | 0.781–0.801 | 0.794–0.848 | 0.799–0.913 | 599–600 | 3 |
| ScenePrepassPass | 0.492–0.512 | 0.692–0.744 | 0.777–0.869 | 599–600 | 3 |
| GTAOPass | 0.419–0.420 | 0.422–0.450 | 0.423–0.652 | 599–600 | 3 |
| FoliagePass | 0.399–0.404 | 0.416–0.671 | 0.708–0.768 | 599–600 | 3 |
| VirtualShadowMapMarkPass | 0.186–0.232 | 0.198–0.256 | 0.203–0.258 | 599–600 | 3 |
| FoliagePrepassPass | 0.179 | 0.183–0.193 | 0.188–0.210 | 599–600 | 3 |
| FoliageCull | 0.177–0.178 | 0.404–0.406 | 0.407–0.433 | 599–600 | 3 |
| SkinDiffusionPass | 0.154 | 0.159–0.168 | 0.160–0.181 | 599–600 | 3 |
| TAAPass | 0.126–0.128 | 0.133–0.134 | 0.134–0.137 | 599–600 | 3 |
| RayTracingScenePass | 0.083–0.085 | 0.344–0.348 | 0.561–0.570 | 599–600 | 3 |
| BloomPass | 0.075–0.076 | 0.077–0.080 | 0.078–0.088 | 599–600 | 3 |
| FXAAPass | 0.027 | 0.027–0.028 | 0.028–0.030 | 599–600 | 3 |
| GroomSceneDepthSnapshotPass | 0.023 | 0.024–0.025 | 0.024–0.026 | 599–600 | 3 |

Top sub-passes by p50 (valid samples only):

| sub-pass | parent | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---|---:|---:|---:|---:|---:|
| ShadowPass/VsmLocal | ShadowPass | 18.916–20.020 | 19.899–21.311 | 20.071–21.565 | 599–600 | 3 |
| ShadowPass/VsmLocal/GroomCasters | ShadowPass | 16.986–17.989 | 17.988–19.166 | 18.150–19.331 | 599–600 | 3 |
| ShadowPass/VsmSun | ShadowPass | 5.936–5.953 | 6.369–6.480 | 6.738–6.784 | 599–600 | 3 |
| GroomPass/StrandShade | GroomPass | 3.394–3.480 | 4.035–4.235 | 4.329–4.395 | 599–600 | 3 |
| ShadowPass/VsmSun/FoliageCull | ShadowPass | 2.663–2.671 | 3.105–3.120 | 3.342–3.445 | 599–600 | 3 |
| GroomPass/StrandPrepass | GroomPass | 2.384–2.406 | 2.698–2.803 | 2.761–2.984 | 599–600 | 3 |
| GroomPass/RootFrames | GroomPass | 1.923–1.926 | 2.178–2.257 | 2.316–2.492 | 599–600 | 3 |
| GroomPass/RootFrames/Dispatch | GroomPass | 1.556–1.558 | 1.780–1.887 | 1.905–2.060 | 599–600 | 3 |
| ShadowPass/VsmLocal/FoliageCull | ShadowPass | 1.443–1.476 | 1.704–1.873 | 1.904–2.028 | 599–600 | 3 |
| ShadowPass/VsmSun/GroomCasters | ShadowPass | 1.343–1.358 | 1.570–1.706 | 1.763–1.806 | 599–600 | 3 |

### population-dolly (frames/run 600)

| channel | p50 ms | p95 ms | p99 ms | max ms | gate misses | samples |
|---|---:|---:|---:|---:|---:|---:|
| frame time | 29.41–29.61 | 31.98–34.22 | 70.44–71.55 | 286.06–291.64 | 600 | 600 |
| CPU | 17.25–17.42 | 20.02–20.60 | 69.18–70.40 | 284.83–290.39 | 420–437 | 600 |
| GPU total (valid) | 29.39–29.59 | 31.67–31.81 | 47.09–48.19 | 117.33–122.83 | 597–599 | 597–599 |
| fence wait | 0.00 | 0.00 | 0.00 | 0.01–0.02 | 0 | 600 |
| present wait | 29.34–29.46 | 32.98–35.49 | 79.26–81.00 | 297.47–303.88 | 600 | 600 |
| recording wall | 0.59 | 0.66–0.68 | 0.74–0.78 | 0.87–1.02 | 0 | 600 |
| recording join wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 600 |

Excluded total-GPU samples (all counted runs): duplicate 5.

Groom deformation upload per frame, MiB (range over counted runs): p50 4.465, p95 5.028, max 24.880.

Top passes by p50 (valid samples only):

| pass | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---:|---:|---:|---:|---:|
| ShadowPass | 16.572–16.631 | 17.588–17.639 | 18.798–19.484 | 597–599 | 3 |
| GroomPass | 4.134–4.206 | 5.491–5.608 | 6.373–6.419 | 597–599 | 3 |
| FoliagePass | 3.662–3.772 | 3.922–4.142 | 4.092–4.217 | 597–599 | 3 |
| FoliagePrepassPass | 1.735–1.741 | 2.021–2.098 | 2.114–2.161 | 597–599 | 3 |
| ScenePrepassPass | 0.641–0.654 | 0.738–0.862 | 1.037–1.089 | 597–599 | 3 |
| ScenePass | 0.633–0.648 | 0.681–0.695 | 0.689–0.706 | 597–599 | 3 |
| GTAOPass | 0.350–0.352 | 0.493–0.496 | 0.591–0.790 | 597–599 | 3 |
| FoliageCull | 0.201–0.204 | 0.417–0.433 | 0.553–0.685 | 597–599 | 3 |
| TAAPass | 0.135–0.136 | 0.139–0.140 | 0.143–0.147 | 597–599 | 3 |
| VirtualShadowMapMarkPass | 0.077–0.091 | 0.092–0.104 | 0.110–0.136 | 597–599 | 3 |
| RayTracingScenePass | 0.084–0.085 | 0.350–0.553 | 0.587–0.601 | 597–599 | 3 |
| BloomPass | 0.081 | 0.083 | 0.296–0.418 | 597–599 | 3 |
| SkinDiffusionPass | 0.032 | 0.033 | 0.033 | 597–599 | 3 |
| FXAAPass | 0.031 | 0.032 | 0.032 | 597–599 | 3 |
| GroomSceneDepthSnapshotPass | 0.024–0.025 | 0.025–0.026 | 0.026 | 597–599 | 3 |

Top sub-passes by p50 (valid samples only):

| sub-pass | parent | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---|---:|---:|---:|---:|---:|
| ShadowPass/VsmSun | ShadowPass | 15.336–15.380 | 16.316–16.387 | 17.549–18.230 | 597–599 | 3 |
| ShadowPass/VsmSun/FoliageCasters | ShadowPass | 9.345–9.590 | 10.619–10.726 | 10.759–11.039 | 597–599 | 3 |
| ShadowPass/VsmSun/FoliageCull | ShadowPass | 3.087–3.198 | 3.543–3.557 | 3.559–3.932 | 597–599 | 3 |
| GroomPass/RootFrames | GroomPass | 1.336–1.422 | 1.575–1.701 | 1.743–1.829 | 597–599 | 3 |
| GroomPass/StrandShade | GroomPass | 1.330–1.380 | 2.029–2.216 | 2.565–2.582 | 597–599 | 3 |
| ShadowPass/VsmSun/GroomCasters | ShadowPass | 1.276–1.376 | 2.983–3.086 | 4.801–5.008 | 597–599 | 3 |
| GroomPass/StrandPrepass | GroomPass | 1.168–1.273 | 1.846–1.901 | 2.180–2.194 | 597–599 | 3 |
| GroomPass/RootFrames/Dispatch | GroomPass | 1.075–1.115 | 1.250–1.390 | 1.431–1.560 | 597–599 | 3 |
| ShadowPass/VsmSun/TerrainCasters | ShadowPass | 0.822–0.842 | 0.880–0.889 | 0.900–1.176 | 597–599 | 3 |
| ShadowPass/VsmPages | ShadowPass | 0.496–0.675 | 0.721–0.809 | 0.940–1.090 | 597–599 | 3 |

### rapid-turn (frames/run 600)

| channel | p50 ms | p95 ms | p99 ms | max ms | gate misses | samples |
|---|---:|---:|---:|---:|---:|---:|
| frame time | 26.05–26.57 | 31.85–32.72 | 47.63–48.69 | 77.58–79.20 | 592–594 | 600 |
| CPU | 12.62–12.74 | 17.00–17.57 | 46.49–47.57 | 76.45–77.98 | 43–62 | 600 |
| GPU total (valid) | 26.02–26.54 | 31.69–32.50 | 32.06–32.87 | 54.29–55.90 | 600 | 600 |
| fence wait | 0.00 | 0.00 | 0.01 | 0.01–0.04 | 0 | 600 |
| present wait | 25.96–26.55 | 32.38–33.07 | 60.05–60.88 | 91.00–93.41 | 584–587 | 600 |
| recording wall | 0.45 | 0.62 | 0.65–0.66 | 0.70–0.77 | 0 | 600 |
| recording join wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 600 |

Groom deformation upload per frame, MiB (range over counted runs): p50 2.003, p95 4.293, max 7.664.

Top passes by p50 (valid samples only):

| pass | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---:|---:|---:|---:|---:|
| ShadowPass | 16.835–17.314 | 19.938–20.277 | 20.338–20.578 | 600 | 3 |
| FoliagePass | 3.596–3.677 | 3.852–4.122 | 4.072–4.199 | 600 | 3 |
| FoliagePrepassPass | 1.570–1.579 | 1.931–2.029 | 2.114–2.273 | 600 | 3 |
| GroomPass | 1.471–1.484 | 3.708–3.731 | 3.918–4.071 | 600 | 3 |
| ScenePrepassPass | 0.346–0.350 | 0.675–0.703 | 0.693–0.920 | 600 | 3 |
| ScenePass | 0.340–0.345 | 0.662–0.680 | 0.672–0.694 | 600 | 3 |
| GTAOPass | 0.301–0.303 | 0.459–0.460 | 0.462–0.614 | 600 | 3 |
| FoliageCull | 0.175–0.177 | 0.407–0.414 | 0.429–0.589 | 600 | 3 |
| TAAPass | 0.126–0.127 | 0.131–0.133 | 0.133–0.137 | 600 | 3 |
| RayTracingScenePass | 0.084 | 0.348–0.350 | 0.573–0.584 | 600 | 3 |
| BloomPass | 0.080–0.081 | 0.082–0.083 | 0.085–0.089 | 600 | 3 |
| VirtualShadowMapMarkPass | 0.057–0.061 | 0.078–0.081 | 0.081–0.084 | 600 | 3 |
| SkinDiffusionPass | 0.032 | 0.033 | 0.033 | 600 | 3 |
| FXAAPass | 0.028 | 0.030 | 0.030–0.031 | 600 | 3 |
| GroomSceneDepthSnapshotPass | 0.024 | 0.024–0.026 | 0.025–0.026 | 600 | 3 |

Top sub-passes by p50 (valid samples only):

| sub-pass | parent | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---|---:|---:|---:|---:|---:|
| ShadowPass/VsmSun | ShadowPass | 14.627–14.979 | 15.377–15.600 | 15.619–15.850 | 600 | 3 |
| ShadowPass/VsmSun/FoliageCasters | ShadowPass | 10.038–10.328 | 10.670–10.711 | 10.787–10.943 | 600 | 3 |
| ShadowPass/VsmSun/FoliageCull | ShadowPass | 2.725–2.742 | 3.188–3.350 | 3.416–3.535 | 600 | 3 |
| ShadowPass/VsmLocal | ShadowPass | 1.215–1.232 | 3.495–3.667 | 3.749–3.839 | 600 | 3 |
| ShadowPass/VsmSun/TerrainCasters | ShadowPass | 0.873–0.884 | 0.889–0.898 | 0.895–1.083 | 600 | 3 |
| ShadowPass/VsmLocal/FoliageCull | ShadowPass | 0.813–0.815 | 0.936–1.104 | 1.089–1.211 | 406 | 3 |
| GroomPass/StrandShade | GroomPass | 0.758–0.770 | 1.218–1.234 | 1.260–1.378 | 600 | 3 |
| ShadowPass/VsmSun/GroomCasters | ShadowPass | 0.717–0.721 | 0.918–0.940 | 1.008–1.172 | 600 | 3 |
| ShadowPass/VsmPages | ShadowPass | 0.545–0.549 | 0.772–0.934 | 0.992–1.090 | 600 | 3 |
| GroomPass/StrandPrepass | GroomPass | 0.429–0.433 | 1.085–1.103 | 1.221–1.368 | 600 | 3 |

Excluded per-pass samples (all counted runs): duplicate-frame-pass 416.

Memory: peak GPU resident 3135.7 MiB, live GPU 2674.7 MiB, peak CPU tracked 82.6 MiB, GPU after scene release n/a.
Report: GPU peak 3854.0 MiB / live 2670.3 MiB; CPU peak 82.6 MiB / live 82.6 MiB; reconciliation reconciled; residency osReported.
Top GPU owners (first counted run): ShadowPass 691.1 MiB, ShadowMap 640.0 MiB, Unattributed 414.1 MiB, GroomPass 274.3 MiB, TransientPool 264.3 MiB, Foliage pinned representations 139.2 MiB, RendererInit 131.8 MiB, TemporalHistory 38.4 MiB, Texture2D 32.0 MiB, VulkanFrameArena 32.0 MiB.

## vk-forward-plus: FAIL

run 1 coats-closeup p95 72.318 ms > 16.667 ms; run 1 head-backlit p95 115.970 ms > 16.667 ms; run 1 head-closeup p95 126.734 ms > 16.667 ms; run 1 population-dolly p95 31.460 ms > 16.667 ms; run 1 rapid-turn p95 37.449 ms > 16.667 ms; run 2 coats-closeup p95 79.208 ms > 16.667 ms; run 2 head-backlit p95 117.029 ms > 16.667 ms; run 2 head-closeup p95 124.044 ms > 16.667 ms; run 2 population-dolly p95 33.091 ms > 16.667 ms; run 2 rapid-turn p95 38.385 ms > 16.667 ms; run 3 coats-closeup p95 73.515 ms > 16.667 ms; run 3 head-backlit p95 114.392 ms > 16.667 ms; run 3 head-closeup p95 126.029 ms > 16.667 ms; run 3 population-dolly p95 31.973 ms > 16.667 ms; run 3 rapid-turn p95 33.262 ms > 16.667 ms. Counted runs: 3 of 3.

Host: editor-mcp / vulkan. Frame time = completed editor frame interval; sampling marshals may perturb it.
Internal 1920x1080, display 1920x1080 (native); upscaler resolved/fallback: {"fallback": "none", "resolved": "native"}.
Selected: lightingTap=None, msaaSamples=1, path=ForwardPlus, rayTracedShadowsRequested=False, skinDiffusion=True, upscaleMode=0, upscaleTechnique=0, virtualShadowMaps=True; VSM selected: True; pinned by manifest: MSAASampleCount, Path, RayTracedShadowsEnabled, Upscale, UpscaleTechnique, VirtualShadowMaps.
Selected settings are what the renderer was set to, not proof each technique produced or consumed work.

### coats-closeup (frames/run 599–600)

| channel | p50 ms | p95 ms | p99 ms | max ms | gate misses | samples |
|---|---:|---:|---:|---:|---:|---:|
| frame time | 42.39–52.47 | 72.32–79.21 | 94.91–109.00 | 343.54–353.72 | 599–600 | 599–600 |
| CPU | 32.28–32.84 | 71.19–73.83 | 93.68–98.80 | 338.61–352.50 | 599–600 | 599–600 |
| GPU total (valid) | 37.30–38.31 | 52.98–61.18 | 72.90–82.24 | 102.98–172.47 | 593–599 | 593–599 |
| fence wait | 0.00 | 0.00 | 0.00 | 0.00–0.02 | 0 | 599–600 |
| present wait | 34.56–53.24 | 73.63–79.91 | 94.75–111.04 | 343.40–353.57 | 599–600 | 599–600 |
| recording wall | 0.70 | 0.75–0.78 | 0.81 | 0.97–1.10 | 0 | 599–600 |
| recording join wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 599–600 |

Excluded total-GPU samples (all counted runs): duplicate 8.

Groom deformation upload per frame, MiB (range over counted runs): p50 11.645, p95 11.851, max 22.231.

Top passes by p50 (valid samples only):

| pass | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---:|---:|---:|---:|---:|
| ShadowPass | 23.558–24.554 | 37.513–37.645 | 40.501–42.155 | 593–599 | 3 |
| FoliagePass | 4.189–4.258 | 4.513–4.570 | 4.674–4.813 | 593–599 | 3 |
| GroomPass | 4.013–4.040 | 4.452–4.497 | 4.553–4.750 | 593–599 | 3 |
| FoliagePrepassPass | 1.389–1.391 | 1.435–1.666 | 1.699–1.825 | 593–599 | 3 |
| ScenePrepassPass | 0.520–0.532 | 0.531–0.753 | 0.809–0.849 | 593–599 | 3 |
| ScenePass | 0.521–0.523 | 0.544–0.547 | 0.548–0.551 | 593–599 | 3 |
| GTAOPass | 0.425 | 0.427–0.428 | 0.435–0.708 | 593–599 | 3 |
| VirtualShadowMapMarkPass | 0.233–0.248 | 0.350–0.418 | 0.358–0.656 | 593–599 | 3 |
| FoliageCull | 0.172–0.174 | 0.186–0.401 | 0.401–0.417 | 593–599 | 3 |
| TAAPass | 0.151–0.152 | 0.155–0.157 | 0.156–0.161 | 593–599 | 3 |
| BloomPass | 0.084 | 0.085–0.086 | 0.087–0.090 | 593–599 | 3 |
| RayTracingScenePass | 0.081–0.082 | 0.348–0.360 | 0.578–0.581 | 593–599 | 3 |
| FXAAPass | 0.033–0.034 | 0.034 | 0.034–0.035 | 593–599 | 3 |
| SkinDiffusionPass | 0.032 | 0.033 | 0.033 | 593–599 | 3 |
| GroomSceneDepthSnapshotPass | 0.024 | 0.025 | 0.026–0.028 | 593–599 | 3 |

Top sub-passes by p50 (valid samples only):

| sub-pass | parent | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---|---:|---:|---:|---:|---:|
| ShadowPass/VsmSun | ShadowPass | 15.304–15.675 | 16.208–16.272 | 16.867–17.320 | 593–599 | 3 |
| ShadowPass/VsmSun/FoliageCasters | ShadowPass | 8.447–8.677 | 8.974–9.053 | 9.127–9.268 | 593–599 | 3 |
| ShadowPass/VsmLocal | ShadowPass | 6.274–7.499 | 20.654–21.278 | 23.982–26.084 | 593–599 | 3 |
| ShadowPass/VsmLocal/FoliageCasters | ShadowPass | 2.794–3.866 | 6.839–6.936 | 7.231–7.429 | 593–599 | 3 |
| ShadowPass/VsmSun/GroomCasters | ShadowPass | 3.214–3.250 | 3.717–3.789 | 4.364–4.410 | 593–599 | 3 |
| ShadowPass/VsmSun/FoliageCull | ShadowPass | 2.683–2.691 | 3.105–3.319 | 3.218–3.491 | 593–599 | 3 |
| ShadowPass/VsmLocal/GroomCasters | ShadowPass | 2.544–2.593 | 12.914–13.060 | 16.351–17.964 | 593–599 | 3 |
| GroomPass/StrandShade | GroomPass | 1.844–1.851 | 2.073–2.119 | 2.219–2.266 | 593–599 | 3 |
| GroomPass/StrandPrepass | GroomPass | 1.404–1.407 | 1.604–1.673 | 1.772–1.932 | 593–599 | 3 |
| ShadowPass/VsmLocal/FoliageCull | ShadowPass | 0.831–0.840 | 1.141–1.183 | 1.336–1.354 | 593–599 | 3 |

### head-backlit (frames/run 600)

| channel | p50 ms | p95 ms | p99 ms | max ms | gate misses | samples |
|---|---:|---:|---:|---:|---:|---:|
| frame time | 31.24–32.22 | 114.39–117.03 | 124.26–135.64 | 137.46–170.38 | 600 | 600 |
| CPU | 18.40–18.60 | 113.33–115.89 | 123.16–130.95 | 136.40–164.94 | 586–596 | 600 |
| GPU total (valid) | 31.04–31.42 | 94.03–98.86 | 102.97–128.29 | 117.05–147.66 | 596–600 | 596–600 |
| fence wait | 0.00 | 0.00 | 0.00 | 0.01 | 0 | 600 |
| present wait | 31.11–32.59 | 119.30–122.17 | 127.72–140.22 | 137.44–170.16 | 600 | 600 |
| recording wall | 0.24 | 0.26–0.38 | 0.28–0.53 | 0.39–0.61 | 0 | 600 |
| recording join wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 600 |

Excluded total-GPU samples (all counted runs): duplicate 4.

Groom deformation upload per frame, MiB (range over counted runs): p50 4.595, p95 4.696, max 4.696.

Top passes by p50 (valid samples only):

| pass | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---:|---:|---:|---:|---:|
| ShadowPass | 24.390–25.100 | 25.852–26.811 | 26.319–28.344 | 596–600 | 3 |
| GroomPass | 3.045–3.061 | 3.336–3.587 | 3.427–3.849 | 596–600 | 3 |
| ScenePass | 0.445–0.449 | 0.450–0.506 | 0.453–0.535 | 596–600 | 3 |
| GTAOPass | 0.362–0.365 | 0.366–0.413 | 0.367–0.442 | 596–600 | 3 |
| ScenePrepassPass | 0.230–0.246 | 0.242–0.485 | 0.494–0.568 | 596–600 | 3 |
| FoliagePass | 0.214–0.215 | 0.224–0.251 | 0.226–0.273 | 596–600 | 3 |
| VirtualShadowMapMarkPass | 0.146–0.214 | 0.153–0.245 | 0.420–0.552 | 596–600 | 3 |
| FoliageCull | 0.169–0.185 | 0.396–0.483 | 0.598–0.637 | 596–600 | 3 |
| SkinDiffusionPass | 0.153 | 0.156–0.176 | 0.157–0.189 | 596–600 | 3 |
| TAAPass | 0.120–0.121 | 0.123–0.126 | 0.124–0.141 | 596–600 | 3 |
| FoliagePrepassPass | 0.084–0.086 | 0.086–0.399 | 0.356–0.570 | 596–600 | 3 |
| RayTracingScenePass | 0.067–0.083 | 0.341–0.347 | 0.361–0.569 | 596–600 | 3 |
| BloomPass | 0.075 | 0.077–0.084 | 0.078–0.095 | 596–600 | 3 |
| FXAAPass | 0.025 | 0.026–0.028 | 0.026–0.030 | 596–600 | 3 |
| GroomSceneDepthSnapshotPass | 0.023 | 0.023–0.025 | 0.024–0.027 | 596–600 | 3 |

Top sub-passes by p50 (valid samples only):

| sub-pass | parent | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---|---:|---:|---:|---:|---:|
| ShadowPass/VsmLocal | ShadowPass | 17.994–18.507 | 18.948–19.659 | 19.388–20.822 | 596–600 | 3 |
| ShadowPass/VsmLocal/GroomCasters | ShadowPass | 16.792–17.292 | 17.881–18.196 | 17.933–19.083 | 596–600 | 3 |
| ShadowPass/VsmSun | ShadowPass | 5.492–5.732 | 6.199–6.318 | 6.563–6.895 | 596–600 | 3 |
| ShadowPass/VsmSun/FoliageCull | ShadowPass | 2.664–2.671 | 3.117–3.155 | 3.473–3.584 | 596–600 | 3 |
| GroomPass/StrandShade | GroomPass | 1.904–1.909 | 2.003–2.171 | 2.232–2.408 | 596–600 | 3 |
| GroomPass/StrandPrepass | GroomPass | 1.128–1.140 | 1.368–1.467 | 1.460–1.671 | 596–600 | 3 |
| ShadowPass/VsmSun/GroomCasters | ShadowPass | 1.043–1.077 | 1.304–1.454 | 1.425–1.647 | 596–600 | 3 |
| ShadowPass/VsmLocal/FoliageCull | ShadowPass | 0.670–0.997 | 1.168–1.247 | 1.272–1.410 | 596–600 | 3 |
| ShadowPass/VsmSun/FoliageCasters | ShadowPass | 0.830–0.836 | 0.850–1.167 | 1.190–1.280 | 596–600 | 3 |
| ShadowPass/VsmPages | ShadowPass | 0.423–0.621 | 0.652–0.795 | 0.839–0.869 | 596–600 | 3 |

### head-closeup (frames/run 600)

| channel | p50 ms | p95 ms | p99 ms | max ms | gate misses | samples |
|---|---:|---:|---:|---:|---:|---:|
| frame time | 37.96–38.98 | 124.04–126.73 | 132.84–134.88 | 143.76–160.55 | 600 | 600 |
| CPU | 26.16–26.74 | 122.87–125.60 | 131.77–133.73 | 142.39–159.00 | 600 | 600 |
| GPU total (valid) | 38.09–39.02 | 102.83–104.31 | 113.16–114.75 | 123.44–141.33 | 600 | 600 |
| fence wait | 0.00 | 0.00 | 0.00 | 0.00–0.01 | 0 | 600 |
| present wait | 37.44–38.54 | 127.14–129.78 | 138.14–139.02 | 155.44–160.22 | 600 | 600 |
| recording wall | 0.99 | 1.07–1.11 | 1.13–1.40 | 1.37–1.82 | 0 | 600 |
| recording join wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 600 |

Groom deformation upload per frame, MiB (range over counted runs): p50 8.600, p95 8.600, max 8.600.

Top passes by p50 (valid samples only):

| pass | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---:|---:|---:|---:|---:|
| ShadowPass | 25.665–26.455 | 26.873–27.438 | 27.460–27.671 | 600 | 3 |
| GroomPass | 8.076–8.188 | 8.972–9.369 | 9.047–9.666 | 600 | 3 |
| ScenePass | 0.669–0.672 | 0.677–0.699 | 0.687–0.732 | 600 | 3 |
| ScenePrepassPass | 0.509–0.524 | 0.736–0.834 | 0.801–0.991 | 600 | 3 |
| GTAOPass | 0.420–0.421 | 0.423–0.438 | 0.424–0.630 | 600 | 3 |
| FoliagePass | 0.401–0.403 | 0.416–0.429 | 0.418–0.450 | 600 | 3 |
| VirtualShadowMapMarkPass | 0.161–0.231 | 0.170–0.254 | 0.173–0.256 | 600 | 3 |
| FoliagePrepassPass | 0.179–0.180 | 0.183–0.191 | 0.184–0.202 | 600 | 3 |
| FoliageCull | 0.177–0.178 | 0.404–0.407 | 0.409–0.501 | 600 | 3 |
| SkinDiffusionPass | 0.154 | 0.159–0.165 | 0.160–0.174 | 600 | 3 |
| TAAPass | 0.127–0.128 | 0.133–0.134 | 0.134–0.136 | 600 | 3 |
| RayTracingScenePass | 0.084–0.085 | 0.350–0.356 | 0.559–0.574 | 600 | 3 |
| BloomPass | 0.076 | 0.077–0.079 | 0.077–0.083 | 600 | 3 |
| FXAAPass | 0.027 | 0.028 | 0.028–0.029 | 600 | 3 |
| GroomSceneDepthSnapshotPass | 0.023 | 0.024–0.025 | 0.024–0.276 | 600 | 3 |

Top sub-passes by p50 (valid samples only):

| sub-pass | parent | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---|---:|---:|---:|---:|---:|
| ShadowPass/VsmLocal | ShadowPass | 18.641–19.344 | 19.780–20.234 | 20.004–20.486 | 600 | 3 |
| ShadowPass/VsmLocal/GroomCasters | ShadowPass | 16.633–17.414 | 17.709–18.290 | 17.901–18.530 | 600 | 3 |
| ShadowPass/VsmSun | ShadowPass | 5.967–5.996 | 6.427–6.654 | 6.766–6.991 | 600 | 3 |
| GroomPass/StrandShade | GroomPass | 3.494–3.526 | 4.169–4.478 | 4.478–4.790 | 600 | 3 |
| ShadowPass/VsmSun/FoliageCull | ShadowPass | 2.674–2.679 | 3.121–3.193 | 3.364–3.465 | 600 | 3 |
| GroomPass/StrandPrepass | GroomPass | 2.402–2.407 | 2.750–3.015 | 2.758–3.229 | 600 | 3 |
| GroomPass/RootFrames | GroomPass | 1.926–1.929 | 2.191–2.268 | 2.271–2.564 | 600 | 3 |
| GroomPass/RootFrames/Dispatch | GroomPass | 1.559–1.563 | 1.819–1.888 | 1.904–2.062 | 600 | 3 |
| ShadowPass/VsmLocal/FoliageCull | ShadowPass | 1.451–1.475 | 1.717–1.902 | 1.919–1.947 | 600 | 3 |
| ShadowPass/VsmSun/GroomCasters | ShadowPass | 1.347–1.358 | 1.619–1.701 | 1.734–1.915 | 600 | 3 |

### population-dolly (frames/run 600)

| channel | p50 ms | p95 ms | p99 ms | max ms | gate misses | samples |
|---|---:|---:|---:|---:|---:|---:|
| frame time | 28.81–28.98 | 31.46–33.09 | 68.93–74.70 | 287.05–291.17 | 600 | 600 |
| CPU | 17.24–17.56 | 19.86–20.38 | 67.79–73.53 | 285.91–289.87 | 435–481 | 600 |
| GPU total (valid) | 28.82–28.98 | 31.30–31.85 | 45.47–49.25 | 121.26–122.75 | 597–599 | 597–599 |
| fence wait | 0.00 | 0.00 | 0.00 | 0.01 | 0 | 600 |
| present wait | 28.67–29.10 | 32.57–33.41 | 77.61–83.77 | 298.58–302.49 | 600 | 600 |
| recording wall | 0.60–0.61 | 0.67–0.70 | 0.73–0.76 | 0.82–0.97 | 0 | 600 |
| recording join wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 600 |

Excluded total-GPU samples (all counted runs): duplicate 5.

Groom deformation upload per frame, MiB (range over counted runs): p50 4.465, p95 5.028, max 24.880.

Top passes by p50 (valid samples only):

| pass | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---:|---:|---:|---:|---:|
| ShadowPass | 16.253–16.479 | 17.345–17.721 | 19.070–19.505 | 597–599 | 3 |
| GroomPass | 3.972–4.082 | 5.275–5.621 | 6.355–6.401 | 597–599 | 3 |
| FoliagePass | 3.665–3.774 | 3.802–4.146 | 4.114–4.231 | 597–599 | 3 |
| FoliagePrepassPass | 1.737–1.740 | 2.016–2.092 | 2.121–2.247 | 597–599 | 3 |
| ScenePrepassPass | 0.648–0.669 | 0.711–0.788 | 0.897–1.099 | 597–599 | 3 |
| ScenePass | 0.561–0.564 | 0.601–0.603 | 0.606–0.611 | 597–599 | 3 |
| GTAOPass | 0.353–0.354 | 0.495–0.496 | 0.532–0.695 | 597–599 | 3 |
| FoliageCull | 0.186–0.187 | 0.412–0.416 | 0.416–0.590 | 597–599 | 3 |
| TAAPass | 0.135–0.136 | 0.139–0.141 | 0.140–0.146 | 597–599 | 3 |
| VirtualShadowMapMarkPass | 0.081–0.087 | 0.098–0.102 | 0.126–0.345 | 597–599 | 3 |
| RayTracingScenePass | 0.084 | 0.348–0.543 | 0.582–0.593 | 597–599 | 3 |
| BloomPass | 0.081 | 0.082–0.084 | 0.085–0.087 | 597–599 | 3 |
| SkinDiffusionPass | 0.032 | 0.033 | 0.033 | 597–599 | 3 |
| FXAAPass | 0.031 | 0.032 | 0.032–0.033 | 597–599 | 3 |
| GroomSceneDepthSnapshotPass | 0.024–0.025 | 0.025 | 0.026 | 597–599 | 3 |

Top sub-passes by p50 (valid samples only):

| sub-pass | parent | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---|---:|---:|---:|---:|---:|
| ShadowPass/VsmSun | ShadowPass | 15.058–15.256 | 16.103–16.445 | 17.805–18.237 | 597–599 | 3 |
| ShadowPass/VsmSun/FoliageCasters | ShadowPass | 9.415–9.571 | 10.542–10.747 | 10.707–11.150 | 597–599 | 3 |
| ShadowPass/VsmSun/FoliageCull | ShadowPass | 2.738–2.751 | 3.068–3.186 | 3.196–3.463 | 597–599 | 3 |
| ShadowPass/VsmSun/GroomCasters | ShadowPass | 1.351–1.407 | 3.004–3.073 | 4.870–5.052 | 597–599 | 3 |
| GroomPass/RootFrames | GroomPass | 1.280–1.333 | 1.492–1.708 | 1.635–1.846 | 597–599 | 3 |
| GroomPass/StrandShade | GroomPass | 1.311–1.323 | 1.968–2.188 | 2.559–2.588 | 597–599 | 3 |
| GroomPass/StrandPrepass | GroomPass | 1.150–1.155 | 1.627–1.897 | 2.176–2.196 | 597–599 | 3 |
| GroomPass/RootFrames/Dispatch | GroomPass | 1.034–1.042 | 1.206–1.340 | 1.397–1.550 | 597–599 | 3 |
| ShadowPass/VsmSun/TerrainCasters | ShadowPass | 0.817–0.889 | 0.864–0.942 | 0.876–0.950 | 597–599 | 3 |
| ScenePrepassPass/DepthPrepass | ScenePrepassPass | 0.613–0.630 | 0.674–0.691 | 0.684–0.703 | 597–599 | 3 |

### rapid-turn (frames/run 599–600)

| channel | p50 ms | p95 ms | p99 ms | max ms | gate misses | samples |
|---|---:|---:|---:|---:|---:|---:|
| frame time | 26.15–27.64 | 33.26–38.38 | 48.14–51.71 | 77.75–78.95 | 589–592 | 599–600 |
| CPU | 12.58–12.84 | 17.07–17.47 | 46.96–49.94 | 76.54–77.77 | 51–79 | 599–600 |
| GPU total (valid) | 26.01–26.53 | 30.33–32.95 | 31.03–33.47 | 54.33–56.30 | 596–600 | 596–600 |
| fence wait | 0.00 | 0.00 | 0.00 | 0.01 | 0 | 599–600 |
| present wait | 26.18–27.73 | 33.39–38.43 | 60.55–63.96 | 90.91–92.17 | 580–585 | 599–600 |
| recording wall | 0.45 | 0.62 | 0.66 | 0.69–0.83 | 0 | 599–600 |
| recording join wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 599–600 |

Excluded total-GPU samples (all counted runs): duplicate 4.

Groom deformation upload per frame, MiB (range over counted runs): p50 2.003, p95 4.293, max 7.664.

Top passes by p50 (valid samples only):

| pass | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---:|---:|---:|---:|---:|
| ShadowPass | 16.852–17.270 | 18.295–20.704 | 18.896–21.074 | 596–600 | 3 |
| FoliagePass | 3.526–3.673 | 3.839–4.079 | 4.139–4.530 | 596–600 | 3 |
| FoliagePrepassPass | 1.564–1.584 | 1.909–1.996 | 1.955–2.224 | 596–600 | 3 |
| GroomPass | 1.445–1.476 | 3.694–3.797 | 3.827–4.144 | 596–600 | 3 |
| ScenePrepassPass | 0.349–0.354 | 0.681–0.703 | 0.694–0.981 | 596–600 | 3 |
| ScenePass | 0.321–0.323 | 0.586–0.590 | 0.595–0.600 | 596–600 | 3 |
| GTAOPass | 0.302–0.303 | 0.459–0.460 | 0.463–0.635 | 596–600 | 3 |
| FoliageCull | 0.171–0.181 | 0.407–0.421 | 0.416–0.435 | 596–600 | 3 |
| TAAPass | 0.127–0.128 | 0.132–0.133 | 0.134–0.138 | 596–600 | 3 |
| RayTracingScenePass | 0.084–0.086 | 0.348–0.360 | 0.566–0.583 | 596–600 | 3 |
| BloomPass | 0.080–0.081 | 0.082–0.083 | 0.083–0.296 | 596–600 | 3 |
| VirtualShadowMapMarkPass | 0.054–0.061 | 0.073–0.080 | 0.075–0.083 | 596–600 | 3 |
| SkinDiffusionPass | 0.032 | 0.033 | 0.033 | 596–600 | 3 |
| FXAAPass | 0.028 | 0.030 | 0.031 | 596–600 | 3 |
| GroomSceneDepthSnapshotPass | 0.023 | 0.024–0.025 | 0.025–0.026 | 596–600 | 3 |

Top sub-passes by p50 (valid samples only):

| sub-pass | parent | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---|---:|---:|---:|---:|---:|
| ShadowPass/VsmSun | ShadowPass | 14.545–15.118 | 15.217–15.694 | 15.484–16.274 | 596–600 | 3 |
| ShadowPass/VsmSun/FoliageCasters | ShadowPass | 10.088–10.306 | 10.437–10.906 | 10.780–11.288 | 596–600 | 3 |
| ShadowPass/VsmSun/FoliageCull | ShadowPass | 2.735–3.162 | 3.079–3.514 | 3.200–3.661 | 596–600 | 3 |
| ShadowPass/VsmLocal | ShadowPass | 1.209–1.234 | 2.154–3.867 | 2.262–4.016 | 596–600 | 3 |
| ShadowPass/VsmSun/TerrainCasters | ShadowPass | 0.868–0.885 | 0.882–0.942 | 0.886–0.951 | 596–600 | 3 |
| ShadowPass/VsmLocal/FoliageCull | ShadowPass | 0.812–0.814 | 0.826–1.114 | 1.160–1.278 | 402–406 | 3 |
| GroomPass/StrandShade | GroomPass | 0.743–0.753 | 1.221–1.228 | 1.250–1.433 | 596–600 | 3 |
| ShadowPass/VsmSun/GroomCasters | ShadowPass | 0.708–0.720 | 0.922–0.989 | 0.929–1.134 | 596–600 | 3 |
| ShadowPass/VsmPages | ShadowPass | 0.544–0.545 | 0.767–0.903 | 0.777–0.994 | 596–600 | 3 |
| GroomPass/StrandPrepass | GroomPass | 0.408–0.442 | 1.080–1.095 | 1.130–1.303 | 596–600 | 3 |

Excluded per-pass samples (all counted runs): duplicate-frame-pass 1119.

Memory: peak GPU resident 3135.7 MiB, live GPU 2674.7 MiB, peak CPU tracked 82.6 MiB, GPU after scene release n/a.
Report: GPU peak 3854.0 MiB / live 2670.3 MiB; CPU peak 82.6 MiB / live 82.6 MiB; reconciliation reconciled; residency osReported.
Top GPU owners (first counted run): ShadowPass 691.1 MiB, ShadowMap 640.0 MiB, Unattributed 414.1 MiB, GroomPass 274.3 MiB, TransientPool 264.3 MiB, Foliage pinned representations 139.2 MiB, RendererInit 131.8 MiB, TemporalHistory 38.4 MiB, Texture2D 32.0 MiB, VulkanFrameArena 32.0 MiB.

## vk-deferred: FAIL

run 1 coats-closeup p95 73.001 ms > 16.667 ms; run 1 head-backlit p95 115.617 ms > 16.667 ms; run 1 head-closeup p95 128.314 ms > 16.667 ms; run 1 population-dolly p95 31.503 ms > 16.667 ms; run 1 rapid-turn p95 32.459 ms > 16.667 ms; run 2 coats-closeup p95 84.608 ms > 16.667 ms; run 2 head-backlit p95 118.364 ms > 16.667 ms; run 2 head-closeup p95 124.508 ms > 16.667 ms; run 2 population-dolly p95 31.564 ms > 16.667 ms; run 2 rapid-turn p95 33.187 ms > 16.667 ms; run 3 coats-closeup p95 76.323 ms > 16.667 ms; run 3 head-backlit p95 116.366 ms > 16.667 ms; run 3 head-closeup p95 125.900 ms > 16.667 ms; run 3 population-dolly p95 31.439 ms > 16.667 ms; run 3 rapid-turn p95 33.091 ms > 16.667 ms. Counted runs: 3 of 3.

Host: editor-mcp / vulkan. Frame time = completed editor frame interval; sampling marshals may perturb it.
Internal 1920x1080, display 1920x1080 (native); upscaler resolved/fallback: {"fallback": "none", "resolved": "native"}.
Selected: lightingTap=None, msaaSamples=1, path=Deferred, rayTracedShadowsRequested=False, skinDiffusion=True, upscaleMode=0, upscaleTechnique=0, virtualShadowMaps=True; VSM selected: True; pinned by manifest: MSAASampleCount, Path, RayTracedShadowsEnabled, Upscale, UpscaleTechnique, VirtualShadowMaps.
Selected settings are what the renderer was set to, not proof each technique produced or consumed work.

### coats-closeup (frames/run 600)

| channel | p50 ms | p95 ms | p99 ms | max ms | gate misses | samples |
|---|---:|---:|---:|---:|---:|---:|
| frame time | 42.24–43.20 | 73.00–84.61 | 96.25–101.88 | 334.29–375.71 | 600 | 600 |
| CPU | 32.19–36.27 | 71.94–83.55 | 95.02–100.75 | 333.15–374.48 | 600 | 600 |
| GPU total (valid) | 37.01–38.11 | 51.29–61.16 | 72.56–78.47 | 95.83–106.56 | 599 | 599 |
| fence wait | 0.00 | 0.00 | 0.00 | 0.01 | 0 | 600 |
| present wait | 39.49–41.95 | 74.50–84.66 | 96.21–101.81 | 334.26–375.56 | 600 | 600 |
| recording wall | 0.64 | 0.71–0.72 | 0.75–0.79 | 0.91–0.98 | 0 | 600 |
| recording join wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 600 |

Excluded total-GPU samples (all counted runs): duplicate 3.

Groom deformation upload per frame, MiB (range over counted runs): p50 11.645, p95 11.851, max 22.231.

Top passes by p50 (valid samples only):

| pass | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---:|---:|---:|---:|---:|
| ShadowPass | 24.200–24.749 | 37.879–38.172 | 41.222–42.019 | 599 | 3 |
| ScenePass | 4.575–4.594 | 4.915–5.065 | 5.045–5.155 | 599 | 3 |
| GroomPass | 4.028–4.041 | 4.410–4.473 | 4.619–4.643 | 599 | 3 |
| DeferredLightingPass | 0.484–0.486 | 0.504–0.690 | 0.719–0.765 | 599 | 3 |
| GTAOPass | 0.426–0.427 | 0.429–0.430 | 0.431–0.703 | 599 | 3 |
| VirtualShadowMapMarkPass | 0.226–0.263 | 0.355–0.422 | 0.366–0.544 | 599 | 3 |
| FoliageCull | 0.174–0.175 | 0.402–0.404 | 0.417–0.421 | 599 | 3 |
| TAAPass | 0.152–0.154 | 0.156–0.158 | 0.157–0.159 | 599 | 3 |
| BloomPass | 0.083–0.084 | 0.084–0.085 | 0.085–0.086 | 599 | 3 |
| RayTracingScenePass | 0.081–0.082 | 0.347–0.348 | 0.563–0.572 | 599 | 3 |
| FXAAPass | 0.033–0.034 | 0.034 | 0.034–0.035 | 599 | 3 |
| SkinDiffusionPass | 0.032 | 0.033 | 0.033 | 599 | 3 |
| GroomSceneDepthSnapshotPass | 0.026 | 0.026–0.027 | 0.027 | 599 | 3 |
| DiffusionDepthSnapshotPass | 0.024 | 0.024–0.025 | 0.025–0.032 | 599 | 3 |
| SceneVelocitySeedPass | 0.022 | 0.023 | 0.024–0.027 | 599 | 3 |

Top sub-passes by p50 (valid samples only):

| sub-pass | parent | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---|---:|---:|---:|---:|---:|
| ShadowPass/VsmSun | ShadowPass | 15.581–15.672 | 16.159–16.224 | 16.600–17.122 | 599 | 3 |
| ShadowPass/VsmSun/FoliageCasters | ShadowPass | 8.557–8.656 | 9.020–9.052 | 9.108–9.183 | 599 | 3 |
| ShadowPass/VsmLocal | ShadowPass | 7.594–7.643 | 20.986–21.409 | 24.619–24.946 | 599 | 3 |
| ShadowPass/VsmLocal/FoliageCasters | ShadowPass | 3.862–4.025 | 6.882–6.972 | 7.342–7.481 | 599 | 3 |
| ShadowPass/VsmSun/GroomCasters | ShadowPass | 3.244–3.268 | 3.670–3.743 | 4.383–4.390 | 599 | 3 |
| ShadowPass/VsmSun/FoliageCull | ShadowPass | 2.694–2.697 | 3.144–3.485 | 3.387–3.666 | 599 | 3 |
| ShadowPass/VsmLocal/GroomCasters | ShadowPass | 2.565–2.609 | 13.173–13.308 | 16.538–17.156 | 599 | 3 |
| ScenePass/DepthPrepass | ScenePass | 2.376–2.381 | 2.635–2.671 | 2.662–2.744 | 599 | 3 |
| ScenePass/Color | ScenePass | 2.124–2.131 | 2.385–2.418 | 2.401–2.482 | 599 | 3 |
| GroomPass/StrandShade | GroomPass | 1.838–1.843 | 2.020–2.037 | 2.145–2.220 | 599 | 3 |

### head-backlit (frames/run 600)

| channel | p50 ms | p95 ms | p99 ms | max ms | gate misses | samples |
|---|---:|---:|---:|---:|---:|---:|
| frame time | 31.37–32.04 | 115.62–118.36 | 120.73–126.00 | 130.48–150.13 | 600 | 600 |
| CPU | 18.41–18.50 | 114.59–117.33 | 119.49–124.97 | 129.41–149.07 | 592–593 | 600 |
| GPU total (valid) | 31.36–32.04 | 94.94–97.89 | 100.61–106.38 | 111.12–128.43 | 599–600 | 599–600 |
| fence wait | 0.00 | 0.00 | 0.00 | 0.01 | 0 | 600 |
| present wait | 31.28–31.92 | 118.92–121.18 | 128.40–129.29 | 133.16–150.18 | 600 | 600 |
| recording wall | 0.19 | 0.20 | 0.21–0.26 | 0.27–0.34 | 0 | 600 |
| recording join wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 600 |

Excluded total-GPU samples (all counted runs): duplicate 1.

Groom deformation upload per frame, MiB (range over counted runs): p50 4.595, p95 4.696, max 4.696.

Top passes by p50 (valid samples only):

| pass | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---:|---:|---:|---:|---:|
| ShadowPass | 25.133–25.469 | 26.201–26.723 | 26.531–27.161 | 599–600 | 3 |
| GroomPass | 3.076–3.092 | 3.471–3.703 | 3.650–4.048 | 599–600 | 3 |
| ScenePass | 0.430–0.437 | 0.666–0.886 | 0.776–1.028 | 599–600 | 3 |
| DeferredLightingPass | 0.394–0.395 | 0.408–0.664 | 0.734–0.938 | 599–600 | 3 |
| GTAOPass | 0.364–0.365 | 0.366–0.367 | 0.367–0.391 | 599–600 | 3 |
| VirtualShadowMapMarkPass | 0.163–0.226 | 0.169–0.231 | 0.173–0.233 | 599–600 | 3 |
| FoliageCull | 0.170–0.171 | 0.412–0.414 | 0.636–0.800 | 599–600 | 3 |
| SkinDiffusionPass | 0.153 | 0.156 | 0.157–0.158 | 599–600 | 3 |
| TAAPass | 0.120 | 0.122–0.123 | 0.123–0.125 | 599–600 | 3 |
| RayTracingScenePass | 0.082–0.083 | 0.344–0.345 | 0.552–0.578 | 599–600 | 3 |
| BloomPass | 0.074–0.075 | 0.078 | 0.078–0.079 | 599–600 | 3 |
| FXAAPass | 0.025 | 0.026 | 0.026 | 599–600 | 3 |
| GroomSceneDepthSnapshotPass | 0.025 | 0.026 | 0.026–0.027 | 599–600 | 3 |
| DiffusionDepthSnapshotPass | 0.024 | 0.028–0.030 | 0.030–0.035 | 599–600 | 3 |
| SceneVelocitySeedPass | 0.017 | 0.018 | 0.018–0.019 | 599–600 | 3 |

Top sub-passes by p50 (valid samples only):

| sub-pass | parent | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---|---:|---:|---:|---:|---:|
| ShadowPass/VsmLocal | ShadowPass | 18.527–18.968 | 19.486–19.786 | 19.695–19.973 | 599–600 | 3 |
| ShadowPass/VsmLocal/GroomCasters | ShadowPass | 17.104–17.482 | 18.124–18.506 | 18.196–18.615 | 599–600 | 3 |
| ShadowPass/VsmSun | ShadowPass | 5.558–5.712 | 6.147–6.395 | 6.356–6.776 | 599–600 | 3 |
| ShadowPass/VsmSun/FoliageCull | ShadowPass | 2.658–2.663 | 3.106–3.368 | 3.436–3.786 | 599–600 | 3 |
| GroomPass/StrandShade | GroomPass | 1.902–1.911 | 2.236–2.311 | 2.313–2.468 | 599–600 | 3 |
| GroomPass/StrandPrepass | GroomPass | 1.176–1.183 | 1.475–1.619 | 1.536–1.928 | 599–600 | 3 |
| ShadowPass/VsmSun/GroomCasters | ShadowPass | 1.046–1.064 | 1.348–1.408 | 1.479–1.659 | 599–600 | 3 |
| ShadowPass/VsmLocal/FoliageCull | ShadowPass | 0.994–1.028 | 1.168–1.470 | 1.271–1.564 | 599–600 | 3 |
| ShadowPass/VsmSun/FoliageCasters | ShadowPass | 0.798–0.850 | 1.005–1.182 | 1.157–1.384 | 599–600 | 3 |
| ShadowPass/VsmSun/TerrainCasters | ShadowPass | 0.523–0.527 | 0.531–0.537 | 0.539–0.781 | 599–600 | 3 |

### head-closeup (frames/run 600)

| channel | p50 ms | p95 ms | p99 ms | max ms | gate misses | samples |
|---|---:|---:|---:|---:|---:|---:|
| frame time | 38.32–57.18 | 124.51–128.31 | 131.85–142.38 | 150.51–165.94 | 600 | 600 |
| CPU | 26.32–26.65 | 123.44–124.81 | 130.70–135.95 | 149.40–155.76 | 600 | 600 |
| GPU total (valid) | 38.44–39.71 | 103.88–109.07 | 112.10–125.01 | 132.12–139.25 | 599–600 | 599–600 |
| fence wait | 0.00 | 0.00 | 0.00 | 0.01 | 0 | 600 |
| present wait | 37.97–57.16 | 129.24–132.39 | 135.82–144.97 | 150.48–166.11 | 600 | 600 |
| recording wall | 0.92 | 1.02–1.04 | 1.14–1.33 | 1.39–1.66 | 0 | 600 |
| recording join wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 600 |

Excluded total-GPU samples (all counted runs): duplicate 1.

Groom deformation upload per frame, MiB (range over counted runs): p50 8.600, p95 8.600, max 8.600.

Top passes by p50 (valid samples only):

| pass | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---:|---:|---:|---:|---:|
| ShadowPass | 26.127–26.753 | 27.464–28.375 | 27.722–28.936 | 599–600 | 3 |
| GroomPass | 8.102–8.405 | 9.016–9.482 | 9.125–9.647 | 599–600 | 3 |
| ScenePass | 0.962–0.969 | 0.979–1.391 | 1.255–1.572 | 599–600 | 3 |
| DeferredLightingPass | 0.430–0.431 | 0.435–0.437 | 0.438–0.441 | 599–600 | 3 |
| GTAOPass | 0.421 | 0.423–0.424 | 0.425–0.436 | 599–600 | 3 |
| VirtualShadowMapMarkPass | 0.180–0.231 | 0.191–0.500 | 0.487–0.804 | 599–600 | 3 |
| FoliageCull | 0.177–0.178 | 0.396–0.403 | 0.408–0.629 | 599–600 | 3 |
| SkinDiffusionPass | 0.154 | 0.159 | 0.162–0.415 | 599–600 | 3 |
| TAAPass | 0.127–0.128 | 0.134–0.135 | 0.135–0.137 | 599–600 | 3 |
| RayTracingScenePass | 0.066–0.084 | 0.346–0.347 | 0.368–0.556 | 599–600 | 3 |
| BloomPass | 0.075–0.076 | 0.077 | 0.078–0.079 | 599–600 | 3 |
| FXAAPass | 0.027 | 0.027–0.028 | 0.028 | 599–600 | 3 |
| GroomSceneDepthSnapshotPass | 0.025 | 0.026 | 0.026 | 599–600 | 3 |
| DiffusionDepthSnapshotPass | 0.024 | 0.025–0.027 | 0.026–0.032 | 599–600 | 3 |
| DiffusionHandoffSnapshotPass | 0.017 | 0.017 | 0.017–0.018 | 599–600 | 3 |

Top sub-passes by p50 (valid samples only):

| sub-pass | parent | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---|---:|---:|---:|---:|---:|
| ShadowPass/VsmLocal | ShadowPass | 19.206–19.857 | 20.298–21.270 | 20.428–21.557 | 599–600 | 3 |
| ShadowPass/VsmLocal/GroomCasters | ShadowPass | 17.274–17.779 | 18.338–18.941 | 18.490–19.306 | 599–600 | 3 |
| ShadowPass/VsmSun | ShadowPass | 5.892–5.972 | 6.382–6.698 | 6.724–7.103 | 599–600 | 3 |
| GroomPass/StrandShade | GroomPass | 3.490–3.582 | 4.192–4.527 | 4.499–4.854 | 599–600 | 3 |
| ShadowPass/VsmSun/FoliageCull | ShadowPass | 2.660–2.684 | 3.100–3.328 | 3.447–3.604 | 599–600 | 3 |
| GroomPass/StrandPrepass | GroomPass | 2.410–2.422 | 2.761–3.161 | 2.778–3.363 | 599–600 | 3 |
| GroomPass/RootFrames | GroomPass | 1.921–1.927 | 2.255–2.444 | 2.284–2.714 | 599–600 | 3 |
| ShadowPass/VsmLocal/FoliageCull | ShadowPass | 1.442–1.640 | 1.668–2.110 | 1.862–2.374 | 599–600 | 3 |
| GroomPass/RootFrames/Dispatch | GroomPass | 1.555–1.559 | 1.845–1.958 | 1.898–2.261 | 599–600 | 3 |
| ShadowPass/VsmSun/GroomCasters | ShadowPass | 1.330–1.333 | 1.688–1.753 | 1.732–1.965 | 599–600 | 3 |

### population-dolly (frames/run 600)

| channel | p50 ms | p95 ms | p99 ms | max ms | gate misses | samples |
|---|---:|---:|---:|---:|---:|---:|
| frame time | 29.25–29.45 | 31.44–31.56 | 69.67–70.47 | 283.36–295.14 | 600 | 600 |
| CPU | 17.20–17.52 | 20.08–20.53 | 68.30–69.30 | 282.27–294.01 | 389–492 | 600 |
| GPU total (valid) | 29.24–29.41 | 31.36–31.49 | 42.97–44.41 | 116.83–120.95 | 599 | 599 |
| fence wait | 0.00 | 0.00 | 0.00 | 0.01 | 0 | 600 |
| present wait | 29.27–29.46 | 32.24–32.74 | 78.81–80.11 | 294.43–306.22 | 600 | 600 |
| recording wall | 0.53–0.54 | 0.62–0.63 | 0.68–0.78 | 0.79–0.94 | 0 | 600 |
| recording join wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 600 |

Excluded total-GPU samples (all counted runs): duplicate 3.

Groom deformation upload per frame, MiB (range over counted runs): p50 4.465, p95 5.028, max 24.880.

Top passes by p50 (valid samples only):

| pass | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---:|---:|---:|---:|---:|
| ShadowPass | 16.595–16.777 | 17.752–17.808 | 19.211–19.731 | 599 | 3 |
| ScenePass | 6.173–6.350 | 6.623–6.791 | 6.816–6.914 | 599 | 3 |
| GroomPass | 4.063–4.127 | 5.598–5.644 | 6.343–6.700 | 599 | 3 |
| DeferredLightingPass | 0.362 | 0.378–0.379 | 0.599–0.681 | 599 | 3 |
| GTAOPass | 0.354–0.357 | 0.494–0.499 | 0.561–0.761 | 599 | 3 |
| FoliageCull | 0.187–0.201 | 0.409–0.430 | 0.433–0.669 | 599 | 3 |
| TAAPass | 0.136–0.137 | 0.140–0.141 | 0.142–0.143 | 599 | 3 |
| RayTracingScenePass | 0.083–0.084 | 0.350–0.352 | 0.589–0.613 | 599 | 3 |
| VirtualShadowMapMarkPass | 0.078–0.083 | 0.090–0.101 | 0.109–0.128 | 599 | 3 |
| BloomPass | 0.079–0.080 | 0.081–0.082 | 0.084–0.085 | 599 | 3 |
| SkinDiffusionPass | 0.032 | 0.033 | 0.033 | 599 | 3 |
| FXAAPass | 0.031 | 0.032 | 0.032 | 599 | 3 |
| GroomSceneDepthSnapshotPass | 0.025 | 0.026 | 0.027–0.052 | 599 | 3 |
| DiffusionDepthSnapshotPass | 0.024 | 0.024–0.025 | 0.025–0.026 | 599 | 3 |
| SceneVelocitySeedPass | 0.018–0.019 | 0.020–0.022 | 0.023–0.024 | 599 | 3 |

Top sub-passes by p50 (valid samples only):

| sub-pass | parent | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---|---:|---:|---:|---:|---:|
| ShadowPass/VsmSun | ShadowPass | 15.403–15.513 | 16.496–16.562 | 17.928–18.423 | 599 | 3 |
| ShadowPass/VsmSun/FoliageCasters | ShadowPass | 9.593–9.699 | 10.668–10.726 | 10.912–10.949 | 599 | 3 |
| ShadowPass/VsmSun/FoliageCull | ShadowPass | 2.757–3.111 | 3.218–3.550 | 3.565–3.810 | 599 | 3 |
| ScenePass/Color | ScenePass | 3.007–3.094 | 3.297–3.522 | 3.360–3.568 | 599 | 3 |
| ScenePass/DepthPrepass | ScenePass | 3.082–3.092 | 3.373–3.464 | 3.436–3.628 | 599 | 3 |
| ShadowPass/VsmSun/GroomCasters | ShadowPass | 1.401–1.412 | 2.984–3.177 | 4.802–5.084 | 599 | 3 |
| GroomPass/RootFrames | GroomPass | 1.316–1.336 | 1.656–1.689 | 1.783–1.900 | 599 | 3 |
| GroomPass/StrandShade | GroomPass | 1.313–1.330 | 2.154–2.214 | 2.549–2.683 | 599 | 3 |
| GroomPass/StrandPrepass | GroomPass | 1.164–1.172 | 1.853–1.913 | 2.185–2.207 | 599 | 3 |
| GroomPass/RootFrames/Dispatch | GroomPass | 1.036–1.074 | 1.368–1.389 | 1.506 | 599 | 3 |

### rapid-turn (frames/run 599–600)

| channel | p50 ms | p95 ms | p99 ms | max ms | gate misses | samples |
|---|---:|---:|---:|---:|---:|---:|
| frame time | 26.36–26.48 | 32.46–33.19 | 48.03–50.56 | 77.32–81.35 | 592–593 | 599–600 |
| CPU | 12.63–12.78 | 17.09–18.13 | 46.95–49.51 | 76.22–80.12 | 52–70 | 599–600 |
| GPU total (valid) | 26.33–26.44 | 32.29–33.02 | 32.81–33.42 | 50.73–56.15 | 599–600 | 599–600 |
| fence wait | 0.00 | 0.00 | 0.01 | 0.01 | 0 | 599–600 |
| present wait | 26.31–26.50 | 32.89–33.82 | 60.80–62.41 | 91.22–93.36 | 584–585 | 599–600 |
| recording wall | 0.41 | 0.56–0.57 | 0.59–0.63 | 0.64–0.80 | 0 | 599–600 |
| recording join wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 599–600 |

Groom deformation upload per frame, MiB (range over counted runs): p50 2.003, p95 4.293, max 7.664.

Top passes by p50 (valid samples only):

| pass | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---:|---:|---:|---:|---:|
| ShadowPass | 17.386–17.462 | 19.665–20.362 | 20.166–20.747 | 599–600 | 3 |
| ScenePass | 5.527–5.577 | 6.903–7.029 | 7.150–7.271 | 599–600 | 3 |
| GroomPass | 1.458–1.481 | 3.806–4.047 | 4.092–4.199 | 599–600 | 3 |
| GTAOPass | 0.304 | 0.461–0.463 | 0.662–0.672 | 599–600 | 3 |
| DeferredLightingPass | 0.296–0.297 | 0.376–0.379 | 0.608–0.641 | 599–600 | 3 |
| FoliageCull | 0.175–0.182 | 0.410–0.413 | 0.436–0.635 | 599–600 | 3 |
| TAAPass | 0.126–0.127 | 0.133–0.134 | 0.137 | 599–600 | 3 |
| RayTracingScenePass | 0.084–0.085 | 0.352–0.454 | 0.570–0.589 | 599–600 | 3 |
| BloomPass | 0.079 | 0.081–0.082 | 0.084–0.341 | 599–600 | 3 |
| VirtualShadowMapMarkPass | 0.049–0.054 | 0.072–0.079 | 0.078–0.083 | 599–600 | 3 |
| SkinDiffusionPass | 0.032 | 0.033 | 0.033 | 599–600 | 3 |
| FXAAPass | 0.028 | 0.030 | 0.031 | 599–600 | 3 |
| GroomSceneDepthSnapshotPass | 0.025 | 0.026 | 0.026–0.040 | 599–600 | 3 |
| DiffusionDepthSnapshotPass | 0.024 | 0.025–0.027 | 0.029–0.033 | 599–600 | 3 |
| SceneVelocitySeedPass | 0.017 | 0.019 | 0.020–0.022 | 599–600 | 3 |

Top sub-passes by p50 (valid samples only):

| sub-pass | parent | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---|---:|---:|---:|---:|---:|
| ShadowPass/VsmSun | ShadowPass | 15.165–15.278 | 15.664–16.042 | 15.804–16.247 | 599–600 | 3 |
| ShadowPass/VsmSun/FoliageCasters | ShadowPass | 10.404–10.524 | 10.784–10.999 | 10.881–11.270 | 599–600 | 3 |
| ShadowPass/VsmSun/FoliageCull | ShadowPass | 2.739–3.102 | 3.225–3.559 | 3.519–3.826 | 599–600 | 3 |
| ScenePass/DepthPrepass | ScenePass | 2.773–2.797 | 3.505–3.570 | 3.726–3.752 | 599–600 | 3 |
| ScenePass/Color | ScenePass | 2.668–2.682 | 3.417–3.470 | 3.547–3.622 | 599–600 | 3 |
| ShadowPass/VsmLocal | ShadowPass | 1.210–1.233 | 2.650–3.658 | 2.763–3.845 | 599–600 | 3 |
| ShadowPass/VsmSun/TerrainCasters | ShadowPass | 0.880–0.891 | 0.897–1.000 | 0.904–1.082 | 599–600 | 3 |
| ShadowPass/VsmLocal/FoliageCull | ShadowPass | 0.812–0.813 | 1.075–1.105 | 1.157–1.175 | 406 | 3 |
| GroomPass/StrandShade | GroomPass | 0.751–0.753 | 1.240–1.253 | 1.454–1.577 | 599–600 | 3 |
| ShadowPass/VsmSun/GroomCasters | ShadowPass | 0.720–0.725 | 0.929–1.002 | 1.162–1.184 | 599–600 | 3 |

Excluded per-pass samples (all counted runs): duplicate-frame-pass 428.

Memory: peak GPU resident 3220.1 MiB, live GPU 2759.1 MiB, peak CPU tracked 82.6 MiB, GPU after scene release n/a.
Report: GPU peak 3938.4 MiB / live 2754.7 MiB; CPU peak 82.6 MiB / live 82.6 MiB; reconciliation reconciled; residency osReported.
Top GPU owners (first counted run): ShadowPass 691.1 MiB, ShadowMap 640.0 MiB, Unattributed 414.1 MiB, GroomPass 274.3 MiB, TransientPool 247.4 MiB, Foliage pinned representations 139.2 MiB, RendererInit 131.8 MiB, GBuffer 101.2 MiB, TemporalHistory 38.4 MiB, Texture2D 32.0 MiB.

Not counted:

- `vk-deferred/round-02-attempt-2` failed: process failed: ValueError: editor capture incomplete: {'attachmentFailures': 0, 'backend': 'vulkan', 'cameras': [{'attachments': [{'file': 'Beauty.png', 'height': 1080, 'name': 'Beauty', 'width': 1920}, {'file': 'SceneColorHDR.hdr', 'height': 1080, 'name': 'SceneColorHDR', 'width': 1920}, {'file': 'Albedo.png', 'height': 1080, 'name': 'Albedo', 'width': 1920}, {'file': 'Normals.png', 'height': 1080, 'name': 'Normals', 'width': 1920}, {'file': 'Velocity.png', 'height': 1080, 'name': 'Velocity', 'width': 1920}, {'file': 'SceneDepth.hdr', 'height': 1080, 'name': 'SceneDepth', 'width': 1920}], 'id': 'head-closeup'}, {'attachments': [{'file': 'Beauty.png', 'height': 1080, 'name': 'Beauty', 'width': 1920}, {'file': 'SceneColorHDR.hdr', 'height': 1080, 'name': 'SceneColorHDR', 'width': 1920}, {'file': 'Albedo.png', 'height': 1080, 'name': 'Albedo', 'width': 1920}, {'file': 'Normals.png', 'height': 1080, 'name': 'Normals', 'width': 1920}, {'file': 'Velocity.png', 'height': 1080, 'name': 'Velocity', 'width': 1920}, {'file': 'SceneDepth.hdr', 'height': 1080, 'name': 'SceneDepth', 'width': 1920}], 'id': 'head-backlit'}, {'attachments': [{'file': 'Beauty.png', 'height': 1080, 'name': 'Beauty', 'width': 1920}, {'file': 'SceneColorHDR.hdr', 'height': 1080, 'name': 'SceneColorHDR', 'width': 1920}, {'file': 'Albedo.png', 'height': 1080, 'name': 'Albedo', 'width': 1920}, {'file': 'Normals.png', 'height': 1080, 'name': 'Normals', 'width': 1920}, {'file': 'Velocity.png', 'height': 1080, 'name': 'Velocity', 'width': 1920}, {'file': 'SceneDepth.hdr', 'height': 1080, 'name': 'SceneDepth', 'width': 1920}], 'id': 'coats-closeup'}, {'attachments': [{'file': 'Beauty.png', 'height': 1080, 'name': 'Beauty', 'width': 1920}, {'file': 'SceneColorHDR.hdr', 'height': 1080, 'name': 'SceneColorHDR', 'width': 1920}, {'file': 'Albedo.png', 'height': 1080, 'name': 'Albedo', 'width': 1920}, {'file': 'Normals.png', 'height': 1080, 'name': 'Normals', 'width': 1920}, {'file': 'Velocity.png', 'height': 1080, 'name': 'Velocity', 'width': 1920}, {'file': 'SceneDepth.hdr', 'height': 1080, 'name': 'SceneDepth', 'width': 1920}], 'id': 'population-dolly'}, {'attachments': [{'file': 'Beauty.png', 'height': 1080, 'name': 'Beauty', 'width': 1920}, {'file': 'SceneColorHDR.hdr', 'height': 1080, 'name': 'SceneColorHDR', 'width': 1920}, {'file': 'Albedo.png', 'height': 1080, 'name': 'Albedo', 'width': 1920}, {'file': 'Normals.png', 'height': 1080, 'name': 'Normals', 'width': 1920}, {'file': 'Velocity.png', 'height': 1080, 'name': 'Velocity', 'width': 1920}, {'file': 'SceneDepth.hdr', 'height': 1080, 'name': 'SceneDepth', 'width': 1920}], 'id': 'rapid-turn'}], 'determinismNote': "editor-mcp host: the scene clock is pinned to the manifest's StartTimeSeconds + n * FixedDtSeconds like the test binary's, but frame pacing, quality tiering and TAA history stay the live editor's — the deterministic run-twice product is the test binary's --olo-capture-manifest front door (docs/guides/renderer-benchmarks.md)", 'host': 'editor-mcp', 'id': 'acceptance-deferred', 'outDir': 'C:/repos/OloEngine-integrated-renderer-release-baseline-1799/build-cached/scratch/b1799/campaign2/vk-deferred/round-02-attempt-2', 'warmupTimedOut': True}

## vk-hybrid: FAIL

run 1 coats-closeup p95 126.292 ms > 16.667 ms; run 1 head-backlit p95 144.833 ms > 16.667 ms; run 1 head-closeup p95 204.372 ms > 16.667 ms; run 1 population-dolly p95 138.941 ms > 16.667 ms; run 1 rapid-turn p95 124.387 ms > 16.667 ms; run 2 coats-closeup p95 136.431 ms > 16.667 ms; run 2 head-backlit p95 143.996 ms > 16.667 ms; run 2 head-closeup p95 203.999 ms > 16.667 ms; run 2 population-dolly p95 107.704 ms > 16.667 ms; run 2 rapid-turn p95 95.284 ms > 16.667 ms; run 3 coats-closeup p95 128.562 ms > 16.667 ms; run 3 head-backlit p95 144.380 ms > 16.667 ms; run 3 head-closeup p95 208.190 ms > 16.667 ms; run 3 population-dolly p95 105.432 ms > 16.667 ms; run 3 rapid-turn p95 96.343 ms > 16.667 ms. Counted runs: 3 of 3.

Host: editor-mcp / vulkan. Frame time = completed editor frame interval; sampling marshals may perturb it.
Internal 1920x1080, display 1920x1080 (native); upscaler resolved/fallback: {"fallback": "none", "resolved": "native"}.
Selected: lightingTap=None, msaaSamples=1, path=Deferred, rayTracedShadowsRequested=True, skinDiffusion=True, upscaleMode=0, upscaleTechnique=0, virtualShadowMaps=True; VSM selected: True; pinned by manifest: MSAASampleCount, Path, RayTracedShadowsEnabled, Upscale, UpscaleTechnique, VirtualShadowMaps.
Selected settings are what the renderer was set to, not proof each technique produced or consumed work.

### coats-closeup (frames/run 600)

| channel | p50 ms | p95 ms | p99 ms | max ms | gate misses | samples |
|---|---:|---:|---:|---:|---:|---:|
| frame time | 88.28–100.66 | 126.29–136.43 | 146.59–156.35 | 408.48–418.51 | 600 | 600 |
| CPU | 85.41–91.98 | 124.89–135.09 | 145.38–155.12 | 407.34–417.27 | 600 | 600 |
| GPU total (valid) | 38.97–39.46 | 60.86–72.10 | 73.72–82.01 | 96.54–103.77 | 598–599 | 598–599 |
| fence wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 600 |
| present wait | 89.09–100.85 | 126.26–136.66 | 146.59–156.24 | 408.54–418.53 | 600 | 600 |
| recording wall | 0.64 | 0.70–0.73 | 0.75–0.91 | 0.98–1.27 | 0 | 600 |
| recording join wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 600 |

Excluded total-GPU samples (all counted runs): duplicate 4.

Groom deformation upload per frame, MiB (range over counted runs): p50 11.645, p95 11.851, max 22.231.

Top passes by p50 (valid samples only):

| pass | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---:|---:|---:|---:|---:|
| ShadowPass | 23.932–25.841 | 36.713–37.764 | 40.144–40.973 | 598–599 | 3 |
| ScenePass | 4.574–4.831 | 5.199–5.252 | 5.425–5.584 | 598–599 | 3 |
| GroomPass | 4.068–4.095 | 4.517–4.776 | 4.775–5.144 | 598–599 | 3 |
| RayTracingScenePass | 1.422–1.438 | 2.016–2.087 | 2.137–2.229 | 598–599 | 3 |
| DeferredLightingPass | 0.492–0.494 | 0.524–0.699 | 0.557–0.819 | 598–599 | 3 |
| GTAOPass | 0.425–0.426 | 0.458–0.483 | 0.473–0.717 | 598–599 | 3 |
| VirtualShadowMapMarkPass | 0.230–0.237 | 0.333–0.397 | 0.341–0.590 | 598–599 | 3 |
| FoliageCull | 0.174–0.175 | 0.184–0.190 | 0.187–0.191 | 598–599 | 3 |
| TAAPass | 0.153 | 0.157–0.161 | 0.161–0.163 | 598–599 | 3 |
| BloomPass | 0.084 | 0.089–0.092 | 0.091–0.302 | 598–599 | 3 |
| FXAAPass | 0.033–0.034 | 0.036–0.038 | 0.037–0.039 | 598–599 | 3 |
| SkinDiffusionPass | 0.032 | 0.034–0.036 | 0.035–0.037 | 598–599 | 3 |
| GroomSceneDepthSnapshotPass | 0.025 | 0.027–0.028 | 0.027–0.028 | 598–599 | 3 |
| DiffusionDepthSnapshotPass | 0.024 | 0.026 | 0.027–0.030 | 598–599 | 3 |
| SceneVelocitySeedPass | 0.022–0.023 | 0.024 | 0.024–0.026 | 598–599 | 3 |

Top sub-passes by p50 (valid samples only):

| sub-pass | parent | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---|---:|---:|---:|---:|---:|
| ShadowPass/VsmSun | ShadowPass | 15.263–15.675 | 16.694–17.285 | 17.278–17.696 | 598–599 | 3 |
| ShadowPass/VsmSun/FoliageCasters | ShadowPass | 8.418–8.743 | 9.231–9.692 | 9.673–10.077 | 598–599 | 3 |
| ShadowPass/VsmLocal | ShadowPass | 7.464–8.444 | 20.780–21.293 | 24.049–24.858 | 598–599 | 3 |
| ShadowPass/VsmLocal/FoliageCasters | ShadowPass | 3.856–4.244 | 6.880–6.983 | 7.246–7.817 | 598–599 | 3 |
| ShadowPass/VsmSun/GroomCasters | ShadowPass | 3.228–3.317 | 3.529–3.935 | 4.455–4.802 | 598–599 | 3 |
| ShadowPass/VsmLocal/GroomCasters | ShadowPass | 2.629–2.756 | 13.233–13.407 | 15.731–16.687 | 598–599 | 3 |
| ShadowPass/VsmSun/FoliageCull | ShadowPass | 2.684–2.690 | 3.033–3.156 | 3.157–3.351 | 598–599 | 3 |
| ScenePass/DepthPrepass | ScenePass | 2.375–2.389 | 2.744–2.820 | 2.944–3.039 | 598–599 | 3 |
| ScenePass/Color | ScenePass | 2.121–2.135 | 2.430–2.511 | 2.468–2.679 | 598–599 | 3 |
| GroomPass/StrandShade | GroomPass | 1.842–1.863 | 2.029–2.168 | 2.247–2.378 | 598–599 | 3 |

### head-backlit (frames/run 600)

| channel | p50 ms | p95 ms | p99 ms | max ms | gate misses | samples |
|---|---:|---:|---:|---:|---:|---:|
| frame time | 46.72–52.64 | 144.00–144.83 | 153.17–156.35 | 162.27–166.37 | 600 | 600 |
| CPU | 45.10–45.81 | 140.07–143.32 | 151.00–153.80 | 159.63–163.40 | 600 | 600 |
| GPU total (valid) | 32.61–32.95 | 106.97–108.47 | 115.48–117.81 | 125.80–128.22 | 600 | 600 |
| fence wait | 0.00 | 0.00 | 0.00 | 0.01 | 0 | 600 |
| present wait | 46.71–52.64 | 143.94–144.71 | 153.11–156.19 | 162.19–166.30 | 600 | 600 |
| recording wall | 0.18 | 0.20 | 0.21–0.24 | 0.26–0.37 | 0 | 600 |
| recording join wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 600 |

Groom deformation upload per frame, MiB (range over counted runs): p50 4.595, p95 4.696, max 4.696.

Top passes by p50 (valid samples only):

| pass | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---:|---:|---:|---:|---:|
| ShadowPass | 24.948–25.453 | 27.040–29.969 | 28.124–33.962 | 600 | 3 |
| GroomPass | 3.086–3.105 | 3.409–3.800 | 3.558–4.135 | 600 | 3 |
| RayTracingScenePass | 1.384–1.406 | 2.026–2.096 | 2.381–2.393 | 600 | 3 |
| ScenePass | 0.428–0.431 | 0.703–0.762 | 0.776–0.865 | 600 | 3 |
| DeferredLightingPass | 0.396–0.397 | 0.476–0.666 | 0.524–0.703 | 600 | 3 |
| GTAOPass | 0.365 | 0.399–0.438 | 0.418–0.465 | 600 | 3 |
| VirtualShadowMapMarkPass | 0.169–0.232 | 0.177–0.256 | 0.185–0.264 | 600 | 3 |
| FoliageCull | 0.171–0.172 | 0.183–0.195 | 0.191–0.213 | 600 | 3 |
| SkinDiffusionPass | 0.153 | 0.173–0.186 | 0.183–0.197 | 600 | 3 |
| TAAPass | 0.121 | 0.125–0.131 | 0.128–0.175 | 600 | 3 |
| BloomPass | 0.075 | 0.082–0.088 | 0.085–0.096 | 600 | 3 |
| FXAAPass | 0.025 | 0.028–0.030 | 0.029–0.031 | 600 | 3 |
| GroomSceneDepthSnapshotPass | 0.025 | 0.028–0.029 | 0.030–0.223 | 600 | 3 |
| DiffusionDepthSnapshotPass | 0.024 | 0.029–0.030 | 0.031–0.036 | 600 | 3 |
| DiffusionHandoffSnapshotPass | 0.016–0.017 | 0.018–0.020 | 0.019–0.022 | 600 | 3 |

Top sub-passes by p50 (valid samples only):

| sub-pass | parent | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---|---:|---:|---:|---:|---:|
| ShadowPass/VsmLocal | ShadowPass | 18.835–19.159 | 20.355–22.297 | 20.763–25.939 | 600 | 3 |
| ShadowPass/VsmLocal/GroomCasters | ShadowPass | 17.593–17.799 | 18.964–20.974 | 19.225–24.525 | 600 | 3 |
| ShadowPass/VsmSun | ShadowPass | 5.391–5.502 | 5.996–6.439 | 6.392–7.057 | 600 | 3 |
| ShadowPass/VsmSun/FoliageCull | ShadowPass | 2.671–2.673 | 3.022–3.143 | 3.113–3.445 | 600 | 3 |
| GroomPass/StrandShade | GroomPass | 1.909–1.919 | 2.164–2.180 | 2.222–2.470 | 600 | 3 |
| GroomPass/StrandPrepass | GroomPass | 1.157–1.179 | 1.419–1.579 | 1.533–1.684 | 600 | 3 |
| ShadowPass/VsmSun/GroomCasters | ShadowPass | 0.978–0.999 | 1.186–1.273 | 1.359–1.723 | 600 | 3 |
| ShadowPass/VsmLocal/FoliageCull | ShadowPass | 0.665–0.992 | 1.083–1.109 | 1.157–1.254 | 600 | 3 |
| RayTracingScenePass/AccelerationStructureBuild | RayTracingScenePass | 0.891–0.934 | 1.556–1.613 | 1.827–1.871 | 600 | 3 |
| ShadowPass/VsmSun/FoliageCasters | ShadowPass | 0.805–0.822 | 0.925–0.963 | 1.052–1.172 | 600 | 3 |

### head-closeup (frames/run 600)

| channel | p50 ms | p95 ms | p99 ms | max ms | gate misses | samples |
|---|---:|---:|---:|---:|---:|---:|
| frame time | 107.20–109.45 | 204.00–208.19 | 216.85–227.66 | 234.94–291.97 | 600 | 600 |
| CPU | 105.90–108.22 | 202.83–207.05 | 215.66–225.54 | 233.31–289.44 | 600 | 600 |
| GPU total (valid) | 41.05–41.91 | 116.29–119.12 | 123.31–127.36 | 136.51–147.38 | 600 | 600 |
| fence wait | 0.00 | 0.00 | 0.00 | 0.01–0.02 | 0 | 600 |
| present wait | 107.17–109.50 | 203.93–208.16 | 216.79–227.79 | 234.46–291.40 | 600 | 600 |
| recording wall | 0.92–0.94 | 1.01–1.06 | 1.08–1.39 | 1.39–2.17 | 0 | 600 |
| recording join wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 600 |

Groom deformation upload per frame, MiB (range over counted runs): p50 8.600, p95 8.600, max 8.600.

Top passes by p50 (valid samples only):

| pass | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---:|---:|---:|---:|---:|
| ShadowPass | 26.577–27.314 | 34.536–35.538 | 35.043–36.149 | 600 | 3 |
| GroomPass | 8.669–8.916 | 10.345–10.791 | 11.167–12.606 | 600 | 3 |
| RayTracingScenePass | 1.887–1.934 | 2.710–2.829 | 3.308–3.440 | 600 | 3 |
| ScenePass | 0.991–1.048 | 1.235–1.336 | 1.307–1.578 | 600 | 3 |
| DeferredLightingPass | 0.443–0.457 | 0.556–0.559 | 0.587–0.654 | 600 | 3 |
| GTAOPass | 0.431–0.449 | 0.536–0.548 | 0.660–0.723 | 600 | 3 |
| VirtualShadowMapMarkPass | 0.196–0.276 | 0.224–0.319 | 0.236–0.503 | 600 | 3 |
| FoliageCull | 0.182–0.186 | 0.228–0.230 | 0.233–0.242 | 600 | 3 |
| SkinDiffusionPass | 0.161–0.164 | 0.195–0.197 | 0.208–0.217 | 600 | 3 |
| TAAPass | 0.134–0.135 | 0.187–0.193 | 0.194–0.202 | 600 | 3 |
| BloomPass | 0.078–0.079 | 0.098–0.099 | 0.101–0.106 | 600 | 3 |
| FXAAPass | 0.027–0.028 | 0.032–0.033 | 0.034–0.039 | 600 | 3 |
| GroomSceneDepthSnapshotPass | 0.026–0.027 | 0.031–0.032 | 0.033–0.034 | 600 | 3 |
| DiffusionDepthSnapshotPass | 0.025 | 0.034–0.035 | 0.039–0.040 | 600 | 3 |
| DiffusionHandoffSnapshotPass | 0.017–0.018 | 0.023 | 0.024–0.026 | 600 | 3 |

Top sub-passes by p50 (valid samples only):

| sub-pass | parent | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---|---:|---:|---:|---:|---:|
| ShadowPass/VsmLocal | ShadowPass | 19.702–20.389 | 25.813–26.825 | 26.383–27.599 | 600 | 3 |
| ShadowPass/VsmLocal/GroomCasters | ShadowPass | 17.642–18.354 | 23.493–24.471 | 24.105–25.337 | 600 | 3 |
| ShadowPass/VsmSun | ShadowPass | 5.868–6.052 | 7.469–7.519 | 7.580–7.853 | 600 | 3 |
| GroomPass/StrandShade | GroomPass | 3.902–4.065 | 4.665–4.850 | 4.999–6.211 | 600 | 3 |
| ShadowPass/VsmSun/FoliageCull | ShadowPass | 2.723–2.801 | 3.191–3.194 | 3.261–3.384 | 600 | 3 |
| GroomPass/StrandPrepass | GroomPass | 2.537–2.653 | 3.285–3.348 | 3.623–3.724 | 600 | 3 |
| GroomPass/RootFrames | GroomPass | 1.985–2.043 | 2.403–2.461 | 2.518–2.660 | 600 | 3 |
| GroomPass/RootFrames/Dispatch | GroomPass | 1.609–1.651 | 1.976–2.035 | 2.092–2.239 | 600 | 3 |
| ShadowPass/VsmLocal/FoliageCull | ShadowPass | 1.485–1.532 | 1.751–1.884 | 1.882–1.991 | 600 | 3 |
| ShadowPass/VsmSun/GroomCasters | ShadowPass | 1.348–1.392 | 2.125–2.178 | 2.165–2.249 | 600 | 3 |

### population-dolly (frames/run 600)

| channel | p50 ms | p95 ms | p99 ms | max ms | gate misses | samples |
|---|---:|---:|---:|---:|---:|---:|
| frame time | 95.61–97.02 | 105.43–138.94 | 158.56–164.63 | 404.46–435.58 | 600 | 600 |
| CPU | 94.24–95.13 | 103.73–106.09 | 157.36–159.57 | 403.17–434.17 | 600 | 600 |
| GPU total (valid) | 34.40–35.00 | 39.34–55.35 | 50.57–70.32 | 133.77–138.08 | 592–599 | 592–599 |
| fence wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 600 |
| present wait | 95.62–96.99 | 105.60–139.28 | 158.77–165.02 | 404.64–435.78 | 600 | 600 |
| recording wall | 0.53 | 0.60–0.63 | 0.69–0.74 | 0.83–0.96 | 0 | 600 |
| recording join wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 600 |

Excluded total-GPU samples (all counted runs): duplicate 10.

Groom deformation upload per frame, MiB (range over counted runs): p50 4.465, p95 5.028, max 24.880.

Top passes by p50 (valid samples only):

| pass | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---:|---:|---:|---:|---:|
| ShadowPass | 18.005–18.208 | 20.523–28.579 | 21.700–30.356 | 592–599 | 3 |
| ScenePass | 7.236–7.355 | 8.059–10.691 | 8.345–11.604 | 592–599 | 3 |
| GroomPass | 4.533–4.882 | 6.566–9.306 | 7.069–10.597 | 592–599 | 3 |
| RayTracingScenePass | 1.799–1.828 | 2.438–3.276 | 2.854–4.260 | 592–599 | 3 |
| GTAOPass | 0.413–0.449 | 0.560–0.640 | 0.774–0.910 | 592–599 | 3 |
| DeferredLightingPass | 0.409–0.418 | 0.470–0.813 | 0.687–1.101 | 592–599 | 3 |
| FoliageCull | 0.205–0.207 | 0.245–0.274 | 0.247–0.341 | 592–599 | 3 |
| TAAPass | 0.145–0.146 | 0.210–0.221 | 0.217–0.369 | 592–599 | 3 |
| VirtualShadowMapMarkPass | 0.088–0.093 | 0.108–0.110 | 0.139–0.148 | 592–599 | 3 |
| BloomPass | 0.088–0.090 | 0.109–0.131 | 0.111–0.296 | 592–599 | 3 |
| SkinDiffusionPass | 0.036–0.037 | 0.039–0.053 | 0.041–0.055 | 592–599 | 3 |
| FXAAPass | 0.035–0.036 | 0.039–0.051 | 0.040–0.054 | 592–599 | 3 |
| GroomSceneDepthSnapshotPass | 0.028 | 0.032–0.034 | 0.034–0.035 | 592–599 | 3 |
| DiffusionDepthSnapshotPass | 0.026–0.027 | 0.033–0.036 | 0.034–0.044 | 592–599 | 3 |
| SceneVelocitySeedPass | 0.020 | 0.030–0.037 | 0.033–0.153 | 592–599 | 3 |

Top sub-passes by p50 (valid samples only):

| sub-pass | parent | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---|---:|---:|---:|---:|---:|
| ShadowPass/VsmSun | ShadowPass | 16.879–17.034 | 19.271–27.039 | 20.489–28.700 | 592–599 | 3 |
| ShadowPass/VsmSun/FoliageCasters | ShadowPass | 11.083–11.307 | 12.345–16.115 | 13.030–17.009 | 592–599 | 3 |
| ScenePass/Color | ScenePass | 3.528–3.659 | 3.954–5.175 | 4.046–5.701 | 592–599 | 3 |
| ScenePass/DepthPrepass | ScenePass | 3.550–3.630 | 4.152–5.374 | 4.339–5.868 | 592–599 | 3 |
| ShadowPass/VsmSun/FoliageCull | ShadowPass | 3.064–3.135 | 3.486–4.151 | 3.710–5.047 | 592–599 | 3 |
| ShadowPass/VsmSun/GroomCasters | ShadowPass | 1.391–1.658 | 3.562–5.306 | 5.449–9.629 | 592–599 | 3 |
| GroomPass/StrandShade | GroomPass | 1.536–1.608 | 2.477–3.437 | 2.863–4.159 | 592–599 | 3 |
| GroomPass/StrandPrepass | GroomPass | 1.360–1.572 | 2.111–3.115 | 2.395–4.193 | 592–599 | 3 |
| GroomPass/RootFrames | GroomPass | 1.476–1.511 | 1.721–2.473 | 1.780–2.931 | 592–599 | 3 |
| GroomPass/RootFrames/Dispatch | GroomPass | 1.197–1.232 | 1.404–2.082 | 1.456–2.545 | 592–599 | 3 |

### rapid-turn (frames/run 599–600)

| channel | p50 ms | p95 ms | p99 ms | max ms | gate misses | samples |
|---|---:|---:|---:|---:|---:|---:|
| frame time | 63.32–82.69 | 95.28–124.39 | 112.40–143.81 | 171.39–288.18 | 599–600 | 599–600 |
| CPU | 62.07–63.80 | 94.09–99.24 | 111.18–117.24 | 165.01–174.71 | 599–600 | 599–600 |
| GPU total (valid) | 26.67–36.93 | 33.99–64.75 | 37.71–67.25 | 56.74–220.81 | 594–599 | 594–599 |
| fence wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 599–600 |
| present wait | 63.15–83.28 | 95.37–124.61 | 112.58–143.90 | 166.86–287.64 | 599–600 | 599–600 |
| recording wall | 0.40–0.42 | 0.56–0.60 | 0.61–0.67 | 0.72–0.86 | 0 | 599–600 |
| recording join wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 599–600 |

Excluded total-GPU samples (all counted runs): duplicate 6.

Groom deformation upload per frame, MiB (range over counted runs): p50 2.003, p95 4.293, max 7.664.

Top passes by p50 (valid samples only):

| pass | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---:|---:|---:|---:|---:|
| ShadowPass | 16.796–21.637 | 18.480–37.801 | 20.028–39.020 | 594–599 | 3 |
| ScenePass | 5.400–8.081 | 7.243–12.086 | 7.591–13.172 | 594–599 | 3 |
| GroomPass | 1.587–2.598 | 3.984–5.976 | 4.115–7.476 | 594–599 | 3 |
| RayTracingScenePass | 1.041–1.482 | 1.468–3.818 | 1.632–5.583 | 594–599 | 3 |
| GTAOPass | 0.301–0.468 | 0.492–0.748 | 0.722–0.892 | 594–599 | 3 |
| DeferredLightingPass | 0.296–0.431 | 0.397–1.371 | 0.567–1.591 | 594–599 | 3 |
| FoliageCull | 0.170–0.227 | 0.197–0.427 | 0.201–0.480 | 594–599 | 3 |
| TAAPass | 0.127–0.193 | 0.136–1.181 | 0.140–1.347 | 594–599 | 3 |
| BloomPass | 0.079–0.112 | 0.084–0.311 | 0.335–0.393 | 594–599 | 3 |
| VirtualShadowMapMarkPass | 0.049–0.067 | 0.081–0.118 | 0.085–0.132 | 594–599 | 3 |
| SkinDiffusionPass | 0.032–0.043 | 0.034–0.068 | 0.036–0.074 | 594–599 | 3 |
| FXAAPass | 0.028–0.039 | 0.031–0.053 | 0.033–0.056 | 594–599 | 3 |
| DiffusionDepthSnapshotPass | 0.024–0.037 | 0.029–0.273 | 0.032–0.304 | 594–599 | 3 |
| GroomSceneDepthSnapshotPass | 0.025–0.031 | 0.026–0.094 | 0.028–0.114 | 594–599 | 3 |
| SceneVelocitySeedPass | 0.017–0.028 | 0.019–0.153 | 0.022–0.213 | 594–599 | 3 |

Top sub-passes by p50 (valid samples only):

| sub-pass | parent | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---|---:|---:|---:|---:|---:|
| ShadowPass/VsmSun | ShadowPass | 14.677–19.811 | 15.798–31.935 | 16.757–32.862 | 594–599 | 3 |
| ShadowPass/VsmSun/FoliageCasters | ShadowPass | 10.307–13.915 | 11.162–19.979 | 11.729–21.452 | 594–599 | 3 |
| ScenePass/DepthPrepass | ScenePass | 2.706–4.024 | 3.627–5.910 | 3.787–7.070 | 594–599 | 3 |
| ScenePass/Color | ScenePass | 2.620–3.886 | 3.606–5.687 | 3.820–6.708 | 594–599 | 3 |
| ShadowPass/VsmSun/FoliageCull | ShadowPass | 2.745–3.411 | 3.109–5.579 | 3.253–6.899 | 594–599 | 3 |
| ShadowPass/VsmLocal | ShadowPass | 1.206–1.384 | 2.329–3.844 | 2.621–3.903 | 594–599 | 3 |
| ShadowPass/VsmSun/TerrainCasters | ShadowPass | 0.872–1.128 | 0.925–3.108 | 0.967–3.432 | 594–599 | 3 |
| RayTracingScenePass/AccelerationStructureBuild | RayTracingScenePass | 0.735–1.047 | 1.083–2.604 | 1.209–4.012 | 594–599 | 3 |
| GroomPass/StrandShade | GroomPass | 0.774–0.998 | 1.324–2.051 | 1.399–2.721 | 594–599 | 3 |
| ShadowPass/VsmSun/GroomCasters | ShadowPass | 0.596–0.882 | 0.751–3.138 | 0.949–3.399 | 594–599 | 3 |

Excluded per-pass samples (all counted runs): duplicate-frame-pass 1119.

Memory: peak GPU resident 3878.4–3879.4 MiB, live GPU 3263.7–3266.1 MiB, peak CPU tracked 82.6 MiB, GPU after scene release n/a.
Report: GPU peak 5094.4–5095.0 MiB / live 3158.4 MiB; CPU peak 82.6 MiB / live 82.6 MiB; reconciliation reconciled; residency osReported.
Top GPU owners (first counted run): Unattributed 699.3 MiB, ShadowPass 691.1 MiB, ShadowMap 640.0 MiB, TransientPool 281.2 MiB, GroomPass 274.3 MiB, RayTracing 141.0 MiB, Foliage pinned representations 139.2 MiB, RendererInit 131.8 MiB, GBuffer 101.2 MiB, TemporalHistory 89.1 MiB.

Not counted:

- `vk-hybrid/round-01-attempt-1` failed: process failed: ValueError: editor capture incomplete: {'attachmentFailures': 0, 'backend': 'vulkan', 'cameras': [{'attachments': [{'file': 'Beauty.png', 'height': 1080, 'name': 'Beauty', 'width': 1920}, {'file': 'SceneColorHDR.hdr', 'height': 1080, 'name': 'SceneColorHDR', 'width': 1920}, {'file': 'Albedo.png', 'height': 1080, 'name': 'Albedo', 'width': 1920}, {'file': 'Normals.png', 'height': 1080, 'name': 'Normals', 'width': 1920}, {'file': 'Velocity.png', 'height': 1080, 'name': 'Velocity', 'width': 1920}, {'file': 'SceneDepth.hdr', 'height': 1080, 'name': 'SceneDepth', 'width': 1920}], 'id': 'head-closeup'}, {'attachments': [{'file': 'Beauty.png', 'height': 1080, 'name': 'Beauty', 'width': 1920}, {'file': 'SceneColorHDR.hdr', 'height': 1080, 'name': 'SceneColorHDR', 'width': 1920}, {'file': 'Albedo.png', 'height': 1080, 'name': 'Albedo', 'width': 1920}, {'file': 'Normals.png', 'height': 1080, 'name': 'Normals', 'width': 1920}, {'file': 'Velocity.png', 'height': 1080, 'name': 'Velocity', 'width': 1920}, {'file': 'SceneDepth.hdr', 'height': 1080, 'name': 'SceneDepth', 'width': 1920}], 'id': 'head-backlit'}, {'attachments': [{'file': 'Beauty.png', 'height': 1080, 'name': 'Beauty', 'width': 1920}, {'file': 'SceneColorHDR.hdr', 'height': 1080, 'name': 'SceneColorHDR', 'width': 1920}, {'file': 'Albedo.png', 'height': 1080, 'name': 'Albedo', 'width': 1920}, {'file': 'Normals.png', 'height': 1080, 'name': 'Normals', 'width': 1920}, {'file': 'Velocity.png', 'height': 1080, 'name': 'Velocity', 'width': 1920}, {'file': 'SceneDepth.hdr', 'height': 1080, 'name': 'SceneDepth', 'width': 1920}], 'id': 'coats-closeup'}, {'attachments': [{'file': 'Beauty.png', 'height': 1080, 'name': 'Beauty', 'width': 1920}, {'file': 'SceneColorHDR.hdr', 'height': 1080, 'name': 'SceneColorHDR', 'width': 1920}, {'file': 'Albedo.png', 'height': 1080, 'name': 'Albedo', 'width': 1920}, {'file': 'Normals.png', 'height': 1080, 'name': 'Normals', 'width': 1920}, {'file': 'Velocity.png', 'height': 1080, 'name': 'Velocity', 'width': 1920}, {'file': 'SceneDepth.hdr', 'height': 1080, 'name': 'SceneDepth', 'width': 1920}], 'id': 'population-dolly'}, {'attachments': [{'file': 'Beauty.png', 'height': 1080, 'name': 'Beauty', 'width': 1920}, {'file': 'SceneColorHDR.hdr', 'height': 1080, 'name': 'SceneColorHDR', 'width': 1920}, {'file': 'Albedo.png', 'height': 1080, 'name': 'Albedo', 'width': 1920}, {'file': 'Normals.png', 'height': 1080, 'name': 'Normals', 'width': 1920}, {'file': 'Velocity.png', 'height': 1080, 'name': 'Velocity', 'width': 1920}, {'file': 'SceneDepth.hdr', 'height': 1080, 'name': 'SceneDepth', 'width': 1920}], 'id': 'rapid-turn'}], 'determinismNote': "editor-mcp host: the scene clock is pinned to the manifest's StartTimeSeconds + n * FixedDtSeconds like the test binary's, but frame pacing, quality tiering and TAA history stay the live editor's — the deterministic run-twice product is the test binary's --olo-capture-manifest front door (docs/guides/renderer-benchmarks.md)", 'host': 'editor-mcp', 'id': 'acceptance-hybrid', 'outDir': 'C:/repos/OloEngine-integrated-renderer-release-baseline-1799/build-cached/scratch/b1799/campaign2/vk-hybrid/round-01-attempt-1', 'warmupTimedOut': True}
