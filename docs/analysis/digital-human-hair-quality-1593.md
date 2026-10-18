# Inspect the authored bob in native frames and motion

Review the hairline, part, locks and highlights in the images below before deciding
whether #1593 meets the programme's visual bar. Binding, coverage and finite-value
contracts cannot decide that. The authored bob replaces the old face-covering veil
with swept scalp curves, a visible centre part and jaw-length locks above the shoulders.
Its highlights retain the physical R/TT/TRT lobes. This is an asset/look-development
change; shared shaders, the scan, rig, skin profile and thickness map are unchanged.

| Original shipping asset | Authored asset, same native GL fixture |
|---|---|
| ![Original front](../testing/evidence/hair1593/baseline-front.webp) | ![Authored front](../testing/evidence/hair1593/GL-front.webp) |
| ![Original crown](../testing/evidence/hair1593/baseline-crown.webp) | ![Authored crown](../testing/evidence/hair1593/GL-crown.webp) |
| ![Original profile](../testing/evidence/hair1593/baseline-profile.webp) | ![Authored profile](../testing/evidence/hair1593/GL-profile.webp) |

The native still A/B holds pose, camera, lights and rendering settings constant.
The baseline uses its shipping fibre/simulation settings; the authored side uses
the final settings, so this compares the delivered packages, not geometry alone.
[Three-quarter](../testing/evidence/hair1593/GL-threequarter.webp),
[backlit](../testing/evidence/hair1593/GL-backlit.webp),
[Vulkan crown](../testing/evidence/hair1593/VK-deferred-crown.webp) and
[Vulkan hybrid](../testing/evidence/hair1593/VK-hybrid.webp) expose other views.

The scalp is populated through the turns and nod; the face stays readable and the
tips separate above the shoulders. Feathered roots and
short regrowth tighten the part through its front end, while varied individual
tips break up the lock ends. The side body retains ordered highlight bands,
which remain the main aspect for a visual second opinion. This report
does not declare film-quality acceptance from strand counts or passing tests.
Closed eyes and oral geometry remain #1402; the wider acceptance gate remains #1259.

## Authoring and look development

The reference and reproduction commands live with the
[human source](../../OloEditor/SandboxProject/Assets/Grooms/HumanSource/README.md).
Python 3.13.7 / Blender 5.2.2 LTS authors roots on the actual glTF's triangles,
interpolated UVs, a frontal hairline, ear clearance and a narrow part. Voronoi locks
follow the scalp into tangent-continuous hanging sections with curved, varied tips.
Non-root controls are projected out of the scanned surface; the ellipsoid supplies
styling direction only. Root diameters are tapered 65–95 micrometres.

The final recipe contains 123,974 strands, 32 arc-length-sampled controls per strand,
1,149 source guides and 4,838 cooked cards. The long locks include shorter
regrowth combed beneath the part, plus finer emergence hairs along the front
boundary; guide caps remain 512 long-hair and 128 undercoat guides. Repeating authoring reproduced the entire recipe
and curve digest. Alembic metadata can vary; the curve digest is the geometry oracle.
The real Alembic importer/cooker/binding builder produced the installed pair, with
no import warnings. Existing registry handles are preserved. SHA-256 identities
and byte sizes are in [verification.json](../testing/evidence/hair1593/verification.json).

Fibre settings are eumelanin 0.5, pheomelanin 0.1, longitudinal roughness 0.18,
azimuthal roughness 0.22 and intensity 1. The old intensity 3 amplified a broad
uncoloured sheen. The R lobe remains present; no brown tint or lobe removal was
added to shared shading. Separate [R](../testing/evidence/hair1593/GL-lobe-1.webp),
[TT](../testing/evidence/hair1593/GL-lobe-2.webp),
[TRT](../testing/evidence/hair1593/GL-lobe-3.webp) and
[multiple-scattering](../testing/evidence/hair1593/GL-lobe-6.webp) captures are retained.

An early static groom looked covered but sagged through the scalp in runtime.
The final source has 6 mm crown volume and group stiffness scale one. Scene
stiffness 2000, damping 12 and gravity `[0, -4, 0]` retain full simulation and
collision within the solver's supported ranges. See the
[runtime clearance lesson](../agent-rules/groom-runtime-scalp-clearance.md).
Only HumanCoat changes in GroomAnimals and generated Acceptance; the acceptance
manifests carry the regenerated scene digest. No dog/breed sources were regenerated.

## Executed matrix

Captured 2026-10-11/12 on Windows 11 Pro 10.0.22631, i7-14700KF, 64 GiB RAM,
RTX 4090 (24,564 MiB), driver 617.14. Fresh `dev-cached-core` clang-cl Release
build, USD disabled, Vulkan SDK 1.4.357.0. Every build used the build-lock wrapper.

| Backend/path | Native views and motion | Resolution, TAA, samples | Upscaling | Distance handover |
|---|---|---|---|---|
| GL Forward | Native fixture + live, full clip | 1920×1080 / 1601×901, on/off, actual 1 | FSR1 and FSR2 Quality | Near → cards → far fallback → near |
| GL Forward+ | Same | Same, actual 1 | FSR1 and FSR2 Quality | Same |
| GL Deferred | Same | Both sizes, on/off, actual 1/4 | Dynamic scale unsupported, #1537 | Same |
| Vulkan Forward | Live native, full clip | Both sizes, on/off, actual 1 | FSR1; FSR2 request resolves to FSR1 | Same |
| Vulkan Forward+ | Same | Same, actual 1 | Same | Same |
| Vulkan Deferred | Same | Both sizes, on/off, actual 1/4 | Dynamic scale unsupported, #1537 | Same |
| Vulkan Deferred hybrid RT | Native still + full clip, active proxy/TLAS | Both sizes, configured on/off, actual 1/4 | Dynamic scale unsupported, #1537 | Near/card/far/near, active detailed/coarse proxy |

MSAA 4 was requested in every raster path. The live scene band stays single-sample
in Forward/Forward+; only Deferred reports four samples. Vulkan FSR2 reports
`backendNotOpenGL`, spatial fallback at the requested scale. These are support
results, not successful runs of the requested unsupported modes. Odd native
Deferred resizing was tested independently of dynamic internal scale.

TAA on/off in the table names the configured flag. Shipping StochasticAlpha
requests a scene temporal resolve even when `TAAEnabled` is unticked (#1429).
Additional captures on all six raster paths and hybrid explicitly ignore that
request to exercise true no-resolve, then restore it. The declared fallback loses
sub-pixel hair, so it is not a supported quality mode for this groom. Captures
and resolved settings distinguish that fallback from configured-off with the
scene's resolve honoured.
[GL no-resolve](../testing/evidence/hair1593/GL-no-resolve-fallback.webp) and
[Vulkan no-resolve](../testing/evidence/hair1593/VK-no-resolve-fallback.webp) show
the loss; the corresponding restored frames are in the image ledger.

Every raster path also has hair-off and lobe controls. Five held native views
cover front, three-quarter, profile, crown and backlit. The full generated Acceptance
scene was captured at both fixed head cameras on all six raster paths. Its multi-second
frames trigger MCP's stale/liveness threshold; camera changes and frame indices
advance in the ledger. Those captures demonstrate integration, not temporal quality.
Clean, fast head-only scenes retain the same placement and Acceptance lights for
the live quality/motion matrix. The GL fixture separately uses the real Scene pipeline.

The actual LookAround duration is 7.24 seconds. The GL fixture steps the real scene
continuously at 60 Hz through the whole clip. Live Play captures observe and unwrap
the active scene's animation time through at least one full cycle on every path.
[GL](../testing/evidence/hair1593/GL-motion.webp),
[Vulkan](../testing/evidence/hair1593/VK-motion.webp) and
[Vulkan hybrid](../testing/evidence/hair1593/VK-hybrid-motion.webp) are sampled
lossless evidence animations, displaying each sampled frame for 600 ms; they are
not recordings at a claimed playback frame rate. Source stills are native 1080p.

Distances 1.5, 2, 2.4, 4, 20, 40, 2 and 1.5 m exercise both directions.
At 20 and 40 m the apparent-size policy requests the shell/mesh tier and truthfully
retains cards: the measured #1252 policy did not select the shell tier. The
fallback reason is retained in the ledger; no forced mesh-success claim is made.

## Delivery, validation and cost

The packed native test round-trips the exact shipping bytes through
AssetPackBuilder and RuntimeAssetManager, preserves its independent skin control,
and verifies live binding/deformation in
[loose](../testing/evidence/hair1593/GL-loose.webp) and
[packed](../testing/evidence/hair1593/GL-packed.webp) native screenshots.
The optional `ExportsHumanHairGame` test uses GameBuildPipeline to produce a
standalone distribution: four packed assets, one scene, staged glTF buffers,
textures and thickness sidecars. A fresh OloRuntime loads that distribution,
deserializes six entities with zero failures, and loads all three render assets
from the pack. Its [native client capture](../testing/evidence/hair1593/runtime-cold.webp)
uses DPI-aware PrintWindow and shows simulation running on the packed hair.
The same cold distribution was also inspected on Vulkan in a
[native runtime capture](../testing/evidence/hair1593/runtime-cold-VK.webp).

The validation-enabled Vulkan editor loaded `VK_LAYER_KHRONOS_validation` with
core/synchronization validation and emitted zero VUIDs during the native, edge,
LOD and full motion campaigns. The hybrid path reported one complete groom proxy,
724,656 resident triangles, 55,073,856 proxy bytes, two TLAS instances, three
ray-traced lights and zero light fallbacks. Motion captures retain fresh RT stats.
The settings/distance sweep retains complete groom participation and two TLAS
instances through the handovers: the far coarse proxy has 93,620 triangles and
7,115,120 bytes. Raster cards retained for the unselected shell tier are reported
separately from that RT proxy. The raster-shadow A/B's inactive groom is not
counted as RT engagement.
This proves caster participation; it does not add RT mask reception to groom shading.
Release shader introspection is compiled out, so logs provide shader diagnostics.
No head-scene shader compile/link error or binding refusal was observed.

The initial full-scene run also exposed baseline Fox version-4 loader refusals.
The unchanged FoxCoatEvidenceTest recipes were re-exported through the current
serializer, preserving the two bindings and colour texture byte for byte and
keeping registry handles. Only the two groom files change. The cooked-binding
regression now includes both Fox coats on cold and warm model loads. Final
Acceptance captures were refreshed with those current-format assets.
[GL](../testing/evidence/hair1593/GL-Fox-pair.webp) and
[Vulkan](../testing/evidence/hair1593/VK-Fox-pair.webp) show the two bound coats.

Fresh editor cost observations used a held native three-quarter head, TAA on,
MSAA 1, native scale, unchanged lights and no competing engine build/test/runtime.
Vulkan validation was disabled. Each path has 24 unique valid GPU frame samples,
complete pass attribution, and a host process record. These samples are descriptive,
not paired run-level regression confidence intervals.

| Backend/path | Frame median ms | CPU median ms | GroomPass GPU median ms | GroomPass CPU dispatch median ms |
|---|---:|---:|---:|---:|
| GL Forward | 43.20 | 42.15 | 38.04 | 38.55 |
| GL Forward+ | 43.03 | 41.94 | 38.19 | 38.49 |
| GL Deferred | 43.05 | 41.98 | 37.33 | 38.47 |
| Vulkan Forward | 44.32 | 43.50 | 9.57 | 38.86 |
| Vulkan Forward+ | 43.87 | 43.09 | 9.21 | 38.45 |
| Vulkan Deferred | 43.51 | 42.78 | 9.21 | 38.73 |

The native apparent-size policy draws 55,733 strands,
1,727,723 segments, and requests 533,556,576 logical geometry bytes (508.84 MiB).
The maximum group width compensation is 6.9992; the short regrowth and long locks
have separate apparent-size strides, so compensation is not uniformly two.
The card tier draws 4,838 cards, 149,978 bound segments and 47,407,536 bytes
(45.21 MiB); runtime binding retains exemplar detail despite the 12-control card
cook setting. Coat volumes add about 3.3 MiB. Driver-reported whole-device usage
was 4,510–4,615 MiB on GL and 5,058–5,142 MiB on Vulkan, including desktop/other
resident allocations. Logical geometry bytes are not driver-reported VRAM.

The groom file grows from 6.43 to 54.88 MiB; the binding grows from 1.07 to 2.90 MiB.
This delivery increases detail and memory and does not meet a 16.7 ms frame budget.
It makes no performance improvement claim. CPU groom/deformation work and the GL
Forward+ cost remain with #1588/#1589; full-scene hitches remain #1594.

Cooker, importer, binding, finite-value, state and packed-runtime contracts were
executed locally: 93 cooker/binding contracts, 48 determinism/reference/rejection
tests, five final native/packed/game/binding tests and three affected groom
integration tests passed. The broader 16-test head/skin/camera regression passed
on the preceding recipe; native, package and binding checks were then repeated
on the final installed pair. The native head suite also bounds guide displacement below
15 mm through the clip, with no refused bindings or roots held at rest. Exact
executed test counts/log digests and all pass medians live in verification.json.
Save-game/component cross-binding, C#/Lua and physics API cells are not reached:
the change adds no component fields or serialization format and changes only
existing asset/fibre/simulation values. Current-format delivery only; no migration.

The full raw PNG/MCP campaign and standalone distribution remain ignored scratch
outputs under `.claude/skills/run-oloengine/shots/hair1593`. Curated images preserve
native pixels in lossless WebP; [images.json](../testing/evidence/hair1593/images.json)
records their source identities. No rendering tolerances were widened.

## Raw run output

The rest of this campaign's raw output (captures, HDR buffers, per-run reports and logs) is not tracked since #1610; the images and reports linked from this page stay. Regenerate it with the matrix described above. The copies this page was written from are in Git history: `git show 0cc2cd225:docs/testing/evidence/hair1593/<file>` prints one, and `git ls-tree -r --name-only 0cc2cd225 docs/testing/evidence/hair1593` lists them.
