# VSM caster evidence for #1524

Verify directional clip levels and local point/spot layers separately when adding
an external VSM caster. Terrain, both voxel meshes, and foliage now share their
existing CSM/atlas deformation and discard stages with VSM entry shaders. The
physical image is rebound after the program switch, with heap-offset flushing
for OpenGL bindless programs. Vulkan workers only consume preallocated view
resources.

Stable family footprints invalidate arrivals, departures, movement and content
revisions before page allocation. Unknown bounds stay conservative. Terrain
uses the light frustum across all built chunks and hashes only depth inputs;
VT feedback rotation and surface material changes retain cached pages. Open
terrain surfaces and foliage render both sides, preserving the physical image
across program switches.

## Measurement contract

Each cell compares the same isolated scene with light casting disabled, ordinary
CSM/local atlas, VSM directional with atlas locals, and VSM directional/local
layers. Two camera angles and sun, spot and point fixtures distinguish casting
from unrelated image changes. The floor-space region measures darkness mass
and world-space centroid, requiring more than 30 contributing pixels, VSM mass
above 15 percent of the atlas result and centroid drift below 1.5 world units.
Headless captures use the receiver entity mask; live captures use the fixed
floor region projected from the recorded camera.
Packaged-runtime capture helpers use per-monitor DPI awareness and require the
physical client size to match the logged render viewport. The initial Windows
capture attempt cropped a 1920 by 1080 frame to 1280 by 720 and is excluded.

Additional controls exercise static page reuse, camera cuts, caster motion,
removal/restoration, foliage cast flags and deformation. No measured performance
claim is made. Captures require a fresh frame and settled presentation settings;
unready captures are rejected. New temporal A/B captures settle for 64 frames;
the per-cell ledger preserves the earlier core captures' frame counts. The
forced impostor failed at eight frames in both CSM and VSM, then passed the
same sun fixture at 64 frames without changing the subject or oracle. Cold
OpenGL program linking can cancel an
unclaimed MCP request: readiness is awaited before retrying that cancelled call.

## Evidence index

The committed editor ledger contains 6,928 measured cells, with 372 packaged-runtime
cells and 90 snow-transition cells retained separately. The
[editor measurements](../../OloEditor/assets/tests/visual/ShadowFamilyLive/Measurements.csv),
[runtime measurements](../../OloEditor/assets/tests/visual/ShadowFamilyLive/RuntimeMeasurements.csv),
[snow measurements](../../OloEditor/assets/tests/visual/ShadowFamilyLive/SnowTransitions/Measurements.csv),
[scope record](../../OloEditor/assets/tests/visual/ShadowFamilyLive/Scope.json),
and [file hash manifest](../../OloEditor/assets/tests/visual/ShadowFamilyLive/Manifest.json)
identify the captures and their completeness.

The headless ledger contains 2,592 core GL cells plus 27 controls in
[ShadowFamilyMeasurements.csv](../../OloEditor/assets/tests/visual/ShadowFamilyMeasurements.csv).
After merging master at `b239439b2`, all 603 targeted CPU/graph contracts,
13 expanded family/cook/shader tests, four attachment/snow snapshot tests and
13 existing shadow parity tests passed without skips. All four Release
production/test targets and the Debug editor built. All 15 scene records
round-tripped through the rebuilt pack.

The broad pre-merge editor/runtime campaigns remain pinned to their original
source and binary hashes. Post-merge integration adds 324 native family cells
and 54 odd-resolution Deferred/MSAA/upscale cells per backend, 18 snow transitions
per backend, and native/odd terrain runtime probes on both backends. Debug Vulkan
records an inserted validation layer and no VUID messages. The incoming graph
change left caster shaders, extraction and cache logic unchanged; these added
checks exercise graph ordering and the changed attachment/snow consumers.
The [integration reports](../../OloEditor/assets/tests/visual/ShadowFamilyLive/IntegrationContracts/Provenance.json)
retain the contract XML, snapshot captures and build hashes.

Both packaged-runtime backends passed 180 broad cells plus six post-merge probes,
with no error diagnostics and capture/render viewport agreement in every retained
process. One minimized capture and one Windows display/DPI-query attempt are
excluded and preserved with controlled or identical-input clean reruns. The
capture helper retains its window handle, restores observed minimization and
restarts settling before publishing a PNG.

Snow disable returned shadow mass and centroid to the dry baseline on all three
paths. The first disabled VSM measurement retains cached pages before a mode
switch. The old renderer left 7,659 displaced silhouette pixels and about
3.1 units of centroid drift on all three GL paths; that negative control remains
separate from the corrected measurements.

## Support boundaries

Forward and Forward+ consume a single scene sample. MSAA is exercised through
the Deferred G-buffer. Bilinear is not a selectable renderer technique; spatial
upscaling uses FSR1 EASU. Temporal requests record the actual resolved technique
and fallback reason; Vulkan and MSAA use the declared spatial fallback.

The package test loads all 15 scene records through the real asset pack. Runtime
launches load that pack and use the stock loose-YAML start-scene route with
production-staged referenced content; they do not demonstrate direct activation
of a packed scene record.

Cooked runtime currently consumes native Forward defaults without a render-path
or MSAA entry point. Scene upscale settings are persisted but not propagated
to the renderer by runtime activation (#1563, owned by #1540). Regular VOX1 edit
volumes are absent from the scene format (#1566, the authored-state contract
is owned by #1558); they are exercised live through the new import tool. The shipped greedy
fixture uses persisted heightfield auto-seeding. `Scene::RenderScene3D` confines
that reconstruction to `GreedyCubic`; switching to MarchingCubes clears the
auto-seeded chunks. A fresh regular override starts empty and its mesher only
rebuilds existing chunks. The sole production VOX1 decoder caller is editor MCP,
so no persisted-scene route currently reconstructs the regular voxel subject.
Regular voxels remain supported and verified in the editor. No schema version changed.
Virtual geometry and groom retain their existing directional VSM routes and
local atlas parity; VSM local-layer support remains absent for those families.

Delete/undo drops the voxel override because the current terrain component copy
omits it (#1561, owned by the active component worktree). Live restoration
explicitly reimports VOX1 data rather than claiming undo preserved the volume.
The pre-existing Mono startup diagnostic is tracked by #1540, whose active
worktree owns application and scripting startup.

The pre-graph setup now publishes disabled snow state each frame (#1562).
The owning #1554 session released that narrow block to this task. The regression
requires visible snow displacement, then restores the dry terrain silhouette
and shadow moments through the production disable transition. Its first disabled
VSM measurement retains the existing page cache; it precedes any mode switch.
The family cache also invalidates the final deforming silhouette once, then
resumes static reuse, independently of other footprint changes.

VSM off with local layers requested on consumes the same CSM/atlas route as
CSMAtlas. The local request remains stored, while activation, layer counts and
sampling remain disabled. `VirtualShadowMapLocal` and `McpRendererSettings`
pin that alias; the matrix records it without duplicate captures.

Bindless receiver shaders must omit raster-pass scratch, which they never read.
The raw GLSL front end counts declared blocks against the stage limit before
eliminating unused declarations. Receiver-only resource declarations repair
three lighting-program link failures. Terrain still has 14 active UBOs plus
the heap-offset UBO on NVIDIA's 14-block fragment limit (#1565). Its beauty
program takes the existing slotted fallback; the VSM family raster programs
compile bindless. The terrain layout is owned by #1558. The final campaign
retains that error and consumed-route distinction rather than claiming a clean
fully bindless beauty pipeline.

The raw bindless loader also reports a NVIDIA parser failure in the shared
groom tint helper (#1567, groom-owned shader reservation). Both groom programs
reject the `packed` parameter; the subsequent undefined-variable messages are
retained. The parameter rename remains a hypothesis awaiting the owning task's
before/after driver verification. Slotted GL/Vulkan groom evidence does not
claim this raw bindless route works.

The Debug Vulkan first terrain frame exposed a shared snow-image transition
in every CSM worker. Retained assertions name all four items and one image
subresource. The shadow pass now transitions that sample on the primary before
recording workers. The corrected Debug editor passed all 324 core native cells
with the validation layer inserted and zero VUID messages; the old assertions
are retained separately.
