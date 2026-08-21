# Baked lightmaps (#439): the units ledger, the seam-split staleness trap, and adding a vertex channel without touching `Vertex`

Issue #439 added the baked-GI path: an offline CPU lightmap bake (xatlas UV2 unwrap +
`PathTracer::EstimateIrradiance` per texel) for static geometry, a path-traced bake mode for the
existing SH light-probe volumes, and the runtime sampling/staleness machinery. Read this before
touching `Renderer/Baking/**`, `Scene/SceneLightmap.*`, `include/LightmapSampling.glsl`, the
lightmap branch of `PBR_MultiLight.glsl`, or `LightProbeBaker`'s path-traced mode.

Every section is a decision that looked interchangeable with its alternative and was not.

---

## 1. A new per-vertex channel does NOT have to widen the vertex struct — and here it must not

The obvious way to add lightmap UVs is a `vec2 TexCoord2` member on `Vertex`. Measured blast
radius of that option before rejecting it: the 32-byte `sizeof`/`offsetof` static_asserts, ~38
shaders whose `OLO_VULKAN` vertex-pull branch hard-codes `gl_VertexIndex * 8` (they would read
garbage with no compile error), 7 skinned shaders whose bone attributes shift location, the
`.omesh` stride reject, the LOD attribute table, the primitive generators' 3-arg constructor
(which would leave the new member uninitialized), +16 bytes on every vertex in the engine, and a
guaranteed hard conflict with the concurrent mesh-shader epic.

The shipped alternative: **a parallel stream on `MeshSource`** (`m_LightmapUVs`, one `vec2` per
vertex), exactly the shape bone influences already use. `Vertex` unchanged, only lightmapped
meshes pay, the Vulkan pull path untouched (its lightmap support is a follow-up second pull
buffer, compiled out until wired).

Three obligations come with a parallel stream, and each fails silently (or merely slowly) if missed:

- **`MeshOptimization::OptimizeMesh` reorders vertices**; every parallel array must ride the same
  `meshopt_remapVertexBuffer` call or its data silently belongs to other vertices afterwards.
  Bone influences, morph deltas and lightmap UVs each have a remap block — a new stream needs one
  too.
- **Attribute locations are assigned by `AddVertexBuffer` order** (`m_VertexBufferIndex` is a VAO
  counter, not a declaration). The lightmap stream is only ever added when `!HasSkeleton()`, which
  pins `a_TexCoord2` at location 3 for static meshes and leaves bones at 3/4 for skinned ones. Add
  a stream in a different order on one path and the shader reads the wrong buffer with no error.
- **An OPTIONAL stream consumed by a shared program must be constant-stubbed on the meshes that
  lack it.** Leaving the attribute disabled on some static VAOs while others buffer-back it makes
  NVIDIA specialize a vertex-shader variant per attribute-layout permutation — logged as GL debug
  id 131218 ("recompiled based on GL state"; the engine warns + captures a draw-site stack on the
  *repeat* recompile of a program, and the first mixed frame after a bake triggered exactly that).
  The fix is `VertexArray::AddConstantVertexBuffer`: a stride-0 DSA binding is literal, so an
  8-byte zero buffer backs the attribute for any vertex/instance count, and every static VAO
  exposes one identical layout. Vulkan implements it as a recorded-nowhere no-op on purpose —
  appending the stub to the VAO's buffer list would shadow pull stream 1, the bone-pull slot.

## 2. The unwrap seam-splits vertices — so the staleness key must be computed AFTER it, and the resolve must be able to re-derive it

xatlas cannot parameterize a closed surface without seams, and a seam vertex needs two different
UV2s — so `LightmapUnwrap::Generate` **rebuilds the vertex/index arrays** (more vertices out than
in, each `xref`-copied from an original). That interacts with the staleness key in a way that has
one right ordering and two wrong-looking-right ones:

- The bake key hashes a geometry proxy (vertex/index counts + bounds + asset handle). Compute it
  **before** the unwrap and a reloaded scene (whose meshes come back un-unwrapped, at the
  pre-split counts) MATCHES the stale key — and renders wrongly: the un-unwrapped mesh has no UV2
  stream, the attribute reads (0,0), and every fragment samples its region's corner texel.
- Compute it after the unwrap but give the runtime no way to re-derive the unwrapped state, and
  every editor restart reads as "stale" — procedural primitives have no `.omesh` to persist their
  UV2 stream in, so the bake dies on reload with nothing actually changed.

The shipped contract: the key is computed **after** `Prepare()` (post-unwrap counts), and
`SceneLightmapRuntime::Resolve` is **self-healing** — a lightmap-static mesh with no UV2 stream is
re-unwrapped *before* the key check, with the same shared parameters (`kLightmapUnwrap*` in
`LightmapUnwrap.h`). That only works because the unwrap is deterministic, which is why the vendor
CMake pins `XA_MULTITHREADED=0`: same mesh in, bit-identical parameterization out, so the
regenerated stream lands exactly on the layout the bake rasterized. Change the unwrap parameters
in one place and not the other and every scene goes permanently stale — worse, a *different pack
resolution* can shift chart UVs without changing the seam-split counts, so the key MATCHES and
every sample reads another chart's texels silently. That is why the unwrap parameters are **not
settings at all**: `LightmapBakeSettings` deliberately has no `UnwrapResolution`/`UnwrapPadding`
fields (the first draft had them; self-review deleted them), and both `Prepare()` and the resolve
hard-code the shared `kLightmapUnwrap*` constants. Do not re-add the knobs without making the
resolve able to re-derive whatever a caller passed.

The generalisable rule: **a staleness key over derived state must hash the state the loader will
actually re-derive, and the loader must be able to re-derive it.** A key over pre-derivation state
is worse than no key — it validates confidently while the derived data is absent.

## 3. The units ledger — who stores E, who does not

The bake kernel IS the reference path tracer, which is what makes the units auditable at all:

| store | quantity | pinned by |
|---|---|---|
| DDGI irradiance atlas | full irradiance E | `DDGI_BlendIrradiance.glsl` header + `DDGIReferenceParityTest` |
| lightmap atlas (#439) | **indirect** irradiance E | `LightmapBakeParityTest` (bit-exact vs `EstimateIrradiance`) |
| baked SH probe volume | **band-limited RADIANCE at the normal — not E** | `LightProbePathTracedBakeTest` uniform-env pin |

Two things about that table:

- The lightmap stores **indirect-only** E *by construction, not by subtraction*: delta lights can
  never be hit by `EstimateIrradiance`'s hemisphere rays and only enter through NEE at later path
  vertices, so punctual direct lighting is excluded with zero extra machinery and stays realtime
  (crisp shadows, dynamic lights keep working). Emissive surfaces and sky DO enter the bake — the
  ambient ladder must therefore *replace* the probe/IBL diffuse for lightmapped pixels, never add
  to it.
- The SH row is a **pre-existing π-scale divergence** discovered (not introduced) by #439:
  `ProjectToSH` stores raw radiance projections with no cosine convolution and `evaluateSH`
  applies none either, so a uniform field of radiance L evaluates to L where irradiance would be
  πL. The path-traced probe bake deliberately **matches the shipped convention** — the two bake
  buttons must agree photometrically, and rescaling the whole baked-SH path is a coordinated
  change of both bake routes plus the shader, not a drive-by. Until then, Hybrid mode's
  `mix(baked, ddgi, …)` and the lightmapped-static / probe-lit-dynamic seam carry that divergence.
  The tests pin it numerically so it cannot silently morph into a second bug.

## 4. Determinism is what makes the bake testable — and it is a property of the SEEDS, not the scheduler

Every texel's seed derives from its **atlas coordinates** (`MakePixelSeed(x, y, bakeSeed)`), every
probe's from its grid index, and packing order sorts by (region size, UUID). Consequences that the
test suite leans on directly: two bakes memcmp equal at any thread count; a single stored texel is
re-derivable bit-exactly by calling the oracle with the same seed (which pins the whole
raster→estimate→store path with `EXPECT_EQ` on floats — correct *here* because bit-identity is the
contract); and an independent-seed re-estimate isolates statistical correctness from
reproducibility. If you ever key a sample on "which worker got there first", all three collapse
into a tolerance band wide enough to hide real regressions — the same argument as
reference-path-tracer.md §2, now load-bearing in an asset pipeline.

## 5. Reused patterns that are contracts, not conveniences

- **`LightmapSampling.glsl` is slot-based with NO `OLO_BINDLESS` token** — a shared include's
  `#ifdef OLO_BINDLESS` is the bindless route's opt-in marker and would drag every includer onto
  the raw-GLSL route (glsl-shaders.md §5e, first exclusion row). The atlas is published through
  `HeapBinding::PublishTextureOffsetAndBind`, the DDGI-atlas mechanism that feeds slot-based and
  converted readers alike.
- **Per-draw lightmap regions follow the Color/Custom pattern end to end**: `DrawMeshCommand`
  field → a generic vec4 FrameDataBuffer stream entry per source in `CommandBucket::BatchCommands`
  (so the value survives the N-into-1 auto-batch collapse) → `InstanceData` in both dispatch
  paths. A per-draw value that skips the stream renders correctly right up until batching merges
  two entities, then quietly shares one entity's region across both.
- **`Ref<T>` propagates const through `operator->`** — `for (const auto& input : inputs)
  input.Mesh->Build();` is a compile error that a syntax-only check catches but an unbuilt target
  hides. It shipped in the first editor-side draft and was caught by a sibling agent's review, not
  a build. It then shipped AGAIN in the self-review batch (`const Ref<Scene> bakedScene`) and was
  caught by the same syntax-only preflight. When a change spans targets, remember which of them
  the verifying build actually compiled.
- **The shader branches on COVERAGE (`sampleLightmapIrradiance(...).a`), never on the colour.**
  The atlas alpha exists precisely to separate "validly baked pure black" (an enclosed surface no
  indirect light reaches — must KEEP its darkness) from "never baked" (must fall through to
  probes/IBL). A `dot(rgb, rgb) > 0` branch collapses the two and the enclosed room glows with
  sky IBL — the exact leak class the bake exists to kill. The first draft had that bug; the vec4
  return with `.a` as the branch signal is the fix, and the `.rgb` is un-premultiplied by the
  sampled alpha so bilinear taps at chart edges don't darken.
- **The ambient ladder has ONE definition** (`include/AmbientLadder.glsl`), shared by
  `PBR_MultiLight` and `PBR_MultiLight_Skinned` — static receivers pass the lightmap sample,
  skinned/dynamic shaders pass `vec4(0.0)` and enter at the probe rung. Structural parity is
  exact; PHOTOMETRIC parity is not (the §3 SH radiance-vs-E divergence) — the include's units
  caveat says so, and no comment may claim continuity until that ledger row is fixed.
- **The bake key hashes what the bake CONSUMES, not what looks equivalent.** Lights are hashed
  per type in `ReferenceSceneBuilder::AddScene`'s exact consumption terms: point/spot positions
  are the raw `TransformComponent::Translation` (the builder never composes the parent chain —
  hashing the composed `transform[3]` false-stales on re-parenting and misses nothing the bake
  sees), and a light the builder rejects (`!(intensity > 0)`) is invisible to the key too.
  `SceneLightmapStalenessTest` pins both directions.
- **`Resolve()` is throttled and memoized**: the O(scene) recheck (UV2 scan + key hash) runs
  every `kResolveRecheckIntervalFrames`, a mesh whose unwrap failed once is memoized until
  `Invalidate()` (xatlas is 100ms+ per mesh — retrying per recheck is a recurring hitch), and the
  stale/missing warnings are once-per-episode latches. `Invalidate()` is the "recheck NOW" lever;
  the editor calls it on bake completion and on `.olmap` hot-reload (the runtime's cheap path
  never re-reads the asset, so an externally replaced file needs that push).

## 6. Deliberately deferred (so nobody re-derives the gap as a bug)

- **Deferred-path lightmap sampling**: `DeferredLightingShared.glsl` shades from the G-Buffer,
  which carries no UV2 — the term would need a G-Buffer-pass fold. Forward/Forward+ carry the
  feature; deferred falls through to probes/IBL.
- **Vulkan lightmap sampling**: the UV2 stream needs a second vertex-pull buffer; the branch is
  compiled out (`OLO_VULKAN` stub) rather than reading unbound state.
- **VirtualGeometry / InstancedMeshComponent / ModelComponent receivers**: only the classic
  `MeshComponent` path samples the lightmap in v1.
- **Albedo textures in the bounce**: `ReferenceScene` materials are factor-only by design
  (reference-path-tracer.md); bounce colour comes from base-colour factors.
- **Sky/HDRI in the bake**: the reference environment is uniform-only; exteriors under an HDRI
  have no ground truth until the environment model grows a directional representation.
