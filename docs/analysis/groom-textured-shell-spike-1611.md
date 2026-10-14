# Textured groom shell experiment (#1611)

Evaluate the measured candidate against cooked strands and cards before selecting
a production tier in #1613. This experiment adds no shipping representation.

## Candidate and inputs

The input files are the committed `reference-shortcoat-animal.ologroom` (5,649
curves) and `reference-longcoat-animal.ologroom` (6,399 curves), loaded with the
production groom decoder. The card arm uses their cooked level and the shipping
coverage-growth/reach functions at each projected size. These regional coats
differ from #1252's generated 30,000/20,000-curve spherical coats; their absolute
numbers must not be substituted into the old tables.

The reference fixture's surface consists of cylindrical regions along Z, in
metres, with periodic circumferential U and bounded axial V. Region names select
charts before UV lookup: the mane and muzzle overlap the head's V band. Undercoat
and guard hair share their region. All roots must agree with the reconstructed
surface within 1 mm, and every curve must belong to exactly one chart.

The deterministic CPU bake creates a 64×32 map per chart containing maximum
normal reach, mean projected direction and contributing root count, and a
64×32×32 height texture. Empty texels receive a nearest occupied envelope and
direction, but retain zero contributing roots. The height texture stores
`1-exp(-tau)` for two measures: width times 3D fibre arc length integrated through
each normalized-height slab, and the cross-section at the slab centre. Residual
height-dependent lean is deposited in UV space, then the shader restores the mean
lean. U wraps; V is clipped at chart boundaries. No uniform synthetic alpha is
used for either reference coat.

Each chart uses a 64-sided, eight-ring proxy redrawn as 8, 12 or 16 normal-offset
shells. The fragment stage
intersects the actual shell inside a conservative raster envelope, so a hash
jitters both depth and silhouette by up to one layer interval. It uses the
production `oloGroomStochasticHash` semantics. Band samples are independent by
layer; repeated cross-sections share a pixel/chart threshold. The slab measure
is projected by `|tangent × view| / |normal · view|` and corrected for the
offset cylinder's larger area. The intersection solver works near the cylinder
axis rather than subtracting two camera-distance-squared terms.

This is a bounded approximation: maps average directions; displaced lookup can
sample a different local reach; open charts cannot represent axial overhang;
the finite shell spacing has no fins. A result describes this candidate and
these assets, rather than textured shells in general.

## Measurement contract

The probe isolates coat geometry with no opaque body or shadow volume. Strands
and cards use #1252's projection and square-ended, true-width bands, expanded to
one pixel with compensating stochastic alpha. An independent 8×8 true-width CPU
raster records the strand reference area. Coverage is summed linear occupancy;
silhouette error is mean absolute per-pixel coverage difference over a 256×256
crop containing the projected bounds.

PNG red encodes occupancy and green encodes diagnostic radiance, rather than
coat colour. Inspection sheets apply the same 8× display gain to every arm and
nearest-neighbour magnification at 32 pixels and below; raw PNGs and metrics retain
their original values.

Energy is integrated linear radiance under the same white fibre, directional
diffuse diagnostic in all arms. This isolates tangent and geometry differences;
it does not measure production Chiang shading, coat tint or self-shadowing.
The existing card rules remain: #1252's analytic coverage band is ±10%, and its
silhouette comparison permits at most 5% deterioration relative to a matched
strand budget. The later production hand-over's ±5% energy bar concerns the
documented scene/seen-energy measurements. Diagnostic energy here is reported
separately and cannot establish production shading parity.

Every arm has 64 warm-up draws and 64 retained `GL_TIME_ELAPSED` samples; medians
describe draw cost, excluding clears, CPU projection/uploads and readback. Motion
timings use the actual moving camera. Each arm then captures the same 64-frame
history. Static arithmetic integration is shown separately from native frames.
Moving histories retain per-frame error against the same-frame strand image and
the residual after subtracting its measured frame difference. Their screen-space mean is
not a production TAA reconstruction.

## Results

### Decision

**No-go for this candidate.** Neither coat establishes a shell-count/size range
within the ±10% coverage bar across the measured views and configurations.
At 128 pixels, jittered shells retain only 51.0–74.0% of short-coat coverage and
35.4–61.7% of long-coat coverage across all three configurations. The 720p/1x
card arm retains 93.1–107.2% and 97.5–101.6%, respectively. This comparison is
well separated from the full-strand reference's 128-pixel sampling error.

This rejects this bake, proxy, lean/intersection construction and 8/12/16-layer
GL raster implementation for these assets and tested ranges. It does not reject
textured shells generally or establish that fins are necessary. Keep #1613's
measured-go prerequisite: completing #1611 with a negative result must not
automatically authorize production integration. A revised candidate needs a new
successful coverage experiment before that prerequisite can be satisfied.

### Campaign tables

Measured on 2026-10-14, 13:59:25–14:00:57 CEST: Windows, clang-cl 23.1.0,
Release (`/O2 /Ob2 /DNDEBUG`), RTX 4090, OpenGL 4.6 / driver 617.42.
The monitor recorded no competing engine, game, test or build processes.
Base revision `99007908a0a66f7c5fc2e7e0ce4b5d5e677fc800` plus the experiment
inputs recorded in `inputs-sha256.json`; measurement preceded the commits.
Binary SHA-256: `023e1a469203b699b782c83ef82500127bc59a93f88bbde6e0d8db4b461ffae3`.
There are 456 validated rows per coat and 58,368 retained GPU samples in total.
Every timing/history case has unique frames 0–63. There are 62 short-coat and
eight long-coat zero-duration query samples; all case medians are positive.
Timings show approximately 1.024 µs steps, so sub-microsecond differences are
not meaningful. Earlier diagnostic/contended pilots are excluded.

#### Short coat

Ranges across front, oblique and silhouette views; 720p, 1x, 64 frames.

| px | arm | coverage / strands | energy / strands | GPU draw ms | mean abs coverage error |
|---:|---|---:|---:|---:|---:|
| 128 | strands | 1.000–1.000 | 1.000–1.000 | 0.0020–0.0041 | 0.000000–0.000000 |
| 128 | cards | 0.931–1.072 | 0.931–1.077 | 0.0010–0.0010 | 0.000576–0.000757 |
| 128 | shell8 | 0.547–0.732 | 0.545–0.718 | 0.0041–0.0061 | 0.000478–0.000597 |
| 128 | shell12 | 0.556–0.738 | 0.551–0.723 | 0.0051–0.0082 | 0.000470–0.000604 |
| 128 | shell16 | 0.542–0.740 | 0.537–0.725 | 0.0072–0.0113 | 0.000465–0.000597 |
| 64 | strands | 1.000–1.000 | 1.000–1.000 | 0.0041–0.0051 | 0.000000–0.000000 |
| 64 | cards | 0.979–1.051 | 0.985–1.066 | 0.0010–0.0020 | 0.000132–0.000196 |
| 64 | shell8 | 0.518–0.830 | 0.506–0.811 | 0.0031–0.0102 | 0.000113–0.000142 |
| 64 | shell12 | 0.506–0.826 | 0.502–0.812 | 0.0082–0.0143 | 0.000112–0.000143 |
| 64 | shell16 | 0.474–0.811 | 0.472–0.792 | 0.0113–0.0184 | 0.000111–0.000142 |
| 32 | strands | 1.000–1.000 | 1.000–1.000 | 0.0061–0.0092 | 0.000000–0.000000 |
| 32 | cards | 0.708–1.188 | 0.701–1.198 | 0.0020–0.0020 | 0.000028–0.000046 |
| 32 | shell8 | 0.519–0.679 | 0.535–0.659 | 0.0072–0.0123 | 0.000028–0.000038 |
| 32 | shell12 | 0.492–0.721 | 0.493–0.711 | 0.0113–0.0174 | 0.000030–0.000040 |
| 32 | shell16 | 0.524–0.697 | 0.504–0.696 | 0.0154–0.0236 | 0.000030–0.000040 |
| 16 | strands | 1.000–1.000 | 1.000–1.000 | 0.0072–0.0102 | 0.000000–0.000000 |
| 16 | cards | 1.217–1.515 | 1.184–1.468 | 0.0020–0.0020 | 0.000009–0.000013 |
| 16 | shell8 | 0.667–0.783 | 0.661–0.801 | 0.0082–0.0102 | 0.000008–0.000010 |
| 16 | shell12 | 0.538–0.717 | 0.535–0.753 | 0.0143–0.0174 | 0.000008–0.000009 |
| 16 | shell16 | 0.359–0.606 | 0.322–0.612 | 0.0195–0.0246 | 0.000007–0.000008 |
| 8 | strands | 1.000–1.000 | 1.000–1.000 | 0.0092–0.0092 | 0.000000–0.000000 |
| 8 | cards | 0.875–2.667 | 0.910–2.567 | 0.0020–0.0020 | 0.000001–0.000003 |
| 8 | shell8 | 0.333–0.875 | 0.355–0.963 | 0.0102–0.0102 | 0.000001–0.000002 |
| 8 | shell12 | 0.444–0.750 | 0.475–0.808 | 0.0164–0.0174 | 0.000001–0.000002 |
| 8 | shell16 | 0.333–0.667 | 0.350–0.684 | 0.0225–0.0236 | 0.000001–0.000002 |
| 4 | strands | 1.000–1.000 | 1.000–1.000 | 0.0082–0.0092 | 0.000000–0.000000 |
| 4 | cards | 0.600–1.333 | 0.587–1.151 | 0.0020–0.0020 | 0.000001–0.000001 |
| 4 | shell8 | 0.000–0.000 | 0.000–0.000 | 0.0102–0.0102 | 0.000000–0.000001 |
| 4 | shell12 | 0.000–0.333 | 0.000–0.291 | 0.0164–0.0174 | 0.000000–0.000001 |
| 4 | shell16 | 0.000–0.333 | 0.000–0.294 | 0.0236–0.0236 | 0.000000–0.000001 |

#### Long coat

Ranges across front, oblique and silhouette views; 720p, 1x, 64 frames.

| px | arm | coverage / strands | energy / strands | GPU draw ms | mean abs coverage error |
|---:|---|---:|---:|---:|---:|
| 128 | strands | 1.000–1.000 | 1.000–1.000 | 0.0061–0.0225 | 0.000000–0.000000 |
| 128 | cards | 0.975–1.016 | 0.977–1.017 | 0.0010–0.0031 | 0.003102–0.003340 |
| 128 | shell8 | 0.365–0.600 | 0.361–0.605 | 0.0082–0.0205 | 0.002423–0.003123 |
| 128 | shell12 | 0.409–0.612 | 0.406–0.616 | 0.0092–0.0184 | 0.002393–0.002948 |
| 128 | shell16 | 0.435–0.617 | 0.432–0.622 | 0.0123–0.0256 | 0.002399–0.002881 |
| 64 | strands | 1.000–1.000 | 1.000–1.000 | 0.0082–0.0082 | 0.000000–0.000000 |
| 64 | cards | 0.974–1.013 | 0.971–1.016 | 0.0010–0.0020 | 0.000589–0.000645 |
| 64 | shell8 | 0.353–0.594 | 0.350–0.599 | 0.0041–0.0102 | 0.000606–0.000783 |
| 64 | shell12 | 0.377–0.593 | 0.373–0.595 | 0.0072–0.0138 | 0.000620–0.000738 |
| 64 | shell16 | 0.413–0.595 | 0.409–0.597 | 0.0092–0.0164 | 0.000626–0.000726 |
| 32 | strands | 1.000–1.000 | 1.000–1.000 | 0.0113–0.0184 | 0.000000–0.000000 |
| 32 | cards | 0.958–1.032 | 0.964–1.043 | 0.0020–0.0041 | 0.000111–0.000136 |
| 32 | shell8 | 0.302–0.594 | 0.300–0.606 | 0.0061–0.0123 | 0.000152–0.000206 |
| 32 | shell12 | 0.347–0.583 | 0.340–0.597 | 0.0092–0.0164 | 0.000152–0.000197 |
| 32 | shell16 | 0.378–0.600 | 0.375–0.614 | 0.0133–0.0220 | 0.000142–0.000190 |
| 16 | strands | 1.000–1.000 | 1.000–1.000 | 0.0174–0.0215 | 0.000000–0.000000 |
| 16 | cards | 0.966–1.085 | 0.971–1.071 | 0.0031–0.0041 | 0.000024–0.000035 |
| 16 | shell8 | 0.358–0.587 | 0.357–0.594 | 0.0102–0.0123 | 0.000034–0.000052 |
| 16 | shell12 | 0.448–0.587 | 0.444–0.595 | 0.0174–0.0205 | 0.000034–0.000043 |
| 16 | shell16 | 0.398–0.601 | 0.401–0.605 | 0.0236–0.0287 | 0.000036–0.000043 |
| 8 | strands | 1.000–1.000 | 1.000–1.000 | 0.0164–0.0184 | 0.000000–0.000000 |
| 8 | cards | 0.864–1.211 | 0.898–1.200 | 0.0020–0.0020 | 0.000007–0.000011 |
| 8 | shell8 | 0.407–0.509 | 0.422–0.522 | 0.0102–0.0123 | 0.000010–0.000012 |
| 8 | shell12 | 0.526–0.600 | 0.539–0.636 | 0.0174–0.0215 | 0.000009–0.000010 |
| 8 | shell16 | 0.519–0.614 | 0.533–0.641 | 0.0236–0.0287 | 0.000008–0.000010 |
| 4 | strands | 1.000–1.000 | 1.000–1.000 | 0.0184–0.0205 | 0.000000–0.000000 |
| 4 | cards | 0.708–1.231 | 0.674–1.179 | 0.0020–0.0020 | 0.000002–0.000003 |
| 4 | shell8 | 0.038–0.500 | 0.043–0.468 | 0.0123–0.0133 | 0.000003–0.000006 |
| 4 | shell12 | 0.190–0.458 | 0.147–0.433 | 0.0195–0.0236 | 0.000003–0.000005 |
| 4 | shell16 | 0.038–0.542 | 0.043–0.513 | 0.0276–0.0307 | 0.000003–0.000006 |

#### Configuration comparison

Ranges across all three jittered shell counts and views, at 128 pixels.
Actual multisample support is four samples for the 4x target; the ordinary
1x texture reports `GL_TEXTURE_SAMPLES=0`, as expected for a non-MS texture.

| coat | configuration | coverage / strands | diagnostic energy / strands | GPU draw ms |
|---|---|---:|---:|---:|
| short | 720p 1x | 0.542–0.740 | 0.537–0.725 | 0.0041–0.0113 |
| short | 720p 4x | 0.529–0.728 | 0.523–0.715 | 0.0266–0.0870 |
| short | 1080p 1x | 0.510–0.714 | 0.512–0.709 | 0.0133–0.0297 |
| long | 720p 1x | 0.365–0.617 | 0.361–0.622 | 0.0082–0.0256 |
| long | 720p 4x | 0.364–0.616 | 0.361–0.620 | 0.0276–0.1014 |
| long | 1080p 1x | 0.354–0.595 | 0.350–0.602 | 0.0041–0.0164 |

#### Camera motion and jitter

32-pixel camera sweep; ranges across three views. Error and residual are
integrated occupancy pixels per frame (CSV crop means multiplied by 65,536).
Residual subtracts the corresponding full-strand frame difference and still
contains alpha sampling noise. Less occupied coat can also produce less
absolute error; these values alone do not rank temporal quality.

| coat | arm | coverage / strands | native error px/frame | motion residual px/frame | GPU draw ms |
|---|---|---:|---:|---:|---:|
| short | cards | 0.840–1.147 | 4.45–5.45 | 8.54–10.43 | 0.0031–0.0041 |
| short | shell8_fixed | 0.568–0.682 | 3.52–4.19 | 6.84–8.03 | 0.0072–0.0113 |
| short | shell8_jitter | 0.550–0.682 | 3.48–4.06 | 6.71–7.83 | 0.0072–0.0123 |
| short | shell12_fixed | 0.533–0.700 | 3.52–4.30 | 6.73–8.38 | 0.0123–0.0159 |
| short | shell12_jitter | 0.533–0.706 | 3.59–4.31 | 6.90–8.43 | 0.0133–0.0164 |
| short | shell16_fixed | 0.541–0.712 | 3.52–4.17 | 6.75–8.11 | 0.0174–0.0215 |
| short | shell16_jitter | 0.534–0.718 | 3.50–4.22 | 6.70–8.22 | 0.0184–0.0236 |
| long | cards | 0.979–1.032 | 29.72–31.02 | 53.21–55.40 | 0.0041–0.0072 |
| long | shell8_fixed | 0.309–0.589 | 21.62–24.42 | 38.97–44.65 | 0.0072–0.0143 |
| long | shell8_jitter | 0.308–0.598 | 21.52–25.05 | 38.95–45.86 | 0.0061–0.0133 |
| long | shell12_fixed | 0.374–0.590 | 21.53–25.03 | 38.94–45.90 | 0.0102–0.0174 |
| long | shell12_jitter | 0.380–0.573 | 21.52–24.84 | 38.97–45.54 | 0.0113–0.0179 |
| long | shell16_fixed | 0.403–0.605 | 22.14–25.58 | 39.92–46.94 | 0.0154–0.0256 |
| long | shell16_jitter | 0.387–0.600 | 21.67–25.50 | 39.29–46.78 | 0.0154–0.0256 |

Height jitter changes individual samples but supplies no consistent recovery
of missing coverage. Native frames and moving sequences remain visibly
sparse; arithmetic means recover an average signal, not the clipped fringe.
The static and moving PNGs were inspected, including 128/32/8/4-pixel
three-view contact sheets and native/mean motion frames 0/32/56.
There are zero short-coat and one long-coat shell cells within ±10% out of
342 shell comparisons per coat. The lone cell is 1080p, 4 pixels, oblique,
12 fixed layers; it does not form an all-view/configuration range.
No production TAA or production energy acceptance is claimed.

Some shell draws beat full strands, but the missing coverage prevents a tier
selection on cost. These are isolated proxy draw costs, not production GroomPass,
herd, shadow or map-cook costs.

### Image and temporal inspection

The 128- and 32-pixel contact sheets show a thinner, clipped fringe in the shell
arms, especially the long coat; 16 layers do not restore the missing outline.
The card arm's long-coat 128-pixel image has visible broad spokes despite its
area agreement. Matching integrated area therefore does not establish matching
silhouette. The supplied coverage MAE compares each arm to full strands;
there is no invented shell/card MAE acceptance threshold.

At 4–8 pixels the short coat becomes isolated stochastic hits. Reference area
SE is 31–70% of its measured mean, compared with 1.8–2.1% at 128 pixels. The long
coat's corresponding SE is 11–22%, compared with 0.67–0.75% at 128 pixels. A
single near-unity low-pixel ratio cannot establish a robust hand-over. The
64-frame histories are inadequate to resolve a ±10% claim in those sparse cells.

The independent CPU 8×8 raster and stochastic strand area at 128 pixels agree
within 5.1% for the short coat and 1.6% for the long coat across the three views.
At 4 pixels the short-coat silhouette view instead measures 0.015625 versus
0.078125 pixels (SE 0.0338112). This bridge is measured, not an asserted proof of
continuous-area accuracy at every size; both finite sampling and the CPU raster's
subpixel quantization matter there.

The radial, tilted and bent GPU instrument controls measure three views.
Projected-area agreement and the check against halving opacity with layer count
are asserted at angle 2 only, with sampling uncertainty and voxel error allowed.
The other views are measured silhouette controls, not coat acceptance tests.

### Local verification

`gpu.xml` records eight passing tests: both complete coat comparisons, both GPU
instrument/state contracts, the distant-camera intersection contract and three
existing strand GPU parity tests. `cpu.xml` records 96 passes covering the four
new bake contracts plus nearby cook, coverage, LOD and binding contracts; 13
existing binding visual cases intentionally skip with `--olo-gl-backend=none`.
The Release build and all pre-commit checks pass.

The offscreen experiment is reachable only in the GL fixture. Vulkan and all
six production GL/Vulkan × Forward/Forward+/Deferred cells are unreachable for
this renderer. FSR2/upscale is also unreachable; native and arithmetic-mean
outputs above are the tested alternatives. These cells are not claimed as
production passes.

## Reproduce and evidence

Build `OloEngine-Tests` in Release through the repository build lock, then run
`GroomShellSpikeBake.*`, `GroomShellSpikeGpuContract.*`,
`GroomShellSpikeIntersection.*`, and `GroomShellSpike*Evidence.*` with a real GL
4.6 context. Headless skips do not provide GPU evidence. Use
`tools/groom-shell-spike/GroomShellSpikeReport.py .v-build/1611` to validate the
complete campaign and generate tables and an evidence hash list. Add
`--contact-sheets` to generate inspection sheets with Pillow.

The raw maps, PNGs, per-frame sequences, timings, XML, hardware records and input
hashes stay in ignored `.v-build/1611/`. The source tests and this analysis are
the reproducible review surface.

The old [opaque-shell result](groom-representation-lod-1252.md) remains valid:
its coverage-one surface is a different representation. The new experiment
measures alpha-tested, mostly empty layers, so it does not repeat that refusal's
premise. The construction follows geometric height filtering in
[Lengyel et al.](https://hhoppe.com/fur.pdf); silhouette limitations of shells are
also described in [NVIDIA's fur whitepaper](https://developer.download.nvidia.com/SDK/10.5/direct3d/Source/Fur/doc/FurShellsAndFins.pdf).
Hashing uses the existing engine helper; see
[Wyman and McGuire](https://research.nvidia.com/labs/rtr/publication/wyman2017hashed/)
for the distinction between alpha thresholds and fractional coverage.
