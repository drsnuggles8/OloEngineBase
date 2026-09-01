# Renderer benchmark scenes, capture manifests, and hero frames (issue #974)

Deterministic benchmark scenes + a manifest-driven capture entry point that answers the
questions the regression suite cannot: *is this frame good, which term is responsible, and
what does it cost?* This is a different product from the golden-image suite (which measures
change, not quality) and from the perf baselines (which measure cost, not quality).

## The two capture products — and why they can never mix

| | Regression goldens | Hero captures |
|---|---|---|
| Purpose | CI stability — "did it change?" | README / release presentation — "is it good?" |
| Resolution | modest (1280×720) | full (up to 3840×2160) |
| Tolerance | thresholded, documented per scene | none — judged by eyes |
| Update policy | `--olo-golden-rebase` after a deliberate visual change | re-captured whenever presentation improves |
| Lives at | `OloEditor/assets/tests/{golden,visual}/` (existing machinery) | `docs/images/benchmarks/` (committed) |

The split is **structural, not conventional**: the golden compare machinery only reads
`assets/tests/golden/` and `assets/tests/visual/`, and hero frames live under `docs/`,
which no test sweeps. A hero capture cannot silently become a regression golden because no
code path connects the two directories. Benchmark capture *result* directories are
git-ignored (`OloEditor/assets/benchmark/captures/`).

The committed hero gallery ([docs/images/benchmarks/](../images/benchmarks/)) stores each
frame as a quality-92 JPEG — the 4K PNG originals run to 22 MB, and the lossless frame is
always reproducible by running the scene's `*.hero.yaml` manifest:
[material-lab-hero.jpg](../images/benchmarks/material-lab-hero.jpg) ·
[courtyard-hero.jpg](../images/benchmarks/courtyard-hero.jpg) ·
[ocean-coast-hero.jpg](../images/benchmarks/ocean-coast-hero.jpg)

## Directory layout

```
OloEditor/SandboxProject/Assets/Scenes/Benchmark/   # the three scenes (.olo, committed)
  MaterialLab.olo        # material laboratory
  Courtyard.olo          # dense architectural / foliage
  OceanCoast.olo         # ocean / coast hero
OloEditor/assets/benchmark/
  manifests/             # versioned capture manifests (.yaml, committed)
  captures/              # result directories (GIT-IGNORED)
docs/images/benchmarks/  # committed hero frames
```

Scenes live under `SandboxProject/Assets/Scenes/` so the existing sweeps cover them
automatically (`AssetSceneLoad.AllSandboxScenesDeserialiseThroughEditorAssetManager`, the
four `AssetContentValidity` structural tests). Manifests live under `OloEditor/assets/`
(outside the project asset root) so they need no `AssetRegistry.oar` entry.

Scenes reference assets **by path only, never by `AssetHandle`** — a non-zero handle absent
from the binary `AssetRegistry.oar` fails `AssetContentValidity`, and the only sanctioned
way to add registry entries is an editor launch. Path spelling: `Assets/...`
(project-relative) for SandboxProject assets, `assets/...` (CWD-relative to `OloEditor/`)
for editor-tree assets.

## Capture manifest schema (v1)

One YAML file per capture product. Versioned from day one; the parser rejects an unknown
`ManifestVersion` or unknown top-level key rather than shrugging.

```yaml
ManifestVersion: 1
Id: ocean-coast-hero            # result dir name; [a-z0-9-]
Product: hero                   # golden | diagnostic | hero
Scene: Scenes/Benchmark/OceanCoast.olo    # project-relative

Backends:
  Supported: [opengl, vulkan]
  # Backend-specific features are DECLARED, never silently skipped:
  Features:
    vulkan:
      UnsupportedAttachments: []          # attachment Names expected to be absent

Camera:                          # explicit editor-camera pose (not the scene camera)
  Id: hero
  Position: [x, y, z]
  YawDegrees: 0.0                # EditorCamera::SetPose convention: yaw 0 = -Z,
  PitchDegrees: 0.0              # +90 = +X; positive pitch tilts DOWN
  FovDegrees: 60.0
  Near: 0.05
  Far: 1000.0
# ... or a camera LIST — each later camera is a deterministic camera CUT,
# re-warmed by its own WarmupFrames (default: the manifest's Warmup.Frames):
# Cameras:
#   - { Id: atrium, Position: [...], ... }            # full warm-up
#   - { Id: vista,  Position: [...], WarmupFrames: 32 }  # post-cut settle

Output:
  Resolution: [3840, 2160]
  RenderScale: 1.0

RendererSettings:                # renderer-side state the scene CANNOT serialize
  Path: Deferred                 # Forward | ForwardPlus | Deferred
  EnableDDGI: true
  TAAEnabled: true               # lives in PostProcessSettings but is not
                                 # scene-serialized, so it is pinned here
  # also: DepthPrepassEnabled, OcclusionCullingEnabled, HZBOcclusionCullingEnabled
  # (unknown keys are fatal — extend BenchmarkManifest.cpp to add one)

Exposure:
  Mode: Manual                   # Manual | Auto (Auto = the convergence capture)
  Exposure: 1.0

Determinism:
  Seed: 974                      # Application random seed (game-thread RNG stream)
  StartTimeSeconds: 12.0         # mock-clock t0 (Time::SetMockTime before scene create)
  FixedDtSeconds: 0.016666666    # the mock clock is STEPPED by this each frame

Warmup:
  Frames: 128                    # effective warm-up; must be >= max(PerFeature)
  PerFeature:                    # self-description: why Frames is what it is
    TAA: 8
    SSGI: 16
    Exposure: 30
    Ocean: 64
    DDGI: 128

Attachments:                     # Beauty first, then every AOV this scene wants
  - Name: Beauty
    Source: UIComposite          # a render-graph resource name (ResourceNames::*)
    Format: png
  - Name: SceneColorHDR
    Source: SceneColor
    Format: hdr                  # Radiance .hdr — full float, no [0,1] clamp
  - Name: Normals
    Source: GBufferNormal
    Format: png
    Normalize: none              # auto | none  (auto normalizes depth-like sources)

Tolerance:
  RepeatRmse: 0.0                # documented run-twice tolerance (0 = byte-identical)

Assets:                          # origin + license of every referenced asset
  - Path: SandboxProject/Assets/Models/KenneyVehicles/ship-small-hull.glb
    Origin: Kenney (kenney.nl)
    License: CC0-1.0
```

## The capture entry point

**CLI:** `OloEngine-Tests.exe --olo-capture-manifest=<path> [--olo-capture-out=<dir>]` —
a `GTEST_SKIP`-unless-flagged tool-mode case (`BenchmarkCaptureTest`), the same pattern as
`--olo-bake-shader-pack`. Requires a GL 4.6 context (skips cleanly headless).

**MCP:** `olo_benchmark_capture` runs the same core inside a live editor — this is the
route that runs manifests under `--rhi=vulkan`, since the test binary's headless context
is GL-only.

Both fronts share one core (`OloEngine/src/OloEngine/Renderer/Benchmark/`):

1. parse + validate the manifest;
2. apply `Determinism` (seed, `Time::SetMockTime(t0)`) **before** the scene loads;
3. load the scene through the editor asset manager;
4. apply `RendererSettings`, `Exposure`, `Output` (render scale forced via RAII);
5. warm `Warmup.Frames` frames, stepping the mock clock by `FixedDtSeconds` each frame,
   with the manifest camera posed from frame 0;
6. capture every attachment at the final frame (native resolution, no downscale) through
   the backend-neutral readback (`RenderCommand::ReadTextureSubImage` /
   `GPUResourceInspector`);
7. write the self-describing result directory.

### Result directory contract

```
OloEditor/assets/benchmark/captures/<Id>/
  manifest.yaml       # verbatim input echo
  result.json         # everything below
  Beauty.png
  <AttachmentName>.png|.hdr
```

`result.json` records: schema version, manifest SHA-256, commit SHA, backend, GPU/driver
strings, machine tag (same key as the perf history), resolution + render scale, the
applied renderer/post-process settings, warm-up frame count and capture frame index, the
per-pass GPU/CPU timings (`GPUPassTimerPool`), renderer memory/stat counters, and per
attachment: file name, source resource, format, dimensions, min/max values.

## Determinism contract

The engine clock is mocked and **stepped**: `Time::SetMockTime(t0 + n·dt)` each frame.
Every subsystem that reads `Time::GetTime()` (scene animation clock → water/ocean/foliage
phase, cloud advection, wake decay) therefore advances identically run to run. A capture
is a **fresh process** (or a fresh renderer session) rendering a fixed number of frames,
so the free-running frame counters (stochastic index, TAA jitter, fog/cloud indices, DDGI
frame index) hold identical values at the capture frame without new reset plumbing.

Sources that read `std::chrono::steady_clock` directly bypass the mock clock and are
converted to `Time::GetTime()` by this work (wind — which drives foliage sway, snow and
precipitation dt; fog noise time; auto-exposure adaptation dt). FSR2 deliberately keeps
real time and stays out of benchmark manifests (`Upscale: Off`).

Known GPU-order nondeterminism (documented, avoided by scene design rather than fixed):
the PBF fluid solver, virtual-geometry software-raster depth ties, Forward+ light-cull
cluster offsets. Benchmark scenes contain no fluid; the dense scene runs Deferred. The
manifest's `Tolerance.RepeatRmse` documents the residual; the acceptance proof is running
the capture twice and diffing against that documented tolerance.

Camera cuts are deterministic the same way: a cut is a camera re-pose at a fixed frame
index, and histories re-converge over a fixed warm-up — same schedule, same result.

## AOV availability (as of this branch)

Any resource registered in the render graph is capturable by name. Available today:
G-Buffer albedo/normal/emissive/velocity/baked-GI (deferred), scene color/depth/normals,
AO, SSGI/SSR signals + resolves, shadow maps (CSM cascades, atlas, raw depth views),
the whole post chain (bloom, TAA, tonemap, composite), cloud buffers.

**Derived attachments** cover the AOVs that live as *lanes* of existing targets or need a
transform, without adding render-graph passes or touching the shipping frame: the capture
core applies `Derive:` CPU-side to the exact texels it read back —
`linear-depth` (hardware depth → view-space metres, using the manifest camera's planes;
export as `.hdr` for real metres), `channel-r/g/b/a` (extract one lane as a grayscale
image — roughness is `GBufferNormal` channel-b, metallic is `GBufferAlbedo` channel-a).
HDR (pre-tonemap) export ships as Radiance `.hdr`.

Two PNG-encoding facts worth knowing when reading captures: a 4-channel source whose
alpha is a *data* lane (GBufferAlbedo carries metallic in A) previews as transparency in
an image viewer — the bytes are faithful, use the `Derive:` extraction for a readable
image of that lane; and PNG clamps float sources to [0,1] unless normalized — real values
live in the `.hdr` exports and the per-attachment min/max in `result.json`.

**Declared unavailable** (their implementation lives inside `DeferredLightingShared.glsl` /
`PBRCommon.glsl`, owned by the in-flight G-Buffer flags-lane branch #996 — coordination
noted, not silently skipped): direct vs indirect diffuse/specular splits, per-pixel shadow
visibility, reflection confidence/hit-distance. When #996 lands, these become candidates
for real debug-only taps following the `OverdrawRenderPass` / `VolumetricShadowVolume`
precedents (enable gates hashed into the frame-graph fingerprint).
