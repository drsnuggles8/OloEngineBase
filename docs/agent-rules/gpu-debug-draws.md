# GPU-pushable shader debug draws (issue #725)

Applies to: any work on a GPU-driven pass — culling, cluster LOD, virtual
texturing, DDGI, GPU quadtrees, particle/fluid computes.

The point of this feature: the decisions those passes make (what was culled, what
bounds it used, where a probe relocated to, which page it asked for) are computed
on the GPU and **never come back**. Before this, the only instrument was "add a
debug colour output, reload, screenshot" — the loop
[volumetric-cloud-debugging.md](volumetric-cloud-debugging.md) documents precisely
because nothing better existed. Now any shader can draw.

---

## 1. Pushing from a shader — three lines

```glsl
#define OLO_DEBUG_DRAW_AABB          // name ONLY the channels you push
#include "include/DebugDrawCommon.glsl"
...
OloDebugDrawAABB(worldMin, worldMax, vec3(1.0, 0.0, 0.0), OLO_DEBUG_SPACE_WORLD);
```

Seven primitives, one append channel each: `OloDebugDrawLine`, `…Circle`,
`…Rectangle`, `…AABB`, `…Box` (8 explicit corners), `…Cone`, `…Sphere`. Every
helper takes a colour and a coordinate-space tag.

**The channels are opt-in per primitive, and that is not politeness.** Each is an
SSBO block, and storage blocks are a scarce per-stage resource: GL guarantees only
8 per stage and NVIDIA's compute limit is 16. `VirtualClusterCull.comp` already
declares 9 of its own — a header that unconditionally declared all seven would put
it over the limit and the shader would simply fail to link. Name what you push;
`OLO_DEBUG_DRAW_ALL` exists for the draw-side shader, which needs them all.

**Turn it on**: `RendererSettings::ShaderDebugDrawEnabled` (Renderer Settings
panel, the F3 overlay, or `olo_shader_debug_draw { enabled: true }` over MCP).

---

## 2. The contract, and why it is shaped this way

Full detail lives in
[`Renderer/Debug/ShaderDebugDrawTypes.h`](../../OloEngine/src/OloEngine/Renderer/Debug/ShaderDebugDrawTypes.h).
Three properties are worth knowing before you rely on the tool.

### 2a. The channel buffer IS the indirect draw

Each channel opens with a 32-byte header whose first 16 bytes are a GL
`DrawArraysIndirectCommand`. `glDrawArraysIndirect` reads it at offset 0 of the
same buffer, so the primitive count never travels through the CPU — which is the
whole reason a compute shader's output can appear in the same frame.

### 2b. Two counters, and the overflow flag falls out for free

```glsl
uint slot = atomicAdd(RequestCount, 1u);   // UNCLAMPED
if (slot >= Capacity) return;              // dropped, but COUNTED
Entries[slot] = entry;
atomicAdd(InstanceCount, 1u);              // only accepted pushes draw
```

The order is the contract. `RequestCount` is what you asked for, `InstanceCount`
is what got drawn, and `RequestCount > Capacity` is exactly "you overflowed and
`Requested - Capacity` draws were dropped". Bumping `InstanceCount` only on the
accepted path is what keeps the indirect draw in range — it can never cover a slot
that was not written.

**Read the overflow flag before you conclude the feature is broken.** "I drew
nothing" and "I overflowed and everything past slot N was thrown away" look
identical on screen and are the hour-wasting failure this exists to remove. The
counters are in the Renderer Settings panel, the F3 overlay, and
`olo_shader_debug_draw`'s response; an overflow also logs once per channel.

### 2c. Push volume is your problem

A per-cluster or per-pixel push overflows a 4096-entry channel instantly. The
shipped virtual-geometry consumer therefore has a **stride** (emit every Nth
cluster, default 32), and any new high-frequency consumer needs the same kind of
gate — a selected entity, a screen region, an every-Nth-index test. An overflowing
channel is not a visualization, it is a picture of an arbitrary prefix in an
arbitrary order.

---

## 3. Coordinate spaces

| Tag | Positions are… | Use it for |
|---|---|---|
| `OLO_DEBUG_SPACE_WORLD` | world space | anything about scene geometry — the default |
| `OLO_DEBUG_SPACE_MAIN_NDC` | already main-camera NDC | screen-space work (tile grids, cull rectangles) without inverting a projection in the pusher |
| `OLO_DEBUG_SPACE_OBSERVER_NDC` | NDC of a second, detached camera | inspecting another frustum from the main view |

`ObserverCameraNDC` is **live but degenerate**: the observer camera itself is
issue #726 and does not exist yet, so the pipeline uploads the *main* camera's
inverse view-projection. That makes the space an exact identity round-trip to
`MainCameraNDC` rather than a source of garbage geometry, and leaves one line to
change (`RenderPipeline::UploadExecutionState`) when #726 lands.

---

## 4. Things that cost debugging time

**Segments become screen-space QUADS, not `GL_LINES`.** Core-profile GL only
guarantees line width 1, and `RendererAPI::DrawArraysIndirect` issues
`GL_TRIANGLES`. Expanding to quads gets a real width knob *and* means the feature
needed no new RHI entry point — worth remembering if you are tempted to "simplify"
it back to lines.

**The `SHADER_STORAGE | COMMAND` barrier is load-bearing, and its absence is the
most confusing possible symptom.** Without `COMMAND` the indirect draw may legally
read a stale instance count, which presents as "the first frame after a push draws
nothing, and then it works" — i.e. an intermittent bug that looks like a race in
whatever shader you were actually debugging.

**Never read a channel buffer directly.** It is `GL_DYNAMIC_COPY` and is read
every frame as the `GL_DRAW_INDIRECT_BUFFER` of its own draw, so it must stay in
video memory. A CPU `glGetNamedBufferSubData` straight off it makes NVIDIA log
131188 and migrate it VIDEO→HOST (131186), permanently slowing the draw. Stats go
through a `DeviceToHost` staging copy issued at the end of the debug pass and read
at the *next* `BeginFrame` — one frame of latency, no stall. Same trap, same fix,
as `VirtualMeshRegistry::ReadFrameCullStats`.

**Stats are one frame old.** By construction, per the above. Do not read a
stale zero as "nothing was pushed" on the frame you enabled the feature.

**A CPU push made after `EndScene` lands in the NEXT frame.** CPU pushes are
staged and flushed by `ShaderDebugDraw::BeginFrame()` inside
`RenderPipeline::UploadExecutionState` — after scene traversal, before the graph
runs — so CPU entries occupy slots `[0, n)` and GPU appends start at `n`. That
shared buffer is deliberate: it is what lets a CPU-computed bound and its
GPU-computed counterpart be drawn together and compared.

**"Zero cost when disabled" needs the buffers to stay ALLOCATED AND BOUND.** The
helpers guard on a plain `Capacity == 0u` read, and reading an unbound SSBO is
undefined in GL (the spec permits program termination). So the seven channels are
created and bound at `Renderer3D::Init` at their header-only size, 32 bytes each,
and never unbound. Deleting that "wasteful" allocation would make every push site
undefined behaviour on the *disabled* path.

**Debug geometry is drawn pre-tonemap and depth-tested, with depth writes OFF.**
Depth *test* is what makes a world-space bound read as being in the scene rather
than pasted over it. Depth *write* stays off so the debug overlay never changes
what a downstream pass computes (AO, SSR, fog all read scene depth). The pass also
narrows the scene framebuffer to colour[0] for its draws — the debug fragment
stage has no entity-ID / view-normal / velocity output, and leaving those
attachments selected would write undefined values over them and break editor
picking wherever a line crosses.

---

## 5. The shipped consumer

`VirtualClusterCull.comp` emits each cluster's world-space cull sphere,
colour-coded by which test decided it:

| Bit | Verdict | Colour |
|---|---|---|
| 1 | drawn (survived every test) | green |
| 2 | frustum-culled | red |
| 4 | cone-culled (backfacing) | blue |
| 8 | Hi-Z occluded in phase 1 | yellow |

`RendererSettings::ShaderDebugDrawClusterBounds` / `olo_shader_debug_draw
{ clusterBounds }`. Deferred path, virtual geometry in view.

Two details worth copying into the next consumer:

- **Phase 1 only.** The two-phase occlusion cull re-tests an already-classified
  subset in phase 2, so emitting there would draw a second sphere over most of the
  same clusters and make the drawn set look larger than the cut actually is. The
  phase-2 dispatch sets `u_DebugDrawClusters = 0` *explicitly*, because uniform
  state is per program and persists across dispatches.
- **Perspective view only.** The shadow cascades bind the same program with
  `u_OrthoMode = 1` and would otherwise emit a second, differently culled set on
  top of the first.

---

## 6. Where the guards are

| What | Where |
|---|---|
| struct sizes/offsets, binding derivation, GLSL literal cross-check, overflow protocol | `tests/Rendering/ShaderDebugDrawContractTest.cpp` (L1/shaderpipe) |
| primitive → line-segment expansion, analytic | `tests/Rendering/ShaderDebugDrawExpansionTest.cpp` (L1/shaderpipe) |
| the real GLSL append helper + overflow, on hardware | `tests/Rendering/PropertyTests/ShaderDebugDrawGpuPushTest.cpp` (shaderpipe, GPU) |
| pixels reach the screen, depth test, disabled draws nothing | `tests/Rendering/PropertyTests/ShaderDebugDrawVisualTest.cpp` (L8) |
| binding-slot uniqueness | `tests/Rendering/ShaderBindingLayoutTest.cpp` |

The contract test reads the shader source as **text** and asserts the
segment-count `#define`s match the C++ constants. That is crude, but the GLSL
cannot be executed headlessly and a drifted constant does not crash — it draws a
fraction of each primitive, which reads as "the sphere looks a bit chunky" rather
than as a bug.
