# Integrated AAA acceptance gate (#1259)

Overall: **FAIL**. Gate: p95 frame time <= 16.667 ms in every scenario of every counted run; 3 counted runs required per cell.

GPU: NVIDIA GeForce RTX 4090; driver 617.14. CPU: Intel(R) Core(TM) i7-14700KF. OS: 10.0.22631.

Build (exe): Release / clang-cl, commit 32c33b9f842b9d2abf45f6c1fcc54be40a40b52a, dirty diff f597ffe27bb0, 44 untracked files, binary 1a4b5a1f2e98.
Asset hash: full-tree.

Ranges are min–max over counted runs. Frame time is a different quantity per host (see each cell); hosts are never pooled.


## gl-forward: FAIL

run 1 coats-closeup p95 168.689 ms > 16.667 ms; run 1 head-backlit p95 68.476 ms > 16.667 ms; run 1 head-closeup p95 407.060 ms > 16.667 ms; run 1 population-dolly p95 303.258 ms > 16.667 ms; run 1 rapid-turn p95 229.387 ms > 16.667 ms; run 2 coats-closeup p95 167.855 ms > 16.667 ms; run 2 head-backlit p95 72.238 ms > 16.667 ms; run 2 head-closeup p95 371.815 ms > 16.667 ms; run 2 population-dolly p95 284.555 ms > 16.667 ms; run 2 rapid-turn p95 218.264 ms > 16.667 ms; run 3 coats-closeup p95 172.661 ms > 16.667 ms; run 3 head-backlit p95 71.175 ms > 16.667 ms; run 3 head-closeup p95 320.898 ms > 16.667 ms; run 3 population-dolly p95 269.878 ms > 16.667 ms; run 3 rapid-turn p95 209.939 ms > 16.667 ms. Counted runs: 3 of 3.

Host: test-binary / opengl. Frame time = wall-clock Scene::OnUpdateEditor call, excluding attachment readback.
Internal 1920x1080, display 1920x1080 (native); upscaler resolved/fallback: {"fallback": "none", "resolved": "native"}.
Selected: lightingTap=None, msaaSamples=1, path=Forward, rayTracedShadowsRequested=False, upscaleMode=0, upscaleTechnique=0, virtualShadowMaps=True; VSM selected: True; pinned by manifest: MSAASampleCount, Path, RayTracedShadowsEnabled, Upscale, UpscaleTechnique, VirtualShadowMaps.
Selected settings are what the renderer was set to, not proof each technique produced or consumed work.

### coats-closeup (frames/run 300)

| channel | p50 ms | p95 ms | p99 ms | max ms | gate misses | samples |
|---|---:|---:|---:|---:|---:|---:|
| frame time | 137.88–149.21 | 167.85–172.66 | 175.85–191.03 | 556.07–641.39 | 300 | 300 |
| CPU | 135.97–147.17 | 165.61–170.82 | 173.89–189.12 | 554.30–639.68 | 300 | 300 |
| GPU total (valid) | 68.04–77.51 | 128.26–144.44 | 153.35–170.45 | 459.12–543.59 | 300 | 300 |
| fence wait | 0.01 | 0.01 | 0.01 | 0.01–0.03 | 0 | 300 |
| present wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording wall | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording join wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |

Top passes by p50 (valid samples only):

| pass | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---:|---:|---:|---:|---:|
| ShadowPass | 33.439–37.920 | 37.124–43.508 | 39.709–47.057 | 300 | 3 |
| FoliagePass | 19.378–22.618 | 20.348–25.862 | 20.596–26.796 | 300 | 3 |
| GroomPass | 9.450–10.891 | 11.483–13.348 | 15.965–33.342 | 300 | 3 |
| ScenePass | 1.554–1.754 | 1.765–2.122 | 1.881–2.492 | 300 | 3 |
| ScenePrepassPass | 1.356–1.489 | 1.476–1.760 | 1.601–2.097 | 300 | 3 |
| FoliageCull | 0.868–1.154 | 2.792–3.110 | 3.372–4.008 | 300 | 3 |
| VirtualShadowMapMarkPass | 0.284–0.315 | 0.313–0.404 | 0.408–0.472 | 300 | 3 |
| TAAPass | 0.153–0.166 | 0.159–0.226 | 0.164–0.526 | 300 | 3 |
| DiffusionHandoffSnapshotPass | 0.038–0.043 | 0.039–0.046 | 0.039–0.048 | 300 | 3 |
| DiffusionDepthSnapshotPass | 0.029–0.033 | 0.030–0.038 | 0.030–0.039 | 300 | 3 |
| SkinDiffusionPass | 0.029–0.033 | 0.033–0.040 | 0.036–0.043 | 300 | 3 |
| ToneMapPass | 0.016–0.018 | 0.019–0.024 | 0.020–0.028 | 300 | 3 |
| UICompositePass | 0.009–0.011 | 0.010–0.013 | 0.010–0.014 | 300 | 3 |
| ParticlePass | 0.006–0.007 | 0.014–0.016 | 0.015–0.017 | 300 | 3 |
| FinalPass | 0.004–0.005 | 0.007–0.008 | 0.007–0.010 | 300 | 3 |

Top sub-passes by p50 (valid samples only):

| sub-pass | parent | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---|---:|---:|---:|---:|---:|
| GroomPass/RootFrames | GroomPass | 4.379–4.987 | 6.005–6.498 | 6.609–10.178 | 300 | 3 |
| ScenePass/Color | ScenePass | 1.543–1.737 | 1.641–2.045 | 1.760–2.229 | 300 | 3 |
| ScenePrepassPass/DepthPrepass | ScenePrepassPass | 1.341–1.467 | 1.383–1.674 | 1.496–1.963 | 300 | 3 |
| GroomPass/StrandShade | GroomPass | 0.778–0.862 | 0.820–1.068 | 0.830–4.671 | 300 | 3 |
| GroomPass/StrandPrepass | GroomPass | 0.635–0.694 | 0.659–0.921 | 0.674–5.157 | 300 | 3 |
| ShadowPass/FoliageCull | ShadowPass | 0.060–0.067 | 0.065–0.079 | 0.070–0.147 | 300 | 3 |
| ShadowPass/FoliageCasters | ShadowPass | 0.011–0.012 | 0.040–0.044 | 0.043–0.050 | 300 | 3 |

### head-backlit (frames/run 300)

| channel | p50 ms | p95 ms | p99 ms | max ms | gate misses | samples |
|---|---:|---:|---:|---:|---:|---:|
| frame time | 42.93–48.29 | 68.48–72.24 | 72.66–75.97 | 77.99–79.52 | 300 | 300 |
| CPU | 41.66–46.37 | 67.30–70.50 | 71.46–74.62 | 76.36–78.30 | 300 | 300 |
| GPU total (valid) | 23.50–28.65 | 48.18–55.17 | 51.34–58.23 | 54.72–62.56 | 300 | 300 |
| fence wait | 0.00 | 0.01 | 0.01 | 0.01–0.03 | 0 | 300 |
| present wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording wall | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording join wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |

Top passes by p50 (valid samples only):

| pass | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---:|---:|---:|---:|---:|
| ShadowPass | 7.028–9.884 | 10.418–15.589 | 11.862–16.818 | 300 | 3 |
| FoliagePass | 3.693–7.602 | 23.687–28.807 | 25.327–31.367 | 300 | 3 |
| GroomPass | 2.913–4.685 | 9.933–24.920 | 28.330–29.192 | 300 | 3 |
| ScenePass | 1.855–3.168 | 29.141–30.023 | 30.787–33.516 | 300 | 3 |
| ScenePrepassPass | 0.445–0.699 | 0.617–1.092 | 0.754–1.371 | 300 | 3 |
| SkinDiffusionPass | 0.151–0.227 | 0.198–0.346 | 0.221–0.431 | 300 | 3 |
| VirtualShadowMapMarkPass | 0.190–0.205 | 0.210–0.231 | 0.236–0.537 | 300 | 3 |
| TAAPass | 0.125–0.199 | 0.173–1.038 | 0.801–1.353 | 300 | 3 |
| FoliageCull | 0.090–0.124 | 0.113–0.258 | 0.231–0.276 | 300 | 3 |
| DiffusionHandoffSnapshotPass | 0.042–0.054 | 0.050–0.148 | 0.084–0.178 | 300 | 3 |
| DiffusionDepthSnapshotPass | 0.031–0.042 | 0.038–0.104 | 0.055–0.128 | 300 | 3 |
| ToneMapPass | 0.015–0.025 | 0.022–0.041 | 0.032–0.048 | 300 | 3 |
| UICompositePass | 0.010–0.016 | 0.014–0.053 | 0.038–0.067 | 300 | 3 |
| ParticlePass | 0.006–0.008 | 0.013–0.017 | 0.016–0.025 | 300 | 3 |
| FinalPass | 0.005–0.006 | 0.008–0.017 | 0.015–0.019 | 300 | 3 |

Top sub-passes by p50 (valid samples only):

| sub-pass | parent | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---|---:|---:|---:|---:|---:|
| ScenePass/Color | ScenePass | 1.849–3.149 | 29.137–30.020 | 30.782–33.511 | 300 | 3 |
| GroomPass/StrandShade | GroomPass | 1.593–2.605 | 2.058–4.962 | 3.889–5.240 | 300 | 3 |
| GroomPass/StrandPrepass | GroomPass | 0.687–0.898 | 0.829–3.450 | 3.030–3.823 | 300 | 3 |
| ScenePrepassPass/DepthPrepass | ScenePrepassPass | 0.432–0.678 | 0.598–1.043 | 0.718–1.220 | 300 | 3 |
| ShadowPass/FoliageCull | ShadowPass | 0.063–0.085 | 0.082–0.162 | 0.146–0.567 | 300 | 3 |
| ShadowPass/FoliageCasters | ShadowPass | 0.007–0.009 | 0.023–0.032 | 0.031–0.045 | 300 | 3 |
| GroomPass/RootFrames | GroomPass | 0.001 | 0.001 | 0.001–0.002 | 300 | 3 |

### head-closeup (frames/run 300)

| channel | p50 ms | p95 ms | p99 ms | max ms | gate misses | samples |
|---|---:|---:|---:|---:|---:|---:|
| frame time | 283.40–340.70 | 320.90–407.06 | 343.56–430.09 | 355.87–474.31 | 300 | 300 |
| CPU | 280.71–337.27 | 317.92–402.81 | 340.76–426.60 | 352.77–470.55 | 300 | 300 |
| GPU total (valid) | 122.54–148.14 | 147.31–187.57 | 158.40–213.48 | 222.12–333.39 | 300 | 300 |
| fence wait | 0.01 | 0.01 | 0.01 | 0.02–0.03 | 0 | 300 |
| present wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording wall | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording join wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |

Top passes by p50 (valid samples only):

| pass | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---:|---:|---:|---:|---:|
| GroomPass | 90.483–115.484 | 113.431–147.485 | 124.031–160.380 | 300 | 3 |
| ShadowPass | 18.233–24.525 | 20.884–30.877 | 21.916–37.572 | 300 | 3 |
| FoliagePass | 3.939–9.134 | 4.533–11.857 | 4.825–14.336 | 300 | 3 |
| ScenePass | 4.410–9.121 | 5.457–13.390 | 5.829–23.140 | 300 | 3 |
| ScenePrepassPass | 1.934–4.201 | 3.104–7.238 | 3.430–14.581 | 300 | 3 |
| TAAPass | 0.776–0.854 | 1.095–2.084 | 1.397–2.803 | 300 | 3 |
| SkinDiffusionPass | 0.198–0.447 | 0.215–0.547 | 0.226–1.059 | 300 | 3 |
| FoliageCull | 0.301–0.369 | 0.339–0.940 | 0.359–1.260 | 300 | 3 |
| VirtualShadowMapMarkPass | 0.242–0.244 | 0.574–0.620 | 1.013–2.110 | 300 | 3 |
| DiffusionHandoffSnapshotPass | 0.077–0.100 | 0.117–0.205 | 0.144–0.347 | 300 | 3 |
| DiffusionDepthSnapshotPass | 0.068–0.088 | 0.084–0.151 | 0.111–0.319 | 300 | 3 |
| ToneMapPass | 0.030–0.051 | 0.041–0.081 | 0.045–0.458 | 300 | 3 |
| UICompositePass | 0.029–0.038 | 0.036–0.053 | 0.041–0.113 | 300 | 3 |
| FinalPass | 0.014–0.017 | 0.018–0.026 | 0.020–0.037 | 300 | 3 |
| ParticlePass | 0.011–0.015 | 0.023–0.031 | 0.024–0.039 | 300 | 3 |

Top sub-passes by p50 (valid samples only):

| sub-pass | parent | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---|---:|---:|---:|---:|---:|
| GroomPass/RootFrames | GroomPass | 46.964–70.787 | 63.201–100.708 | 67.540–121.192 | 300 | 3 |
| ScenePass/Color | ScenePass | 4.403–9.113 | 5.452–13.383 | 5.823–23.132 | 300 | 3 |
| ScenePrepassPass/DepthPrepass | ScenePrepassPass | 1.897–4.123 | 3.068–7.173 | 3.356–14.525 | 300 | 3 |
| GroomPass/StrandPrepass | GroomPass | 0.260–0.281 | 0.296–0.355 | 0.326–0.796 | 300 | 3 |
| ShadowPass/FoliageCull | ShadowPass | 0.140–0.177 | 0.158–0.303 | 0.164–0.784 | 300 | 3 |
| GroomPass/StrandShade | GroomPass | 0.034–0.086 | 0.037–0.100 | 0.039–0.108 | 300 | 3 |
| ShadowPass/FoliageCasters | ShadowPass | 0.018–0.020 | 0.057–0.075 | 0.059–0.091 | 300 | 3 |

### population-dolly (frames/run 300)

| channel | p50 ms | p95 ms | p99 ms | max ms | gate misses | samples |
|---|---:|---:|---:|---:|---:|---:|
| frame time | 192.81–215.29 | 269.88–303.26 | 318.21–347.02 | 450.82–552.21 | 300 | 300 |
| CPU | 191.15–213.07 | 268.21–301.41 | 316.46–345.01 | 448.60–550.04 | 300 | 300 |
| GPU total (valid) | 89.56–116.05 | 191.40–212.92 | 231.37–277.33 | 347.22–402.75 | 291–295 | 291–295 |
| fence wait | 0.00 | 0.01 | 0.01 | 0.02–0.10 | 0 | 300 |
| present wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording wall | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording join wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |

Excluded total-GPU samples (all counted runs): duplicate 22.

Top passes by p50 (valid samples only):

| pass | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---:|---:|---:|---:|---:|
| ShadowPass | 47.583–58.514 | 53.658–75.855 | 72.316–112.752 | 291–295 | 3 |
| FoliagePass | 15.522–20.299 | 16.502–25.762 | 16.794–26.899 | 291–295 | 3 |
| GroomPass | 18.690–18.979 | 54.991–89.947 | 106.313–140.058 | 291–295 | 3 |
| ScenePass | 1.404–1.836 | 2.204–2.445 | 2.526–3.606 | 291–295 | 3 |
| ScenePrepassPass | 1.304–1.734 | 2.097–2.326 | 2.386–3.037 | 291–295 | 3 |
| TAAPass | 0.144–0.201 | 0.162–0.550 | 0.415–1.444 | 291–295 | 3 |
| FoliageCull | 0.113–0.152 | 4.092–4.655 | 5.023–6.482 | 291–295 | 3 |
| VirtualShadowMapMarkPass | 0.051–0.067 | 0.071–0.084 | 0.080–0.266 | 291–295 | 3 |
| DiffusionHandoffSnapshotPass | 0.038–0.046 | 0.042–0.050 | 0.251–0.283 | 291–295 | 3 |
| DiffusionDepthSnapshotPass | 0.030–0.038 | 0.033–0.047 | 0.033–0.169 | 291–295 | 3 |
| SkinDiffusionPass | 0.028–0.038 | 0.035–0.051 | 0.038–0.058 | 291–295 | 3 |
| ToneMapPass | 0.016–0.023 | 0.020–0.031 | 0.023–0.305 | 291–295 | 3 |
| UICompositePass | 0.010–0.013 | 0.011–0.029 | 0.012–0.295 | 291–295 | 3 |
| ParticlePass | 0.006–0.008 | 0.015–0.017 | 0.016–0.019 | 291–295 | 3 |
| FinalPass | 0.005 | 0.007–0.010 | 0.010–0.015 | 291–295 | 3 |

Top sub-passes by p50 (valid samples only):

| sub-pass | parent | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---|---:|---:|---:|---:|---:|
| GroomPass/RootFrames | GroomPass | 10.134–11.067 | 35.707–65.342 | 86.289–127.186 | 291–295 | 3 |
| ScenePass/Color | ScenePass | 1.394–1.832 | 2.168–2.442 | 2.523–3.601 | 291–295 | 3 |
| ScenePrepassPass/DepthPrepass | ScenePrepassPass | 1.289–1.709 | 2.077–2.284 | 2.372–3.019 | 291–295 | 3 |
| GroomPass/StrandPrepass | GroomPass | 0.062–0.096 | 0.068–0.111 | 0.071–0.540 | 291–295 | 3 |
| ShadowPass/FoliageCull | ShadowPass | 0.077–0.095 | 0.084–0.173 | 0.199–0.348 | 291–295 | 3 |
| GroomPass/StrandShade | GroomPass | 0.054–0.074 | 0.059–0.092 | 0.060–0.104 | 291–295 | 3 |
| ShadowPass/FoliageCasters | ShadowPass | 0.010–0.011 | 0.049–0.058 | 0.054–0.074 | 291–295 | 3 |

### rapid-turn (frames/run 300)

| channel | p50 ms | p95 ms | p99 ms | max ms | gate misses | samples |
|---|---:|---:|---:|---:|---:|---:|
| frame time | 80.68–84.14 | 209.94–229.39 | 242.21–252.94 | 284.38–319.55 | 300 | 300 |
| CPU | 79.49–82.62 | 208.44–227.78 | 240.49–250.73 | 283.46–318.00 | 300 | 300 |
| GPU total (valid) | 63.18–66.38 | 128.36–139.25 | 174.66–207.83 | 184.60–248.91 | 293–297 | 293–297 |
| fence wait | 0.00 | 0.01 | 0.01–0.71 | 0.02–2.77 | 0 | 300 |
| present wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording wall | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording join wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |

Excluded total-GPU samples (all counted runs): duplicate 14.

Top passes by p50 (valid samples only):

| pass | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---:|---:|---:|---:|---:|
| ShadowPass | 41.772–44.105 | 48.674–66.452 | 52.292–70.925 | 293–297 | 3 |
| FoliagePass | 14.266–14.371 | 16.409–21.037 | 17.156–25.314 | 293–297 | 3 |
| GroomPass | 3.598–3.768 | 30.680–39.215 | 49.554–65.335 | 293–297 | 3 |
| ScenePass | 0.673–0.674 | 1.639–2.049 | 2.229–2.351 | 293–297 | 3 |
| ScenePrepassPass | 0.586–0.590 | 1.494–2.118 | 1.977–2.749 | 293–297 | 3 |
| TAAPass | 0.128–0.129 | 0.143–0.743 | 0.718–0.965 | 293–297 | 3 |
| FoliageCull | 0.090–0.091 | 4.094–4.576 | 5.214–6.167 | 293–297 | 3 |
| VirtualShadowMapMarkPass | 0.043–0.047 | 0.055–0.081 | 0.060–0.292 | 293–297 | 3 |
| DiffusionHandoffSnapshotPass | 0.038 | 0.040–0.048 | 0.041–0.051 | 293–297 | 3 |
| DiffusionDepthSnapshotPass | 0.030 | 0.031–0.113 | 0.033–0.206 | 293–297 | 3 |
| SkinDiffusionPass | 0.027 | 0.033–0.045 | 0.050–0.259 | 293–297 | 3 |
| ToneMapPass | 0.015 | 0.019–0.031 | 0.023–0.039 | 293–297 | 3 |
| UICompositePass | 0.010 | 0.013–0.036 | 0.015–0.043 | 293–297 | 3 |
| ParticlePass | 0.006 | 0.015 | 0.018–0.022 | 293–297 | 3 |
| FinalPass | 0.004–0.005 | 0.007–0.013 | 0.009–0.016 | 293–297 | 3 |

Top sub-passes by p50 (valid samples only):

| sub-pass | parent | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---|---:|---:|---:|---:|---:|
| GroomPass/RootFrames | GroomPass | 1.762–1.965 | 13.219–19.079 | 28.658–42.878 | 293–297 | 3 |
| ScenePass/Color | ScenePass | 0.666–0.667 | 1.621–2.030 | 2.225–2.347 | 293–297 | 3 |
| ScenePrepassPass/DepthPrepass | ScenePrepassPass | 0.565–0.571 | 1.479–2.102 | 1.964–2.711 | 293–297 | 3 |
| ShadowPass/FoliageCull | ShadowPass | 0.072–0.074 | 0.081–0.173 | 0.179–0.667 | 293–297 | 3 |
| GroomPass/StrandPrepass | GroomPass | 0.055–0.069 | 0.101–0.238 | 0.117–0.459 | 293–297 | 3 |
| GroomPass/StrandShade | GroomPass | 0.047–0.048 | 0.055–0.081 | 0.058–0.090 | 293–297 | 3 |
| ShadowPass/FoliageCasters | ShadowPass | 0.011–0.012 | 0.028–0.030 | 0.048–0.167 | 293–297 | 3 |

Excluded per-pass samples (all counted runs): duplicate-frame-pass 376750.

Memory: peak GPU resident 1890.2 MiB, live GPU 1676.4 MiB, peak CPU tracked 92.7 MiB, GPU after scene release 1676.4 MiB.
Report: GPU peak 4253.1 MiB / live 1673.8 MiB; CPU peak 92.7 MiB / live 92.7 MiB; reconciliation notObservable; residency unknown.
Top GPU owners (first counted run): ShadowPass 501.0 MiB, GroomPass 411.7 MiB, Unattributed 196.8 MiB, TransientPool 166.1 MiB, RendererInit 144.3 MiB, Foliage pinned representations 123.8 MiB, ShadowMap 80.0 MiB, Texture2D 25.3 MiB, TemporalHistory 15.8 MiB, EnvironmentMap 4.1 MiB.

## gl-forward-plus: FAIL

run 1 coats-closeup p95 168.286 ms > 16.667 ms; run 1 head-backlit p95 67.561 ms > 16.667 ms; run 1 head-closeup p95 378.959 ms > 16.667 ms; run 1 population-dolly p95 295.317 ms > 16.667 ms; run 1 rapid-turn p95 238.309 ms > 16.667 ms; run 2 coats-closeup p95 167.278 ms > 16.667 ms; run 2 head-backlit p95 65.742 ms > 16.667 ms; run 2 head-closeup p95 356.983 ms > 16.667 ms; run 2 population-dolly p95 289.528 ms > 16.667 ms; run 2 rapid-turn p95 215.737 ms > 16.667 ms; run 3 coats-closeup p95 167.289 ms > 16.667 ms; run 3 head-backlit p95 65.704 ms > 16.667 ms; run 3 head-closeup p95 336.208 ms > 16.667 ms; run 3 population-dolly p95 271.828 ms > 16.667 ms; run 3 rapid-turn p95 214.120 ms > 16.667 ms. Counted runs: 3 of 3.

Host: test-binary / opengl. Frame time = wall-clock Scene::OnUpdateEditor call, excluding attachment readback.
Internal 1920x1080, display 1920x1080 (native); upscaler resolved/fallback: {"fallback": "none", "resolved": "native"}.
Selected: lightingTap=None, msaaSamples=1, path=ForwardPlus, rayTracedShadowsRequested=False, upscaleMode=0, upscaleTechnique=0, virtualShadowMaps=True; VSM selected: True; pinned by manifest: MSAASampleCount, Path, RayTracedShadowsEnabled, Upscale, UpscaleTechnique, VirtualShadowMaps.
Selected settings are what the renderer was set to, not proof each technique produced or consumed work.

### coats-closeup (frames/run 300)

| channel | p50 ms | p95 ms | p99 ms | max ms | gate misses | samples |
|---|---:|---:|---:|---:|---:|---:|
| frame time | 138.43–149.42 | 167.28–168.29 | 180.72–185.97 | 522.56–595.42 | 300 | 300 |
| CPU | 136.62–147.61 | 165.25–166.20 | 178.01–184.16 | 520.82–593.75 | 300 | 300 |
| GPU total (valid) | 68.64–70.82 | 133.24–142.86 | 153.65–163.65 | 415.80–496.16 | 300 | 300 |
| fence wait | 0.01 | 0.01 | 0.01 | 0.02–0.03 | 0 | 300 |
| present wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording wall | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording join wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |

Top passes by p50 (valid samples only):

| pass | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---:|---:|---:|---:|---:|
| ShadowPass | 34.121–35.115 | 37.482–38.074 | 39.415–43.887 | 300 | 3 |
| FoliagePass | 19.380–19.863 | 20.300–21.631 | 20.563–22.966 | 300 | 3 |
| GroomPass | 9.494–10.025 | 11.586–11.870 | 14.209–18.198 | 300 | 3 |
| ScenePass | 1.484–1.499 | 1.565–1.894 | 1.799–2.129 | 300 | 3 |
| ScenePrepassPass | 1.319–1.351 | 1.477–1.756 | 1.599–1.985 | 300 | 3 |
| FoliageCull | 0.838–0.949 | 2.818–3.302 | 3.728–3.799 | 300 | 3 |
| VirtualShadowMapMarkPass | 0.273–0.292 | 0.322–0.366 | 0.501–0.582 | 300 | 3 |
| TAAPass | 0.153–0.154 | 0.159–0.166 | 0.160–0.399 | 300 | 3 |
| DiffusionHandoffSnapshotPass | 0.038 | 0.039–0.040 | 0.039–0.042 | 300 | 3 |
| DiffusionDepthSnapshotPass | 0.029–0.030 | 0.030–0.031 | 0.031–0.033 | 300 | 3 |
| SkinDiffusionPass | 0.029 | 0.033–0.034 | 0.036 | 300 | 3 |
| ToneMapPass | 0.015–0.016 | 0.019–0.020 | 0.020 | 300 | 3 |
| UICompositePass | 0.009 | 0.010 | 0.012–0.013 | 300 | 3 |
| ParticlePass | 0.006 | 0.014 | 0.015–0.016 | 300 | 3 |
| FinalPass | 0.004 | 0.007 | 0.007 | 300 | 3 |

Top sub-passes by p50 (valid samples only):

| sub-pass | parent | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---|---:|---:|---:|---:|---:|
| GroomPass/RootFrames | GroomPass | 4.303–4.735 | 5.808–6.359 | 6.607–7.285 | 300 | 3 |
| ScenePass/Color | ScenePass | 1.431–1.450 | 1.504–1.658 | 1.602–1.845 | 300 | 3 |
| ScenePrepassPass/DepthPrepass | ScenePrepassPass | 1.306–1.335 | 1.346–1.465 | 1.428–1.742 | 300 | 3 |
| GroomPass/StrandShade | GroomPass | 0.778–0.780 | 0.824–0.825 | 0.831–0.834 | 300 | 3 |
| GroomPass/StrandPrepass | GroomPass | 0.635–0.639 | 0.666–0.670 | 0.683–0.687 | 300 | 3 |
| ShadowPass/FoliageCull | ShadowPass | 0.060–0.061 | 0.065–0.067 | 0.071–0.304 | 300 | 3 |
| ScenePass/LightCulling | ScenePass | 0.041–0.042 | 0.046–0.048 | 0.051–0.061 | 300 | 3 |
| ShadowPass/FoliageCasters | ShadowPass | 0.011 | 0.040–0.042 | 0.043–0.045 | 300 | 3 |

### head-backlit (frames/run 300)

| channel | p50 ms | p95 ms | p99 ms | max ms | gate misses | samples |
|---|---:|---:|---:|---:|---:|---:|
| frame time | 43.85–45.60 | 65.70–67.56 | 69.66–71.53 | 71.85–74.39 | 300 | 300 |
| CPU | 42.68–44.48 | 64.67–66.56 | 68.62–70.41 | 70.80–73.17 | 300 | 300 |
| GPU total (valid) | 23.98–25.25 | 45.40–51.77 | 47.69–53.96 | 49.61–54.82 | 300 | 300 |
| fence wait | 0.00 | 0.01 | 0.01 | 0.01–0.05 | 0 | 300 |
| present wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording wall | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording join wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |

Top passes by p50 (valid samples only):

| pass | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---:|---:|---:|---:|---:|
| FoliagePass | 8.049–9.494 | 25.678–28.510 | 27.944–29.841 | 300 | 3 |
| ShadowPass | 7.685–8.861 | 11.056–13.911 | 13.301–15.441 | 300 | 3 |
| GroomPass | 3.083–3.779 | 5.430–11.227 | 9.179–27.597 | 300 | 3 |
| ScenePass | 1.409–1.884 | 2.312–4.332 | 8.780–10.341 | 300 | 3 |
| ScenePrepassPass | 0.478–0.614 | 0.644–0.938 | 0.775–1.924 | 300 | 3 |
| VirtualShadowMapMarkPass | 0.186–0.207 | 0.215–0.233 | 0.228–0.691 | 300 | 3 |
| SkinDiffusionPass | 0.162–0.203 | 0.208–0.284 | 0.230–0.563 | 300 | 3 |
| TAAPass | 0.132–0.176 | 0.179–0.985 | 0.780–1.335 | 300 | 3 |
| FoliageCull | 0.096–0.115 | 0.229–0.276 | 0.689–0.966 | 300 | 3 |
| DiffusionHandoffSnapshotPass | 0.045–0.051 | 0.051–0.133 | 0.078–0.279 | 300 | 3 |
| DiffusionDepthSnapshotPass | 0.033–0.039 | 0.038–0.100 | 0.049–0.151 | 300 | 3 |
| ToneMapPass | 0.017–0.022 | 0.024–0.039 | 0.031–0.042 | 300 | 3 |
| UICompositePass | 0.010–0.013 | 0.018–0.048 | 0.039–0.057 | 300 | 3 |
| ParticlePass | 0.006–0.008 | 0.014–0.015 | 0.017–0.023 | 300 | 3 |
| FinalPass | 0.005–0.006 | 0.009–0.018 | 0.016–0.022 | 300 | 3 |

Top sub-passes by p50 (valid samples only):

| sub-pass | parent | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---|---:|---:|---:|---:|---:|
| GroomPass/StrandShade | GroomPass | 1.632–2.196 | 2.124–4.549 | 3.921–4.796 | 300 | 3 |
| ScenePass/Color | ScenePass | 1.364–1.822 | 2.256–4.267 | 8.733–10.299 | 300 | 3 |
| GroomPass/StrandPrepass | GroomPass | 0.714–0.819 | 0.819–3.491 | 3.079–3.790 | 300 | 3 |
| ScenePrepassPass/DepthPrepass | ScenePrepassPass | 0.464–0.597 | 0.632–0.899 | 0.745–1.885 | 300 | 3 |
| ShadowPass/FoliageCull | ShadowPass | 0.065–0.079 | 0.084–0.156 | 0.146–0.170 | 300 | 3 |
| ScenePass/LightCulling | ScenePass | 0.042–0.049 | 0.050–0.069 | 0.062–0.072 | 300 | 3 |
| ShadowPass/FoliageCasters | ShadowPass | 0.007–0.008 | 0.025–0.030 | 0.034–0.038 | 300 | 3 |
| GroomPass/RootFrames | GroomPass | 0.001 | 0.001 | 0.001 | 300 | 3 |

### head-closeup (frames/run 300)

| channel | p50 ms | p95 ms | p99 ms | max ms | gate misses | samples |
|---|---:|---:|---:|---:|---:|---:|
| frame time | 290.71–325.12 | 336.21–378.96 | 348.95–399.16 | 365.32–440.75 | 300 | 300 |
| CPU | 287.93–322.26 | 333.06–375.62 | 346.10–396.38 | 362.40–437.47 | 300 | 300 |
| GPU total (valid) | 133.26–139.71 | 164.23–173.06 | 170.42–206.33 | 191.86–297.36 | 300 | 300 |
| fence wait | 0.01 | 0.01 | 0.01 | 0.02–0.08 | 0 | 300 |
| present wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording wall | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording join wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |

Top passes by p50 (valid samples only):

| pass | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---:|---:|---:|---:|---:|
| GroomPass | 87.743–108.711 | 117.799–135.727 | 128.144–145.375 | 300 | 3 |
| ShadowPass | 19.137–23.963 | 23.260–29.897 | 25.254–37.075 | 300 | 3 |
| FoliagePass | 3.900–9.217 | 5.884–11.684 | 7.194–14.863 | 300 | 3 |
| ScenePass | 3.650–8.076 | 5.273–11.271 | 6.166–17.329 | 300 | 3 |
| ScenePrepassPass | 1.890–4.352 | 3.059–7.592 | 4.696–13.614 | 300 | 3 |
| TAAPass | 0.772–0.873 | 0.913–2.022 | 1.458–2.376 | 300 | 3 |
| SkinDiffusionPass | 0.203–0.449 | 0.292–0.520 | 0.601–1.329 | 300 | 3 |
| FoliageCull | 0.298–0.372 | 0.334–0.897 | 0.343–1.665 | 300 | 3 |
| VirtualShadowMapMarkPass | 0.227–0.309 | 0.552–0.682 | 0.695–1.667 | 300 | 3 |
| DiffusionHandoffSnapshotPass | 0.074–0.100 | 0.101–0.185 | 0.127–0.233 | 300 | 3 |
| DiffusionDepthSnapshotPass | 0.067–0.089 | 0.079–0.154 | 0.096–0.169 | 300 | 3 |
| ToneMapPass | 0.030–0.051 | 0.041–0.072 | 0.143–0.227 | 300 | 3 |
| UICompositePass | 0.028–0.038 | 0.035–0.062 | 0.043–0.089 | 300 | 3 |
| FinalPass | 0.014–0.017 | 0.018–0.025 | 0.020–0.044 | 300 | 3 |
| ParticlePass | 0.011–0.015 | 0.023–0.030 | 0.026–0.033 | 300 | 3 |

Top sub-passes by p50 (valid samples only):

| sub-pass | parent | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---|---:|---:|---:|---:|---:|
| GroomPass/RootFrames | GroomPass | 35.795–66.373 | 64.282–87.668 | 69.100–98.841 | 300 | 3 |
| ScenePass/Color | ScenePass | 3.568–7.921 | 5.201–11.148 | 6.077–16.918 | 300 | 3 |
| ScenePrepassPass/DepthPrepass | ScenePrepassPass | 1.837–4.303 | 3.010–7.428 | 4.646–13.486 | 300 | 3 |
| GroomPass/StrandPrepass | GroomPass | 0.188–0.285 | 0.289–0.666 | 0.313–0.841 | 300 | 3 |
| ShadowPass/FoliageCull | ShadowPass | 0.136–0.177 | 0.155–0.300 | 0.353–0.752 | 300 | 3 |
| ScenePass/LightCulling | ScenePass | 0.065–0.099 | 0.082–0.299 | 0.271–0.545 | 300 | 3 |
| GroomPass/StrandShade | GroomPass | 0.034–0.086 | 0.054–0.098 | 0.061–0.104 | 300 | 3 |
| ShadowPass/FoliageCasters | ShadowPass | 0.017–0.020 | 0.058–0.074 | 0.065–0.259 | 300 | 3 |

### population-dolly (frames/run 300)

| channel | p50 ms | p95 ms | p99 ms | max ms | gate misses | samples |
|---|---:|---:|---:|---:|---:|---:|
| frame time | 193.27–207.75 | 271.83–295.32 | 328.88–334.37 | 436.09–467.19 | 300 | 300 |
| CPU | 191.38–205.99 | 269.85–293.27 | 326.67–332.16 | 434.34–465.35 | 300 | 300 |
| GPU total (valid) | 85.03–86.94 | 194.50–204.99 | 249.21–282.39 | 321.81–342.65 | 289–291 | 289–291 |
| fence wait | 0.00 | 0.01 | 0.01 | 0.02–0.05 | 0 | 300 |
| present wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording wall | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording join wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |

Excluded total-GPU samples (all counted runs): duplicate 29.

Top passes by p50 (valid samples only):

| pass | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---:|---:|---:|---:|---:|
| ShadowPass | 45.962–46.908 | 52.253–52.953 | 73.250–94.394 | 289–291 | 3 |
| GroomPass | 16.697–18.004 | 72.130–78.720 | 124.902–134.097 | 289–291 | 3 |
| FoliagePass | 14.904–15.577 | 16.352–17.101 | 17.737–18.525 | 289–291 | 3 |
| ScenePass | 1.333–1.366 | 1.617–1.757 | 1.741–2.202 | 289–291 | 3 |
| ScenePrepassPass | 1.306–1.343 | 1.602–1.754 | 1.829–2.097 | 289–291 | 3 |
| TAAPass | 0.143–0.145 | 0.162–0.167 | 0.195–0.978 | 289–291 | 3 |
| FoliageCull | 0.111–0.114 | 2.653–3.905 | 4.698–4.778 | 289–291 | 3 |
| VirtualShadowMapMarkPass | 0.052–0.058 | 0.067–0.077 | 0.082–0.088 | 289–291 | 3 |
| DiffusionHandoffSnapshotPass | 0.038–0.039 | 0.042–0.045 | 0.046–0.237 | 289–291 | 3 |
| DiffusionDepthSnapshotPass | 0.030 | 0.032–0.035 | 0.037–0.113 | 289–291 | 3 |
| SkinDiffusionPass | 0.027–0.029 | 0.034–0.035 | 0.037–0.041 | 289–291 | 3 |
| ToneMapPass | 0.016 | 0.020–0.022 | 0.023–0.048 | 289–291 | 3 |
| UICompositePass | 0.010 | 0.011 | 0.013–0.029 | 289–291 | 3 |
| ParticlePass | 0.006 | 0.014–0.015 | 0.016–0.017 | 289–291 | 3 |
| FinalPass | 0.005 | 0.007–0.008 | 0.009–0.013 | 289–291 | 3 |

Top sub-passes by p50 (valid samples only):

| sub-pass | parent | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---|---:|---:|---:|---:|---:|
| GroomPass/RootFrames | GroomPass | 9.691–10.197 | 47.549–64.399 | 83.322–100.297 | 289–291 | 3 |
| ScenePrepassPass/DepthPrepass | ScenePrepassPass | 1.293–1.327 | 1.583–1.742 | 1.817–2.085 | 289–291 | 3 |
| ScenePass/Color | ScenePass | 1.288–1.324 | 1.565–1.710 | 1.680–2.160 | 289–291 | 3 |
| ShadowPass/FoliageCull | ShadowPass | 0.075–0.078 | 0.084–0.089 | 0.178–0.511 | 289–291 | 3 |
| GroomPass/StrandPrepass | GroomPass | 0.063–0.065 | 0.069–0.073 | 0.092–0.424 | 289–291 | 3 |
| GroomPass/StrandShade | GroomPass | 0.054–0.055 | 0.059–0.066 | 0.070–0.080 | 289–291 | 3 |
| ScenePass/LightCulling | ScenePass | 0.037–0.039 | 0.045–0.047 | 0.051–0.059 | 289–291 | 3 |
| ShadowPass/FoliageCasters | ShadowPass | 0.009–0.010 | 0.049–0.052 | 0.055–0.060 | 289–291 | 3 |

### rapid-turn (frames/run 300)

| channel | p50 ms | p95 ms | p99 ms | max ms | gate misses | samples |
|---|---:|---:|---:|---:|---:|---:|
| frame time | 78.25–84.84 | 214.12–238.31 | 250.34–257.77 | 266.25–297.12 | 300 | 300 |
| CPU | 76.98–82.98 | 212.23–236.55 | 248.34–255.68 | 264.12–294.87 | 300 | 300 |
| GPU total (valid) | 63.21–64.33 | 115.30–131.11 | 177.86–203.96 | 216.74–231.99 | 292–294 | 292–294 |
| fence wait | 0.00 | 0.01 | 0.01 | 0.01–0.07 | 0 | 300 |
| present wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording wall | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording join wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |

Excluded total-GPU samples (all counted runs): duplicate 20.

Top passes by p50 (valid samples only):

| pass | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---:|---:|---:|---:|---:|
| ShadowPass | 40.898–41.954 | 46.858–48.551 | 51.576–57.206 | 292–294 | 3 |
| FoliagePass | 13.778–14.018 | 15.728–15.831 | 16.168–16.968 | 292–294 | 3 |
| GroomPass | 3.521–3.745 | 38.958–43.052 | 60.333–70.221 | 292–294 | 3 |
| ScenePass | 0.650–0.653 | 1.427–1.436 | 1.588–2.161 | 292–294 | 3 |
| ScenePrepassPass | 0.584–0.591 | 1.442–1.450 | 1.702–2.066 | 292–294 | 3 |
| TAAPass | 0.128 | 0.142–0.154 | 0.157–0.438 | 292–294 | 3 |
| FoliageCull | 0.088 | 3.966–4.417 | 5.128–5.450 | 292–294 | 3 |
| VirtualShadowMapMarkPass | 0.043–0.047 | 0.053–0.070 | 0.057–0.083 | 292–294 | 3 |
| DiffusionHandoffSnapshotPass | 0.038 | 0.039–0.044 | 0.042–0.047 | 292–294 | 3 |
| DiffusionDepthSnapshotPass | 0.030 | 0.030–0.034 | 0.033–0.241 | 292–294 | 3 |
| SkinDiffusionPass | 0.027 | 0.032–0.035 | 0.037–0.040 | 292–294 | 3 |
| ToneMapPass | 0.015 | 0.019 | 0.022–0.028 | 292–294 | 3 |
| UICompositePass | 0.009–0.010 | 0.013 | 0.014–0.016 | 292–294 | 3 |
| ParticlePass | 0.006 | 0.014–0.015 | 0.015–0.017 | 292–294 | 3 |
| FinalPass | 0.004 | 0.007 | 0.009–0.012 | 292–294 | 3 |

Top sub-passes by p50 (valid samples only):

| sub-pass | parent | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---|---:|---:|---:|---:|---:|
| GroomPass/RootFrames | GroomPass | 1.801–1.870 | 16.795–22.103 | 38.467–52.943 | 292–294 | 3 |
| ScenePass/Color | ScenePass | 0.606–0.610 | 1.381–1.390 | 1.542–2.113 | 292–294 | 3 |
| ScenePrepassPass/DepthPrepass | ScenePrepassPass | 0.566–0.571 | 1.415–1.430 | 1.688–2.053 | 292–294 | 3 |
| ShadowPass/FoliageCull | ShadowPass | 0.071–0.074 | 0.080–0.083 | 0.291–0.299 | 292–294 | 3 |
| GroomPass/StrandPrepass | GroomPass | 0.055–0.060 | 0.097–0.101 | 0.121–0.128 | 292–294 | 3 |
| GroomPass/StrandShade | GroomPass | 0.048 | 0.054–0.058 | 0.060–0.066 | 292–294 | 3 |
| ScenePass/LightCulling | ScenePass | 0.035–0.036 | 0.040–0.042 | 0.044–0.054 | 292–294 | 3 |
| ShadowPass/FoliageCasters | ShadowPass | 0.011 | 0.025 | 0.031–0.252 | 292–294 | 3 |

Excluded per-pass samples (all counted runs): duplicate-frame-pass 377145.

Memory: peak GPU resident 1890.2 MiB, live GPU 1676.4 MiB, peak CPU tracked 92.7 MiB, GPU after scene release 1676.4 MiB.
Report: GPU peak 4253.1 MiB / live 1673.8 MiB; CPU peak 92.7 MiB / live 92.7 MiB; reconciliation notObservable; residency unknown.
Top GPU owners (first counted run): ShadowPass 501.0 MiB, GroomPass 411.7 MiB, Unattributed 196.8 MiB, TransientPool 166.1 MiB, RendererInit 144.3 MiB, Foliage pinned representations 123.8 MiB, ShadowMap 80.0 MiB, Texture2D 25.3 MiB, TemporalHistory 15.8 MiB, EnvironmentMap 4.1 MiB.

## gl-deferred: FAIL

run 1 coats-closeup p95 190.309 ms > 16.667 ms; run 1 head-backlit p95 71.608 ms > 16.667 ms; run 1 head-closeup p95 443.381 ms > 16.667 ms; run 1 population-dolly p95 319.846 ms > 16.667 ms; run 1 rapid-turn p95 236.474 ms > 16.667 ms; run 2 coats-closeup p95 177.032 ms > 16.667 ms; run 2 head-backlit p95 67.170 ms > 16.667 ms; run 2 head-closeup p95 388.604 ms > 16.667 ms; run 2 population-dolly p95 298.926 ms > 16.667 ms; run 2 rapid-turn p95 261.362 ms > 16.667 ms; run 3 coats-closeup p95 174.623 ms > 16.667 ms; run 3 head-backlit p95 71.815 ms > 16.667 ms; run 3 head-closeup p95 350.504 ms > 16.667 ms; run 3 population-dolly p95 291.353 ms > 16.667 ms; run 3 rapid-turn p95 219.670 ms > 16.667 ms. Counted runs: 3 of 3.

Host: test-binary / opengl. Frame time = wall-clock Scene::OnUpdateEditor call, excluding attachment readback.
Internal 1920x1080, display 1920x1080 (native); upscaler resolved/fallback: {"fallback": "none", "resolved": "native"}.
Selected: lightingTap=None, msaaSamples=1, path=Deferred, rayTracedShadowsRequested=False, upscaleMode=0, upscaleTechnique=0, virtualShadowMaps=True; VSM selected: True; pinned by manifest: MSAASampleCount, Path, RayTracedShadowsEnabled, Upscale, UpscaleTechnique, VirtualShadowMaps.
Selected settings are what the renderer was set to, not proof each technique produced or consumed work.

### coats-closeup (frames/run 300)

| channel | p50 ms | p95 ms | p99 ms | max ms | gate misses | samples |
|---|---:|---:|---:|---:|---:|---:|
| frame time | 150.44–160.72 | 174.62–190.31 | 182.52–201.44 | 586.23–605.42 | 300 | 300 |
| CPU | 149.01–159.25 | 171.33–188.69 | 180.93–200.01 | 584.95–604.00 | 300 | 300 |
| GPU total (valid) | 63.05–76.67 | 83.35–141.22 | 158.00–182.77 | 488.40–504.89 | 300 | 300 |
| fence wait | 0.00 | 0.01 | 0.01 | 0.02–0.06 | 0 | 300 |
| present wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording wall | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording join wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |

Top passes by p50 (valid samples only):

| pass | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---:|---:|---:|---:|---:|
| ShadowPass | 36.055–44.832 | 39.397–52.806 | 40.793–58.254 | 300 | 3 |
| GroomPass | 11.824–13.224 | 25.823–33.878 | 31.999–38.185 | 300 | 3 |
| ScenePass | 7.896–10.932 | 8.547–13.479 | 9.057–14.874 | 300 | 3 |
| DeferredLightingPass | 1.826–2.486 | 1.996–3.700 | 2.079–5.587 | 300 | 3 |
| VirtualShadowMapMarkPass | 0.325–0.378 | 0.374–0.697 | 0.389–1.015 | 300 | 3 |
| TAAPass | 0.155–0.223 | 0.161–1.270 | 0.169–1.479 | 300 | 3 |
| FoliageCull | 0.094–0.127 | 5.330–5.703 | 5.847–6.576 | 300 | 3 |
| SceneVelocitySeedPass | 0.094–0.105 | 0.122–0.321 | 0.129–1.059 | 300 | 3 |
| DiffusionHandoffSnapshotPass | 0.038–0.046 | 0.039–0.049 | 0.040–0.051 | 300 | 3 |
| DiffusionDepthSnapshotPass | 0.030–0.039 | 0.031–0.142 | 0.032–0.196 | 300 | 3 |
| SkinDiffusionPass | 0.027–0.037 | 0.033–0.047 | 0.036–0.050 | 300 | 3 |
| ToneMapPass | 0.015–0.022 | 0.020–0.055 | 0.022–0.063 | 300 | 3 |
| UICompositePass | 0.010–0.013 | 0.013–0.019 | 0.014–0.022 | 300 | 3 |
| ParticlePass | 0.005–0.007 | 0.012–0.014 | 0.015–0.020 | 300 | 3 |
| FinalPass | 0.005–0.006 | 0.008–0.012 | 0.009–0.015 | 300 | 3 |

Top sub-passes by p50 (valid samples only):

| sub-pass | parent | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---|---:|---:|---:|---:|---:|
| GroomPass/RootFrames | GroomPass | 4.935–5.597 | 6.790–9.483 | 9.425–11.333 | 300 | 3 |
| ScenePass/DepthPrepass | ScenePass | 4.036–5.551 | 4.517–6.882 | 4.642–7.889 | 300 | 3 |
| ScenePass/Color | ScenePass | 3.779–5.134 | 4.188–6.538 | 4.362–7.353 | 300 | 3 |
| GroomPass/StrandPrepass | GroomPass | 0.866–1.011 | 0.952–5.126 | 0.988–5.905 | 300 | 3 |
| GroomPass/StrandShade | GroomPass | 0.776–0.911 | 0.829–4.330 | 0.843–4.566 | 300 | 3 |
| ShadowPass/FoliageCasters | ShadowPass | 0.679–0.882 | 0.798–1.187 | 0.828–1.415 | 300 | 3 |
| ShadowPass/FoliageCull | ShadowPass | 0.074–0.096 | 0.088–0.183 | 0.092–0.209 | 300 | 3 |
| ScenePass/LightCulling | ScenePass | 0.047–0.058 | 0.055–0.072 | 0.058–0.087 | 300 | 3 |

### head-backlit (frames/run 300)

| channel | p50 ms | p95 ms | p99 ms | max ms | gate misses | samples |
|---|---:|---:|---:|---:|---:|---:|
| frame time | 44.53–47.92 | 67.17–71.81 | 68.81–77.25 | 70.34–82.83 | 300 | 300 |
| CPU | 43.31–46.68 | 65.91–70.52 | 67.62–75.98 | 69.21–81.48 | 300 | 300 |
| GPU total (valid) | 27.03–29.13 | 50.69–60.05 | 53.92–62.45 | 57.84–65.19 | 300 | 300 |
| fence wait | 0.00 | 0.01 | 0.01 | 0.01–0.02 | 0 | 300 |
| present wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording wall | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording join wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |

Top passes by p50 (valid samples only):

| pass | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---:|---:|---:|---:|---:|
| ScenePass | 9.916–11.767 | 29.849–32.324 | 31.425–36.586 | 300 | 3 |
| ShadowPass | 8.094–9.528 | 11.826–15.441 | 12.987–18.568 | 300 | 3 |
| GroomPass | 3.736–4.944 | 5.484–12.982 | 8.987–31.082 | 300 | 3 |
| DeferredLightingPass | 1.624–2.489 | 11.036–12.527 | 12.360–14.492 | 300 | 3 |
| SkinDiffusionPass | 0.159–0.227 | 0.214–0.329 | 0.224–0.678 | 300 | 3 |
| VirtualShadowMapMarkPass | 0.183–0.219 | 0.233–0.474 | 9.891–11.469 | 300 | 3 |
| TAAPass | 0.134–0.189 | 0.188–0.991 | 0.803–1.634 | 300 | 3 |
| FoliageCull | 0.095–0.121 | 0.755–0.927 | 2.401–2.593 | 300 | 3 |
| SceneVelocitySeedPass | 0.093–0.099 | 0.111–0.157 | 0.144–0.923 | 300 | 3 |
| DiffusionHandoffSnapshotPass | 0.044–0.054 | 0.052–0.151 | 0.090–0.343 | 300 | 3 |
| DiffusionDepthSnapshotPass | 0.032–0.043 | 0.041–0.222 | 0.121–0.351 | 300 | 3 |
| ToneMapPass | 0.016–0.025 | 0.023–0.043 | 0.032–0.068 | 300 | 3 |
| UICompositePass | 0.011–0.014 | 0.014–0.046 | 0.036–0.077 | 300 | 3 |
| ParticlePass | 0.006–0.007 | 0.010–0.012 | 0.011–0.017 | 300 | 3 |
| FinalPass | 0.005–0.006 | 0.006–0.016 | 0.012–0.017 | 300 | 3 |

Top sub-passes by p50 (valid samples only):

| sub-pass | parent | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---|---:|---:|---:|---:|---:|
| ScenePass/Color | ScenePass | 9.411–11.315 | 29.161–31.904 | 30.821–36.080 | 300 | 3 |
| GroomPass/StrandShade | GroomPass | 1.660–2.519 | 2.189–4.863 | 3.949–5.176 | 300 | 3 |
| GroomPass/StrandPrepass | GroomPass | 1.179–1.430 | 1.357–3.927 | 3.371–4.107 | 300 | 3 |
| ScenePass/DepthPrepass | ScenePass | 0.387–0.525 | 0.585–0.763 | 0.631–1.237 | 300 | 3 |
| ShadowPass/FoliageCull | ShadowPass | 0.067–0.082 | 0.080–0.156 | 0.139–0.659 | 300 | 3 |
| ScenePass/LightCulling | ScenePass | 0.042–0.053 | 0.051–0.073 | 0.062–0.080 | 300 | 3 |
| ShadowPass/FoliageCasters | ShadowPass | 0.008 | 0.013–0.019 | 0.046–0.052 | 300 | 3 |
| GroomPass/RootFrames | GroomPass | 0.001 | 0.001 | 0.001 | 300 | 3 |

### head-closeup (frames/run 300)

| channel | p50 ms | p95 ms | p99 ms | max ms | gate misses | samples |
|---|---:|---:|---:|---:|---:|---:|
| frame time | 320.54–337.36 | 350.50–443.38 | 368.93–617.39 | 379.11–748.91 | 300 | 300 |
| CPU | 317.56–334.33 | 347.82–438.78 | 365.86–613.18 | 375.99–743.30 | 300 | 300 |
| GPU total (valid) | 146.83–152.33 | 183.98–194.98 | 189.46–273.63 | 216.49–342.15 | 300 | 300 |
| fence wait | 0.01 | 0.01 | 0.01 | 0.02–0.03 | 0 | 300 |
| present wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording wall | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording join wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |

Top passes by p50 (valid samples only):

| pass | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---:|---:|---:|---:|---:|
| GroomPass | 115.556–120.324 | 139.107–163.169 | 156.156–232.460 | 300 | 3 |
| ShadowPass | 19.525–24.312 | 25.014–33.411 | 26.985–42.881 | 300 | 3 |
| ScenePass | 2.630–5.193 | 4.312–8.478 | 4.685–12.961 | 300 | 3 |
| DeferredLightingPass | 2.486–5.072 | 4.031–7.982 | 4.643–9.205 | 300 | 3 |
| TAAPass | 0.234–0.856 | 1.032–2.102 | 1.447–6.049 | 300 | 3 |
| SkinDiffusionPass | 0.203–0.432 | 0.318–0.579 | 0.383–1.511 | 300 | 3 |
| FoliageCull | 0.142–0.362 | 0.333–0.901 | 0.368–1.431 | 300 | 3 |
| VirtualShadowMapMarkPass | 0.209–0.316 | 0.506–0.628 | 0.769–1.157 | 300 | 3 |
| SceneVelocitySeedPass | 0.137–0.184 | 0.404–1.265 | 0.555–1.433 | 300 | 3 |
| DiffusionDepthSnapshotPass | 0.046–0.156 | 0.185–0.382 | 0.236–1.849 | 300 | 3 |
| DiffusionHandoffSnapshotPass | 0.058–0.100 | 0.116–0.231 | 0.140–0.745 | 300 | 3 |
| ToneMapPass | 0.028–0.050 | 0.044–0.092 | 0.048–0.359 | 300 | 3 |
| UICompositePass | 0.017–0.036 | 0.039–0.061 | 0.049–0.105 | 300 | 3 |
| FinalPass | 0.009–0.017 | 0.017–0.023 | 0.019–0.036 | 300 | 3 |
| ParticlePass | 0.010–0.013 | 0.023–0.029 | 0.025–0.039 | 300 | 3 |

Top sub-passes by p50 (valid samples only):

| sub-pass | parent | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---|---:|---:|---:|---:|---:|
| GroomPass/RootFrames | GroomPass | 60.320–74.562 | 78.169–115.481 | 85.852–166.236 | 300 | 3 |
| ScenePass/Color | ScenePass | 1.305–2.614 | 2.270–5.174 | 2.913–7.326 | 300 | 3 |
| ScenePass/DepthPrepass | ScenePass | 1.163–2.271 | 1.937–4.270 | 2.327–6.052 | 300 | 3 |
| GroomPass/StrandPrepass | GroomPass | 0.138–0.285 | 0.307–0.579 | 0.404–0.853 | 300 | 3 |
| ShadowPass/FoliageCull | ShadowPass | 0.084–0.173 | 0.158–0.376 | 0.183–0.850 | 300 | 3 |
| ScenePass/LightCulling | ScenePass | 0.059–0.097 | 0.083–0.129 | 0.210–0.566 | 300 | 3 |
| GroomPass/StrandShade | GroomPass | 0.033–0.079 | 0.054–0.105 | 0.059–0.122 | 300 | 3 |
| ShadowPass/FoliageCasters | ShadowPass | 0.016–0.018 | 0.072–0.100 | 0.084–0.231 | 300 | 3 |

### population-dolly (frames/run 300)

| channel | p50 ms | p95 ms | p99 ms | max ms | gate misses | samples |
|---|---:|---:|---:|---:|---:|---:|
| frame time | 205.30–225.69 | 291.35–319.85 | 323.61–417.23 | 478.79–513.65 | 300 | 300 |
| CPU | 203.47–223.03 | 289.03–317.60 | 321.84–414.63 | 477.07–511.83 | 300 | 300 |
| GPU total (valid) | 89.38–106.75 | 201.34–216.57 | 267.59–299.22 | 349.41–379.70 | 291–292 | 291–292 |
| fence wait | 0.00 | 0.01 | 0.01–0.02 | 0.02–28.60 | 0–1 | 300 |
| present wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording wall | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording join wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |

Excluded total-GPU samples (all counted runs): duplicate 26.

Top passes by p50 (valid samples only):

| pass | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---:|---:|---:|---:|---:|
| ShadowPass | 49.056–51.645 | 57.114–77.909 | 76.346–88.748 | 291–292 | 3 |
| GroomPass | 19.152–21.938 | 69.846–93.521 | 129.506–161.844 | 291–292 | 3 |
| ScenePass | 11.474–12.871 | 12.703–18.304 | 13.083–19.405 | 291–292 | 3 |
| DeferredLightingPass | 1.153–1.315 | 1.533–1.839 | 1.881–2.356 | 291–292 | 3 |
| TAAPass | 0.144–0.160 | 0.164–0.251 | 0.242–0.620 | 291–292 | 3 |
| FoliageCull | 0.113–0.130 | 4.985–5.749 | 5.966–8.849 | 291–292 | 3 |
| SceneVelocitySeedPass | 0.102–0.104 | 0.127–0.131 | 0.168–0.365 | 291–292 | 3 |
| VirtualShadowMapMarkPass | 0.081–0.104 | 0.110–0.132 | 0.176–0.499 | 291–292 | 3 |
| DiffusionHandoffSnapshotPass | 0.039–0.044 | 0.042–0.050 | 0.044–0.053 | 291–292 | 3 |
| DiffusionDepthSnapshotPass | 0.030–0.035 | 0.033–0.044 | 0.035–0.145 | 291–292 | 3 |
| SkinDiffusionPass | 0.028–0.033 | 0.034–0.051 | 0.039–0.282 | 291–292 | 3 |
| ToneMapPass | 0.016–0.018 | 0.020–0.032 | 0.023–0.325 | 291–292 | 3 |
| UICompositePass | 0.010–0.011 | 0.011–0.016 | 0.012–0.018 | 291–292 | 3 |
| ParticlePass | 0.006 | 0.014–0.015 | 0.015–0.018 | 291–292 | 3 |
| FinalPass | 0.005 | 0.007–0.009 | 0.011–0.013 | 291–292 | 3 |

Top sub-passes by p50 (valid samples only):

| sub-pass | parent | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---|---:|---:|---:|---:|---:|
| GroomPass/RootFrames | GroomPass | 10.351–12.129 | 43.388–74.398 | 83.878–109.488 | 291–292 | 3 |
| ScenePass/DepthPrepass | ScenePass | 5.582–6.481 | 6.824–9.380 | 7.229–9.892 | 291–292 | 3 |
| ScenePass/Color | ScenePass | 5.450–6.220 | 6.486–9.073 | 6.742–9.694 | 291–292 | 3 |
| ShadowPass/FoliageCull | ShadowPass | 0.078–0.089 | 0.089–0.111 | 0.195–0.402 | 291–292 | 3 |
| GroomPass/StrandPrepass | GroomPass | 0.065–0.073 | 0.070–0.113 | 0.072–0.419 | 291–292 | 3 |
| GroomPass/StrandShade | GroomPass | 0.053–0.063 | 0.058–0.091 | 0.059–0.097 | 291–292 | 3 |
| ScenePass/LightCulling | ScenePass | 0.045–0.052 | 0.051–0.060 | 0.070–0.263 | 291–292 | 3 |
| ShadowPass/FoliageCasters | ShadowPass | 0.009–0.011 | 0.559–0.664 | 0.784–0.905 | 291–292 | 3 |

### rapid-turn (frames/run 300)

| channel | p50 ms | p95 ms | p99 ms | max ms | gate misses | samples |
|---|---:|---:|---:|---:|---:|---:|
| frame time | 81.97–97.17 | 219.67–261.36 | 239.27–305.47 | 272.77–343.93 | 300 | 300 |
| CPU | 80.44–95.91 | 217.84–259.26 | 236.94–303.23 | 270.14–341.50 | 300 | 300 |
| GPU total (valid) | 58.95–64.40 | 136.68–150.92 | 191.08–203.61 | 198.36–269.89 | 290–293 | 290–293 |
| fence wait | 0.00 | 0.01 | 0.01 | 0.01 | 0 | 300 |
| present wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording wall | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |
| recording join wait | 0.00 | 0.00 | 0.00 | 0.00 | 0 | 300 |

Excluded total-GPU samples (all counted runs): duplicate 26.

Top passes by p50 (valid samples only):

| pass | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---:|---:|---:|---:|---:|
| ShadowPass | 41.775–45.115 | 47.727–52.798 | 51.150–58.392 | 290–293 | 3 |
| ScenePass | 9.342–9.729 | 12.497–13.514 | 13.660–14.918 | 290–293 | 3 |
| GroomPass | 4.157–4.760 | 41.311–56.349 | 68.080–89.764 | 290–293 | 3 |
| DeferredLightingPass | 0.836–0.849 | 1.257–1.372 | 1.396–1.788 | 290–293 | 3 |
| TAAPass | 0.129–0.130 | 0.151–0.154 | 0.166–0.391 | 290–293 | 3 |
| SceneVelocitySeedPass | 0.085–0.095 | 0.106–0.120 | 0.162–0.374 | 290–293 | 3 |
| FoliageCull | 0.091 | 4.482–4.639 | 5.579–6.356 | 290–293 | 3 |
| VirtualShadowMapMarkPass | 0.056–0.065 | 0.078–0.104 | 0.120–0.350 | 290–293 | 3 |
| DiffusionHandoffSnapshotPass | 0.038 | 0.042–0.044 | 0.044–0.050 | 290–293 | 3 |
| DiffusionDepthSnapshotPass | 0.030 | 0.033–0.034 | 0.035–0.041 | 290–293 | 3 |
| SkinDiffusionPass | 0.027 | 0.032–0.033 | 0.036–0.037 | 290–293 | 3 |
| ToneMapPass | 0.015 | 0.019 | 0.022–0.028 | 290–293 | 3 |
| UICompositePass | 0.010 | 0.013–0.014 | 0.015–0.016 | 290–293 | 3 |
| ParticlePass | 0.005 | 0.012–0.013 | 0.014–0.015 | 290–293 | 3 |
| FinalPass | 0.004 | 0.007 | 0.008–0.009 | 290–293 | 3 |

Top sub-passes by p50 (valid samples only):

| sub-pass | parent | p50 ms | p95 ms | p99 ms | samples | runs |
|---|---|---:|---:|---:|---:|---:|
| ScenePass/DepthPrepass | ScenePass | 4.746–4.948 | 6.261–6.967 | 6.912–7.851 | 290–293 | 3 |
| ScenePass/Color | ScenePass | 4.512–4.604 | 6.138–6.753 | 6.857–7.797 | 290–293 | 3 |
| GroomPass/RootFrames | GroomPass | 1.809–2.189 | 10.297–19.383 | 46.862–54.706 | 290–293 | 3 |
| ShadowPass/FoliageCasters | ShadowPass | 0.520–0.525 | 0.689–0.790 | 0.812–1.083 | 290–293 | 3 |
| ShadowPass/FoliageCull | ShadowPass | 0.085–0.086 | 0.093–0.096 | 0.103–0.298 | 290–293 | 3 |
| GroomPass/StrandPrepass | GroomPass | 0.057–0.058 | 0.098–0.104 | 0.106–0.122 | 290–293 | 3 |
| GroomPass/StrandShade | GroomPass | 0.047–0.048 | 0.058–0.059 | 0.061–0.063 | 290–293 | 3 |
| ScenePass/LightCulling | ScenePass | 0.041 | 0.050–0.054 | 0.055–0.059 | 290–293 | 3 |

Excluded per-pass samples (all counted runs): duplicate-frame-pass 377264.

Memory: peak GPU resident 1977.1–1977.4 MiB, live GPU 1763.3–1763.6 MiB, peak CPU tracked 92.7 MiB, GPU after scene release 1763.3–1763.6 MiB.
Report: GPU peak 4340.1–4340.5 MiB / live 1760.7–1761.0 MiB; CPU peak 92.7 MiB / live 92.7 MiB; reconciliation notObservable; residency unknown.
Top GPU owners (first counted run): ShadowPass 501.0 MiB, GroomPass 411.7 MiB, Unattributed 196.8 MiB, TransientPool 166.1 MiB, RendererInit 144.3 MiB, Foliage pinned representations 123.8 MiB, GBuffer 87.0 MiB, ShadowMap 80.0 MiB, Texture2D 25.3 MiB, TemporalHistory 15.8 MiB.
