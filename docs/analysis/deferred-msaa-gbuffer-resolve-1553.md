# Deferred MSAA G-buffer resolve (#1553)

Resolve the full G-buffer once, after the last writer of its multisample attachments.
Per-sample lighting keeps scene, virtual geometry, recovered GPU instances and opaque
decals in multisample storage. `GBufferResolvePass` publishes all six colors and depth
after those writers. Hi-Z and decal projection receive depth-only resolves immediately
before they need current depth.

Resolve-before-lighting preserves its existing rasterization rule: scene geometry uses
MSAA, but late geometry writes single-sample attachments. `GBufferEarlyResolvePass`
therefore runs after ScenePass and before those late writers. Moving that resolve after
the late single-sample writers would overwrite their pixels. Only one of the two nodes
declares work in a frame; with MSAA off neither declares a resolve.

Whole-framebuffer graph accesses cover entity ID and baked GI as well as the lighting
inputs. The emissive flags shader resolve also declares its multisample sample and
resolved render-target write. Deferred lighting declares the resolved depth/entity-ID
transfers even when lighting itself samples multisample colors, retaining the final
resolve when screen-space AO is off.

## Evidence

The baseline uses the original renderer at `b76e5b4be` with the corrected decal fixture.
At 1920x1080, both OpenGL and Vulkan recorded 21 full-family attachment blits totaling
273,715,200 bytes in per-sample mode with recovered instances and opaque decals.
Resolve-before-lighting recorded seven blits totaling 91,238,400 bytes. At FSR Performance's
960x540 internal size those figures were 68,428,800 and 22,809,600 bytes.

The ledger measures logical copied bytes, not physical memory bandwidth or elapsed GPU
time. Concurrent builds make frame-time comparisons on this host unsuitable for a speed
claim. Before/after reports use `Before_` and `After_DeferredResolve_` prefixes under
`OloEditor/assets/tests/exports/`; visual evidence is under `assets/tests/visual/`.
These are generated, git-ignored run artifacts, retained locally or as CI artifacts;
the tests construct their fixtures and assertions without reading committed captures.

Both live backends recorded these counts with both late writers active:

| Backend | Lighting | Internal size | Before blits / bytes | After blits / bytes |
|---|---|---|---|---|
| OpenGL | Per-sample | 1920x1080 | 21 / 273,715,200 | 9 / 107,827,200 |
| Vulkan | Per-sample | 1920x1080 | 21 / 273,715,200 | 9 / 107,827,200 |
| OpenGL | Resolve-before | 1920x1080 | 7 / 91,238,400 | 7 / 91,238,400 |
| Vulkan | Resolve-before | 1920x1080 | 7 / 91,238,400 | 7 / 91,238,400 |
| OpenGL | Per-sample | 960x540 FSR | 21 / 68,428,800 | 9 / 26,956,800 |
| Vulkan | Per-sample | 960x540 FSR | 21 / 68,428,800 | 9 / 26,956,800 |
| OpenGL | Resolve-before | 960x540 FSR | 7 / 22,809,600 | 7 / 22,809,600 |
| Vulkan | Resolve-before | 960x540 FSR | 7 / 22,809,600 | 7 / 22,809,600 |

Per-sample mode removes twelve color blits and 165,888,000 logical bytes per native
frame (60.6% of this family's old traffic). None, GPU-only, decal-only and both late
writers require respectively 7, 8, 8 and 9 blits; every case has exactly six color
blits. Resolve-before mode stays at seven because its late writers use single-sample
storage. MSAA off performs no G-buffer resolve. The independent projection-depth
bug fix adds one four-byte-per-texel snapshot in single-sample decal raster modes;
that copy is outside the MS-to-resolved totals above. Their pixels remain unchanged.

Release tests passed 34 graph/cache contracts, twelve export tests and two virtual-
geometry tests, plus three Vulkan pass tests. The export matrix covers samples
1/2/4 x both lighting modes x four late-writer combinations. Recovered geometry is
pinned by dropping the phase-two queue as a negative control: omission gives RMSE
22.49-22.94, while 41,935-44,979 flat interior pixels match the unculled reference
at RMSE 0. Per-sample whole-frame recovery also has RMSE 0. Resolve-before's intentional
single-sample late raster differs at MSAA edges, as it did in the original renderer.

All 96 headless before/after PNG pairs have zero changed RGBA pixels, including MSAA
off, albedo, normals and two composite camera angles. All sixteen live albedo/normal
pairs also match exactly within their backend. Cross-backend equivalence is not claimed.
The four virtual-geometry MSAA images match the unculled baseline exactly. Its previous
fixture's settings application disabled the requested HZB toggle; after correcting that
fixture, the per-sample two-phase cell records the required intermediate depth resolve.
The camera-motion test also passes with the real two-phase path enabled.

Live verification covers all 24 Deferred combinations on each backend, both 4x FSR
lighting modes and four Forward/Forward+ native/FSR controls per backend. The eight
control copy-group lists match their baseline exactly. Vulkan's two hybrid shadow
cells report one ray-traced light and zero fallback lights. Both logs contain the
explicit `--rhi` backend selection and zero errors/VUIDs.

Local comparison reports are `DeferredResolve_1553_{ImageComparison,
LiveImageComparison,VirtualGeometryComparison,ControlComparison}.json`. Cell ledgers
are `RenderTargetExports_{Before,After}_DeferredResolve_GL_Deferred_Msaa*_*.json`
and `{Before,After}_Live_DeferredResolve_{GL,Vulkan}_*.json`. PNGs with the corresponding
cell prefixes include the red receiving decal, recovered instance field and two camera
angles. Historical #1332 goldens and incidental timing reports were restored.

Regenerate the headless ledgers, captures and replay scenes by running
`RenderTargetExportEvidence.DeferredResolveMatrix` and
`RenderTargetExportEvidence.DeferredResolveUsesTheUpscaledAttachmentSize` in the
Release test executable with a GL context. For baseline output, use the original
renderer with the corrected fixtures and set `OLO_GBUFFER_RESOLVE_BASELINE=1`.
The live reports and comparison summaries above record the verification session;
they are not inputs to these tests. Run artifacts are excluded from the PR so
verification does not add hundreds of captures and reports to the source tree.

## Bugs found while verifying

- The previous evidence decal had no receiving geometry. It issued a draw and a resolve
  while contributing no fragments. The corrected fixture projects red albedo onto Cube0's
  visible front face; the matrix checks for those red pixels.
- Debug display previously ran inside ScenePass, before late writers. It now runs at the
  deferred lighting stage, after the finished G-buffer is available.
- Vulkan cannot blit RGBA8 albedo into RGBA16F scene color. Albedo debug now uses a texel-fetch
  fullscreen shader; the Vulkan debug test exercises the actual HDR destination format.
- Resolved-mode opaque decals previously sampled the depth attachment of their own raster
  framebuffer. Disabling depth writes does not make that feedback legal on Vulkan. They now
  copy projection depth into an independent graph transient before drawing. Per-sample mode
  already has independent multisample raster and single-sample projection depth.

- The virtual MSAA fixture enabled HZB before applying renderer settings, which restored
  the stored off flag. Preserving the toggle after application makes its two-phase
  cells exercise the path they name; their ledger assertions detect this mistake.
