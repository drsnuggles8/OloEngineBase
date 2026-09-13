# Reference fixtures: head, groomed animals, vegetation (issue #1239)

Five deterministic benchmark fixtures that record what characters and flora look like and cost
**today**, so epic #1225's quality work has a baseline to be measured against. They extend the
issue-#974 harness ([renderer-benchmarks.md](renderer-benchmarks.md)); everything there still
applies.

**The rule that governs every change to these fixtures: a fixture records a limitation, it does
not hide one.** The head has no subsurface scattering, the long coat is a solid shell, the meadow
is alpha-masked cards. Improving any of those *here* destroys the baseline the feature owner is
measured against. Upgrade the engine, re-capture, and let the numbers move.

## Capturing one

```powershell
$m = "OloEditor/assets/benchmark/manifests/reference-head.diagnostic.yaml"
build-cached\OloEngine\tests\Debug\OloEngine-Tests.exe --olo-capture-manifest=$m
```

Results land in `OloEditor/assets/benchmark/captures/<Id>/` (git-ignored), one subdirectory per
camera plus `result.json`. The editor front door (`olo_benchmark_capture`) runs the same manifests
under `--rhi=vulkan`.

| Fixture | Manifest `Id` | Subject | Records the absence of |
|---|---|---|---|
| `ReferenceHead.olo` | `reference-head` | Suzanne + DamagedHelmet | subsurface scattering, eye shading, groom |
| `AnimalShortCoat.olo` | `animal-short-coat` | Khronos Fox, Walk clip | — this one is a real short coat |
| `AnimalLongCoat.olo` | `animal-long-coat` | procedural quadruped | strand geometry, anisotropic specular, coat motion |
| `Meadow.olo` | `meadow` | grass/flower billboards | foliage geometry, leaf translucency |
| `Woodland.olo` | `woodland` | pine/palm canopy + impostors | leaf detail, canopy translucency |

Scenes are generated: `OloEngine/tests/scripts/generate_reference_fixture_scenes.py`. Edit the
generator, never the `.olo`.

## The four sequences

Every fixture manifest declares the same four cameras, by name, and
`ReferenceFixtureManifest.EveryFixtureDeclaresFourSequences` fails if one goes missing:

- **`frontal`** — conventionally lit, straight on. Pays the full `Warmup.Frames`.
- **`grazing`** — view near-tangent to the surface, light raking. Where specular models and
  missing sheen terms show up.
- **`backlit`** — camera into the key light, where a missing transmission term is unmissable:
  real skin, fur and leaves glow, opaque geometry gives a hard silhouette.
- **`moving`** — the camera advances *between* frames (below). The only sequence that populates
  the velocity buffer, or shows impostor pops and alpha-test crawl.

A later camera is a cut, re-warmed over its own `WarmupFrames` on a fixed schedule.

## Manifest schema v2

`ManifestVersion: 2` adds exactly two things. A v1 manifest is **not** silently upgraded — the v2
provenance fields are required, so an implicit upgrade would turn every issue-#974 manifest into a
parse error at an unrelated moment.

### Camera motion

```yaml
- Id: moving
  Position: [0.0, 6.40, 16.0]
  YawDegrees: 0.0
  WarmupFrames: 32
  Motion:
    VelocityPerSecond: [0.0, 0.0, -6.0]   # world metres / second
    YawRateDegreesPerSecond: 0.0
    PitchRateDegreesPerSecond: 0.0
```

Rates are **per second**, integrated against `Determinism.FixedDtSeconds`, so halving the dt and
doubling the frame count lands on the same pose. The pose is a **closed form** of the frame index
(`BenchmarkManifest.cpp::CameraPoseAtFrame`), not an accumulation — that is what lets the test
binary (stepping a mock clock) and the editor host (counting live frames) trace the same path
without drifting apart. The editor host re-poses one frame at a time when `Motion` is present.

The capture is the **last** warm-up frame, so the recorded pose is the integrated one at
`WarmupFrames - 1`; `result.json` has it as `cameras[].capturedPose`. Read that, not `Position`.

An all-zero `Motion` block is a hard parse error — it would advertise a moving sequence and
produce a still one — and so is a malformed number, which `as<T>(fallback)` would read as zero.

### Asset provenance

Every `Assets:` record carries the full block, and all of it is required in v2:

| Field | Vocabulary | Why it is here |
|---|---|---|
| `Redistribution` | `committed` / `fetch-required` / `local-only` | decides whether bytes may live in a public repo |
| `LicenseVerified` | `in-repo-file` / `upstream-declared` / `unverified` | "nobody checked" is a statable answer, not an optimistic string |
| `Version` | free text, or `generated` | which upstream revision this is |
| `Sha256` | 64 lowercase hex | makes the acquisition path *checkable*, not merely described |
| `Units` | `metres` / `centimetres` / `unitless` | a 100x mis-scale invalidates every screen-coverage number |
| `UpAxis` | `+Y` / `+Z` / `n/a` | |
| `ColorSpace` | `srgb` / `linear` / `n/a` | per file — sampling metal/roughness as sRGB is a material bug that reads as a lighting bug |
| `Acquisition` | URL, or the procedure | the reproducible local acquisition path |

Cross-field rule: `LicenseVerified: in-repo-file` requires `Redistribution: committed`. There is no
in-repo licence file for an asset this repo does not ship.

```powershell
python tools/benchmark/reference_assets.py                # verify every hash
python tools/benchmark/reference_assets.py --write-hashes # after a deliberate asset change
python tools/benchmark/reference_assets.py --fetch        # pull fetch-required assets
```

`--write-hashes` is the only way a hash should ever move. Review that diff: a hash that changed
without an intended asset change is the bug the mechanism exists to catch.

### The licensed AAA head is a declared gap

A photoreal scanned human head with skin, eye and groom maps is what "licensed AAA head" means,
and **none is obtainable without accepting a third-party licence**, so this repository ships none.
Rather than substitute something and call the criterion met, the gap is a `local-only` record in
`reference-head.diagnostic.yaml` with the procedure for filling it; `reference_assets.py` reports
it as a gap, not a failure. The rig is built so a licensed head dropped at the declared path
changes the subject and nothing else.

The stand-ins are honest about what they measure: Suzanne gives organic head *shape* response on
untextured geometry, DamagedHelmet the same rig against real 2K PBR texture detail. Neither is
skin.

## Determinism proof

```powershell
$exe = "build-cached\OloEngine\tests\Debug\OloEngine-Tests.exe"
& $exe --olo-capture-manifest=$m --olo-capture-out=captures/a
& $exe --olo-capture-manifest=$m --olo-capture-out=captures/b
python tools/benchmark/compare_captures.py captures/a captures/b
```

It prints per-attachment RMSE in 0..255 units (what `Tolerance.RepeatRmse` and the golden
machinery both use) and exits non-zero if a PNG exceeds the tolerance in `result.json`. HDR
attachments are compared in float radiance and reported separately — clamping them first would
hide the out-of-range differences an HDR export exists to preserve.

## Measured baseline

The numbers — determinism, per-pass GPU cost, proposed gates, and the findings the fixtures have
already produced — live in
[benchmark-reference-fixtures-baseline.md](benchmark-reference-fixtures-baseline.md). They are
split out because a measurement record grows every time the fixtures are re-run on a new
configuration, and this guide is the contract, not the log.
