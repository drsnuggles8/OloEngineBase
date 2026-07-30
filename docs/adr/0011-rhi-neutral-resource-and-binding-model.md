# API-neutral RHI resource/binding model: split identity from binding address from native handle, heap+offset from day one

Issue [#691](https://github.com/drsnuggles8/OloEngineBase/issues/691) Phase 1.
[ADR 0010](0010-vulkan-rhi-heap-bindless-only.md) decided *that* we add a Vulkan
backend and *that* its binding model is `VK_EXT_descriptor_heap` bindless with
no classic descriptor-set path. This ADR decides *what the engine-side
abstraction looks like* so that Phase 2's call-site sweep happens exactly once.

Three decisions are recorded together because they were taken together and
constrain each other: the resource/binding model (§1), backend selection (§2),
and the PSO-cache + hot-reload story (§3). They are kept in separate sections so
a future revisit can target one without reopening the others.

**No code motion.** This ADR ships alongside declaration-only headers under
`OloEngine/src/OloEngine/Renderer/RHI/` (the vocabulary, no implementations) and
a coverage ratchet (`OloEngine/tests/Rendering/RHIBoundaryRatchetTest.cpp`).
Nothing is converted; no `Ref<T>` changes; no `VkXxx` anywhere.

---

## 0. The survey the design is built on

The issue's body estimated "~620 raw `glXxx()` calls outside `Platform/OpenGL/`";
the task handover measured 724. Neither reproduces. The 724 came from a pattern
loose enough to match `glfw*` window calls, which are not GL — that accounts for
almost the entire gap. (Stripping comments and string literals matters too, but
much less: with the same pattern it removes only 16 of 565 hits. It is still
worth doing for a number a test asserts on.)

Measured on `ca260e44` with comments and string/char/raw-string literals
stripped first (the exact rule the ratchet test implements — see
`OloEngine/tests/Rendering/RHIBoundaryRatchetTest.cpp`), counting identifiers
matching `gl[A-Z]\w*` immediately followed by `(`:

| Bucket | GL calls | Files with calls | Files including `<glad/gl.h>` |
| --- | ---: | ---: | ---: |
| **Sweep** — `OloEngine/` minus `Renderer/Debug/` | **313** | 35 | 70 |
| **Tools** — `OloEngine/Renderer/Debug/` | **236** | 7 | 9 |
| Backend — `Platform/OpenGL/` (not a violation) | 497 | 16 | 22 |
| **Total outside `Platform/OpenGL/`** | **549** | **42** | **79** |

108 distinct GL entry points are used outside the backend. Four corrections to
the picture the issue paints:

- **Window/context creation contributes zero raw GL.** The handover attributed
  33 calls to `Platform/Linux/LinuxWindow.cpp` and 32 to
  `Platform/Windows/WindowsWindow.cpp`; both are in fact `glfw*` calls. There is
  no window/context exemption bucket to argue about — `Platform/*Window.cpp` is
  already GL-free.
- **`Renderer/Debug/` is 43% of the problem** (236 of 549) in 7 files, and it is
  *not* legitimately exempt (§1.6).
- **The include graph is the real boundary, and it is far worse than the call
  count.** 79 files outside `Platform/OpenGL/` include `<glad/gl.h>` but only 42
  call GL. The other **40 include it purely to name `GLenum` / `GL_*` constants
  they pass into `RendererAPI`'s own virtuals** — `SetBlendFunc(GLenum, GLenum)`,
  `SetDepthFunc(GLenum)`, `SetStencilOp(GLenum, GLenum, GLenum)`,
  `CreateTexture2D(u32, u32, GLenum internalFormat)`, and a dozen more. The
  facade is not leaky at the edges; GL is part of its declared vocabulary.
- **`RendererAPI.h` itself includes `<glad/gl.h>`,** so it hands the entire GL
  API to every translation unit that includes it. That is why only 3 of the 42
  GL callers need a direct `glad` include — the rest get it transitively. **This
  dictates Phase 2's ordering** (§1.7): until `RendererAPI.h` is clean, deleting
  a `#include <glad/gl.h>` from a pass proves nothing, because the symbols are
  still visible.

---

## 1. Decision — the neutral resource/binding model

### 1.1 One `u32` currently conflates three things; split it into three

The boundary currency today is `u32 GetRendererID()`, plus its aliases
(`using RendererID = u32` in `Commands/RenderCommand.h`,
`TransientPool::AcquiredInfo::RendererID`,
`RGCommandContext::ResolveTexture() -> u32`, `BindTexture(u32 slot, u32 id)`).
A GL name is simultaneously an object identity, a thing you can hand to any GL
entry point, and — via the texture unit it is bound to — most of the story of
how a shader reaches it.

Vulkan separates all three, and `VK_EXT_descriptor_heap` separates them
*further*: what the shader indexes is not the image at all, it is an offset into
the resource heap where a descriptor for one *view* of that image lives, and the
descriptor's lifetime is not the image's lifetime.

**The failure mode to avoid in Phase 2 is replacing `u32 RendererID` with a
single opaque `RHIHandle`.** That is a rename, not an abstraction: it keeps the
conflation and buys nothing, and Phase 5 would have to sweep again. The model is
therefore three distinct types with three distinct visibilities:

| Concept | Type | Who may see it |
| --- | --- | --- |
| **Resource identity** — "which GPU object is this?" | `RHI::ResourceHandle { u32 Index; u32 Generation; }` | Everyone. This is the engine-wide currency. |
| **View identity** — "which *way of looking at* that object?" | `RHI::ViewHandle { u32 Index; u32 Generation; }` | Everyone. One resource, many views — this is what owns a heap slot. |
| **Binding address** — "what integer does the shader index with?" | `RHI::HeapOffset { u32 Value; }`, obtained *only* via `Heap::OffsetOf(ViewHandle)` | Everyone, and it is *shader-visible data* — it goes into a UBO/SSBO field. Fetch it; never store it (§1.2). |
| **Native handle** — "what do I pass to the driver?" | backend-private (`GLuint`, `VkImage`, …) | `Platform/<Backend>/` only. |

**Does bindless still need a "binding address" at all?** Yes — bindless is what
makes it a *first-class* concept rather than an implicit one. Today the binding
address is a compile-time constant duplicated in two places: `TEX_SHADOW_ATLAS = 13`
in `ShaderBindingLayout.h` (one of 60 such `TEX_*` constants) and
`layout(binding = 13)` in the GLSL, re-established every frame by a bind call.
Under heap-bindless the shader instead does
`texture(g_ResourceHeap[material.albedoOffset], uv)`, and that offset is
allocated at runtime and must be *transported* into a UBO. What dies is the
**act of binding** — the ~173 `BindTexture` calls and the `TEX_*` constants.
What survives, promoted from constant to data, is the number itself.

It cannot simply *be* the resource identity, for four reasons that are all
already live in this codebase: one resource maps to many views and therefore
many offsets (`CreateDepthArrayCompareOffView` is the existing proof); a
persistent view's offset is stable for the object's life while a transient's is
re-allocated every frame from the ring; `HeapOffset` must be a bare `u32` a
shader can index with, whereas `ResourceHandle` is 64 bits and the generation is
the entire point of it; and a resource that is only ever a colour attachment
needs no heap slot at all, so fusing them would size the heap by resource count.

`RHIResourceHandle` deliberately mirrors the `{ Index, Generation }` shape the
render graph already uses (`RGTextureHandle` / `RGBufferHandle` /
`RGFramebufferHandle` in `Renderer/ResourceHandle.h`), so the graph's existing
handle discipline generalises instead of acquiring a second, competing scheme.
The generation counter is not decoration: GL recycles names, so today two
different objects can compare equal via `Texture::operator==`
(`GetRendererID() == other.GetRendererID()`) if one was deleted and the other
created. A generation catches that; a raw `u32` cannot.

`RHIHeapOffset` is a plain `u32` wrapper *on purpose*. It has to survive being
written into a UBO and read by GLSL as an array index, so it cannot be an opaque
type. Wrapping it in a struct only buys type-safety at the C++ boundary — that
is the point, but it must stay layout-compatible with `u32`.

### 1.2 Heap slots are owned by *views*, and follow the two lifetime classes the engine already has

Under `VK_EXT_descriptor_heap` there is one resource heap and one sampler heap.
Because samplers live in a separate heap, a texture does not need a slot per
sampler — it needs a slot per **view**, where a view is
`(resource, subresource range, format reinterpretation)`. The engine already has
this concept in two places: `RGSubresourceRange` (mip/layer/slice) and
`RendererAPI::CreateDepthArrayCompareOffView`, which exists precisely because
one depth array needs to be reachable both as `sampler2DArrayShadow` and as a
plain `sampler2DArray`.

**Decision: the heap slot is owned by the view, and slot lifetime follows the
resource's existing lifetime class.** The engine already has exactly two, and
they are already separated in code:

- **Persistent** (`Ref<Texture2D>` assets, materials, IBL maps, DDGI atlases) —
  one slot per view, allocated at view creation, freed at view destruction. The
  offset is stable for the object's life, so it can be baked once into material
  data and never touched again. That stability *is* the performance argument for
  bindless; anything that re-allocates persistent slots per frame gives it back.
- **Frame-transient** (everything from `TransientPool`) — slots come from a
  per-frame ring that resets in `TransientPool::ReleaseAll()`, which already
  exists and already runs at exactly the right moment (end of frame, after
  execution).

Do **not** invent a third lifetime class for descriptors. The reason is
`docs/agent-rules/render-graph-transient-aliasing.md`: `TransientPool` hands the
*same* physical object to two logical resources with disjoint lifetimes, and
`WriteNewVersion` is a *rename* of one physical resource, not an allocation. If
transient slots were persistent, an aliased pair would need one offset rewritten
mid-frame — the exact stale-read archetype that document warns about, and one
that LIFO pool reuse hides in steady state. With a per-frame ring, each
acquisition gets its own offset, and a version rename gets a second offset
pointing at the same physical object. That is not a bug, it is the correct
model, and it makes the alias *visible in the heap* (two offsets, one object)
instead of invisible.

Slot allocation is **explicit and observable**, never implicit-on-first-use, for
the same reason. The heap gets a poison mode mirroring `OLO_RG_POISON_TRANSIENTS`
— a freed slot is overwritten with a descriptor for a known-bad resource, so a
use-after-free renders as a deterministic, obviously-wrong result rather than
whatever the previous tenant left behind.

**A `HeapOffset` is fetched, never stored.** The only way to obtain one is
`Heap::OffsetOf(ViewHandle)`, which validates the view's generation first. A
loose `u32` cached in a material or a pass member is a stale-offset bug waiting
to happen — persistent offsets are stable, but a `FrameTransient` one is
re-allocated every frame, and under `WriteNewVersion` aliasing one physical
object legitimately holds two offsets within a single frame. Fetching at the
point of write turns that into a generation compare on the CPU.

That is cheap insurance against an expensive failure. The consequence of a bad
descriptor under this model is **not** "samples the wrong resource" — DXVK's
experience with the equivalent descriptor-buffer model is that "if you screw up,
you will likely hang your GPU and have all sorts of fun trying to debug that."
An engine-side generation check that fires before the descriptor ever reaches
the GPU is worth more than any amount of after-the-fact instrumentation.

Poison-on-free stays worth building even though Vulkan SDK 1.4.357.0 (2026-07-28)
turned on GPU-assisted validation for `VK_EXT_descriptor_heap` — see ADR 0010's
"Tooling floor" note. The two instruments cover different halves: GPU-AV catches
a shader indexing a slot it should not, while poison catches *engine-side slot
lifetime* — a slot freed while something still holds its offset, which is
well-formed indexing into a stale descriptor and therefore not a validation
error at all. That is the failure mode the transient ring makes possible, so it
is the one this model has to instrument itself. GPU-AV also does not run on the
OpenGL backend, where the Phase 3 `ARB_bindless_texture` rehearsal happens.

### 1.2a The sampler heap is a second heap, and the engine has nothing to port into it

`VK_EXT_descriptor_heap` gives you exactly two heaps: one for resources, one for
samplers. §1.2 covers the resource heap. The sampler heap needs saying
separately, because it is the one place where the engine has **no existing
concept to migrate** rather than a GL-shaped one to convert.

On OpenGL today, filter and wrap state live *on the texture object*
(`glTextureParameteri`), so there is no shareable "sampler" anywhere in the
engine — every texture carries its own. Under a split heap that would mean one
sampler-heap slot per texture, when in reality a few distinct sampler
configurations serve hundreds of textures. **Phase 3/4 therefore has to
introduce sampler deduplication that has no GL counterpart** — `RHI::SamplerDesc`
exists in the header for that, as a value type deliberately cheap to hash and
compare.

**Immutable / embedded samplers are explicitly not modelled.** Descriptor-heap
makes them markedly more complicated (for driver authors too, per DXVK's
account), the engine does not use them, and adding them speculatively buys
nothing. It is recorded here rather than silently omitted because *middleware*
can want them, and if any ever does it becomes a sampler-heap management problem
— a Phase 4 device-bring-up checklist item.

One layout consequence worth fixing now, because it makes `HeapOffset`-as-index
concrete: the heap is addressed as a **flat array with a single uniform stride**
— the largest descriptor size the backend needs — so a slot index becomes a byte
offset by multiplication. Uniform stride wastes a little memory on small
descriptor types and is the standard trade; DXVK's summary of full bindless
under this model is exactly *"use the size of the largest descriptor type that
you need as an array stride, index into the heap in your shader, allocate
memory, bind heap, done."* Per-type strides are the alternative, and they buy
memory at the cost of making every shader-side index type-dependent — not worth
it here.

### 1.3 `GetRendererID()` is deleted, and its three callers get three differently-named APIs

`GetRendererID()` does not get one replacement, because it serves three callers
with genuinely different needs. Giving them one API is what allowed GL to spread
this far in the first place.

1. **Passes binding a texture for sampling** — **173** `BindTexture(...)` sites
   in 47 `.cpp` files (the issue's "~125 across ~35 passes" undercounts; a
   handful of the 173 are the facade's own definitions rather than pass calls).
   Under heap-bindless there is no slot to bind to: the pass
   writes `texture->GetHeapOffset()` into its UBO and the shader indexes the
   heap. `BindTexture` does not get abstracted, it **disappears**. This is the
   single biggest reason Phase 3's `ARB_bindless_texture` rehearsal is worth
   doing — it forces the GLSL-side and UBO-side change on the backend we can
   still debug easily, before any Vulkan-specific effort.
2. **Debug / introspection tools** (`Renderer/Debug/`, MCP capture) — these
   genuinely need the native object. They get `RHIResourceHandle` for identity,
   plus an escape hatch that is **named to be conspicuous**:
   `RHI::GetNativeHandleForDebug(RHIResourceHandle) -> RHINativeHandle`
   (a `u64` plus a backend tag). The naming is the mechanism: a
   `GetNativeHandleForDebug` call sitting in a render pass is a self-evident
   smell in review, where `GetRendererID()` reads as ordinary. The ratchet
   counts uses of this escape hatch outside `Renderer/Debug/` and
   `Platform/`, with a baseline of zero (§1.7).
3. **`TransientPool` bookkeeping** — `AcquiredInfo::RendererID` only ever answers
   "did these two plan entries get handed the same object?". `RHIResourceHandle`
   answers that *better*, because the generation distinguishes a recycled name
   from a genuine alias — which is exactly the distinction the transient-plan
   MCP tooling was built to investigate.

### 1.4 `TransientPool` describes a physical resource without a `u32`

`TransientPool::AcquiredInfo::RendererID` becomes `RHIResourceHandle Handle`.
Nothing else in that struct changes: `Kind`, `Width`, `Height`, `SizeBytes` are
already neutral. `BucketInfo` is already neutral (its `Format` is
`ImageFormat as u32`, an engine enum, not a `GLenum`).

`TextureDescriptorKey` — the pooling key — is already backend-neutral and needs
no change, which is the strongest available evidence that the pool's *aliasing
model* is sound and only its *identity plumbing* is GL-shaped.

### 1.5 Barriers: the usage pair is the neutral truth; `MemoryBarrierFlags` becomes a GL lowering

`RenderGraph::ResourceTransition` already carries almost exactly a Vulkan
barrier: `ResourceName`, `ProducerPass`, `ConsumerPass`, `FromUsage`, `ToUsage`,
`Range`, and cross-lane metadata (`IsCrossLane`, `ProducerLane`, `ConsumerLane`).
The barrier planner is already documented as backend-agnostic and already
computes write-after-write barriers. This is real RHI-shaped thinking that is
mostly right.

What is wrong is that `MemoryBarrierFlags` — a `glMemoryBarrier` bitmask — has
been promoted to the neutral currency. It sits in `PlannedBarrier::Flags`, in
`ResourceTransition::Flags`, in `RGCommandContext::MemoryBarrier(flags)`, and in
`RendererAPI::MemoryBarrier(flags)`.

**Decision: `(FromUsage, ToUsage, Range, lanes)` is the neutral truth.
`MemoryBarrierFlags` is demoted to a GL lowering artefact.**
`ResolveProducerBarrierFlags` / `ResolveConsumerBarrierFlags` move into
`Platform/OpenGL/` as *that backend's* lowering; a Vulkan backend writes its own
lowering to `(srcStageMask, srcAccessMask, oldLayout) → (dstStageMask,
dstAccessMask, newLayout)`.

**The neutral model deliberately does not carry an image layout.** Layout is a
pure function of usage — `ShaderSample → SHADER_READ_ONLY_OPTIMAL`,
`RenderTarget → COLOR_ATTACHMENT_OPTIMAL`,
`DepthStencil → DEPTH_STENCIL_ATTACHMENT_OPTIMAL`, `ShaderImage → GENERAL`,
`TransferDest → TRANSFER_DST_OPTIMAL` — so the Vulkan backend derives it. Adding
a layout enum to the graph would leak Vulkan upward for no gain.

**One concrete defect found while validating this, which Phase 5 must fix:**
`ResourceTransition` types the pair as `RGWriteUsage FromUsage` →
`RGReadUsage ToUsage`. It therefore *structurally cannot express a write→write
transition*. The planner does emit WAW barriers
(`RenderGraphBarrierPlanner.cpp`, the "WAW: emit a barrier for every prior
writer whose range overlaps" branch), but when `BuildResourceTransitions` then
looks for a matching **read** declaration on the consumer and finds none, it
silently falls back to `t.ToUsage = RGReadUsage::ShaderSample`. On GL this is
harmless, because the flags come from `barrier.Flags`, which the planner derived
correctly from the *write* usage — the bogus `ToUsage` is never read. On Vulkan
a backend deriving `(dstStageMask, dstAccessMask, newLayout)` from `ToUsage`
would transition a storage-image WAW into `SHADER_READ_ONLY_OPTIMAL` with a
read-only access mask: **wrong layout, wrong access, and silent**. This is the
canonical shape of the bug class Phase 1 exists to catch — a neutral-looking
record whose neutrality is only accidental, held up by a GL-specific field
alongside it.

The fix is a **single unified access enum** (`RHIAccess`) covering reads,
writes, and the "no access" initial state, replacing the read/write enum pair in
the transition record. `RGReadUsage` / `RGWriteUsage` may stay as the *builder's*
declaration vocabulary (passes genuinely do declare reads and writes
separately); it is the *transition* record that must be unified.

Two smaller gaps to resolve in Phase 5, recorded here so they are not
rediscovered: `RGWriteUsage::Clear` is ambiguous between clear-as-load-op and
`vkCmdClearColorImage` and needs splitting; and `ProducerPass = "external"`
currently defaults `FromUsage` to `RenderTarget`, which for a genuinely
first-use-this-frame resource should be an `Undefined` access (letting Vulkan
discard the contents) rather than a colour-attachment transition.

### 1.6 `Renderer/Debug/` relocates; it is not exempt

The 236 GL calls in `Renderer/Debug/` are tempting to exempt: a state guard and
a resource inspector are, in a sense, legitimately GL-specific. **They are not
exempt, because of `CLAUDE.md`'s own rendering-verification rule.** "Do not
report a rendering change as done on the strength of unit tests alone" is
enforced by exactly these tools plus the MCP capture endpoints they back
(`olo_render_capture_target`, `olo_render_transient_plan`). If they stay
GL-only, every Vulkan pass in Phase 7 is unverifiable by the loop the repo
mandates — which is what #691 Phase 8 is already warning about.

So they are a *third* category, distinct from both "sweep to zero" and
"legitimately native": **neutral interface in `Renderer/Debug/`, GL
implementation physically relocated to `Platform/OpenGL/`.** They get their own
ratchet counter with their own baseline (§1.7), so their progress is visible but
does not block Phase 2's headline number from reaching zero.

`GLStateGuard` is the interesting member and warrants a Phase 8 note:
`docs/agent-rules/gl-clear-program-revalidation.md` exists because of an
NVIDIA-specific `glClear` revalidation hazard, and
`docs/agent-rules/render-pass-published-state.md` exists because a blanket
`GLStateGuard(Restore)` silently reverted engine-global bindings. Both are
*OpenGL global-state* problems. Vulkan has no equivalent global state to guard,
so `GLStateGuard`'s neutral interface should be a no-op on Vulkan rather than a
port — and the `RendererAttachedTest` fixture's `GLStateGuard(Restore)` wrapper
needs a backend-conditional, not a translation.

### 1.7 The Phase 2 contract, and what the ratchet number means

Three counters, all monotonically non-increasing, all baselined in
`OloEngine/tests/Rendering/rhi_boundary_baseline.json`:

| Counter | Baseline | Target | Phase |
| --- | ---: | ---: | --- |
| `sweep_gl_calls` — GL calls in `OloEngine/` minus `Renderer/Debug/` | 313 | **0** | 2 |
| `sweep_glad_includes` — files there including `<glad/gl.h>` | 70 | **0** | 2 |
| `tools_gl_calls` — GL calls in `OloEngine/Renderer/Debug/` | 236 | 0 (relocated) | 8 |

`sweep_glad_includes` is the counter that actually *proves* the property. A call
count is a progress measure that a clever workaround can game (wrap the call in
a helper that still lives in `Renderer/`); a translation unit that cannot see
`glad/gl.h` provably cannot name a GL symbol. When it reaches zero, Phase 2 is
done in a way no grep can be wrong about.

**Ordering consequence, and it is the non-obvious one:** because
`RendererAPI.h` includes `<glad/gl.h>` and every pass includes `RendererAPI.h`,
the include counter is *not yet meaningful*. Removing `#include <glad/gl.h>`
from `BloomRenderPass.cpp` today changes nothing — the symbols still arrive
transitively, so the file still compiles and the boundary is still open. **Strip
`GLenum`/`GLuint`/`GLint` out of `RendererAPI`'s virtuals first** (that is
already a listed Phase 2 task; this makes it the *first* one, not a parallel
one). Only then does each removed include become a real, compiler-enforced
guarantee — and at that point the other 40 zero-call includers fall out almost
mechanically, because they only ever wanted `GL_SRC_ALPHA` and friends.

The neutral replacements for those `GLenum` parameters are ordinary engine enums
in an `OloEngine::RHI` namespace — `RHI::BlendFactor`, `RHI::CompareOp`,
`RHI::StencilOp`, `RHI::CullMode`, `RHI::PolygonMode`, `RHI::Format` — declared
in `RHITypes.h`. Namespaced rather than `RHI`-prefixed so they read as
`RHI::CompareOp::Less` at the call site, and so `RHI::ResourceHandle` cannot be
confused with the render graph's existing `OloEngine::ResourceHandle`. They are
not novel; they are the same enums every RHI has. The value of declaring them
now is that Phase 2 has a fixed target to sweep *toward* rather than inventing
one file at a time.

### 1.8 What Phase 1 does not do, and who picks each item up

**Nothing here is abandoned.** Every item below lands inside issue #691's
roadmap; this section says *which phase owns it* and, more importantly,
distinguishes three states that are easy to confuse:

**(a) Never in Phase 1's scope.** Ordinary later phases of the roadmap. Phase 1
is "no code motion" by the issue's own definition, so these are listed only so a
reader does not go looking for them here.

| Item | Owner |
| --- | --- |
| The ~313-call-site sweep itself | Phase 2 |
| `ARB_bindless_texture` rehearsal on OpenGL | Phase 3 |
| Vulkan SDK / volk / VMA vendoring; any `VkXxx` code | Phase 4 |

**(b) Decided *here*, implemented later.** The decision is made in this ADR — a
later phase carries it out. Re-litigating these needs an ADR amendment, not just
an implementation choice.

| Item | Decided in | Owner | What breaks if it is skipped |
| --- | --- | --- | --- |
| Unify `ResourceTransition`'s read/write enum pair into `RHI::Access` | §1.5 | Phase 5 | A storage-image write→write barrier lowers to `SHADER_READ_ONLY_OPTIMAL` with a read-only access mask. Silent on GL, wrong on Vulkan. |
| Split `RGWriteUsage::Clear` into load-op vs transfer clear | §1.5 | Phase 5 | An explicit clear misses its `TRANSFER_DST` transition. |
| `ProducerPass == "external"` should mean `Access::Undefined`, not `RenderTarget` | §1.5 | Phase 5 | Vulkan preserves contents it could have discarded — a silent perf loss, not a correctness bug. |
| Relocate `Renderer/Debug/` GL code to `Platform/OpenGL/` behind a neutral interface | §1.6 | Phase 8 | Phase 7 cannot be verified under CLAUDE.md's rendering rule, because the capture/inspect tools are OpenGL-only. |
| Strip `GLenum`/`GLuint` from `RendererAPI`'s virtuals **first**, before any per-file include removal | §1.7 | Phase 2 | The `sweep_glad_includes` ratchet measures nothing, because the symbols still arrive transitively. |

**(c) Identified here, deliberately *not* decided.** Phase 1 established that
these are needed and left the mechanism open, because choosing it without an
implementation to test against would be guessing.

| Item | Raised in | Owner | Why it is open |
| --- | --- | --- | --- |
| Sampler deduplication + the sampler-heap allocator | §1.2a | Phase 3/4 | The engine has no shareable-sampler concept to port — on GL, filter/wrap state lives on the texture object. There is nothing to convert, so the design should follow the first real Vulkan sampler-heap usage rather than precede it. |
| Whether `GPUResourceInspector` surfaces `RHI::ResourceHandle` + a native handle, or a backend-tagged opaque blob | §1.6 | Phase 8 | Depends on what the Vulkan capture path can actually produce. |

The one genuinely open *risk* — not a deferral — is re-verifying RDNA2 driver
support before Phase 4 commits (ADR 0010's "Correction" section).

### 1.9 Outside corroboration, and the one thing it changed

The model above was designed against this codebase rather than against other
engines' write-ups, so it is worth recording where an experienced outside
account agrees and where it did not. Philip Rebohle (DXVK maintainer) has
shipped legacy bindful, descriptor-indexing, descriptor-buffer *and*
descriptor-heap in production; his comparison (written ~January 2026 — see the
dating caveat at the end of this section) lines up with the choices here on
every point that touches this ADR:

- **Heap+offset is the right shape.** *"Full bindless is trivial and barely
  requires any setup code. Use the size of the largest descriptor type that you
  need as an array stride, index into the heap in your shader, allocate memory,
  bind heap, done."* — §1.2a adopts exactly that layout.
- **Descriptor buffer would have been the wrong intermediate step.**
  *"Fundamentally, descriptor buffers are just VkDescriptorSet with extra
  steps"*, carrying the descriptor-indexing restrictions forward. ADR 0010 ruled
  it out as a fallback; this is independent support for that, not just for the
  destination.
- **View API objects mostly disappear; view *identity* does not.** DXVK replaced
  `VkImageView` with a descriptor blob and *"manage[s] things in more or less the
  same way as before"*. That is precisely why `ViewHandle` is engine-owned
  (§1.1) — the API stops owning the concept, so we must.
- **The failure mode is worse than "renders wrong".** *"You pretty much need
  GPU-assisted validation to figure out what you're screwing up, and if you screw
  up, you will likely hang your GPU."* This is why §1.2's fetch-don't-store rule
  and poison-on-free are load-bearing rather than nice-to-have. Note this is a
  property of the *model*, not of tooling maturity — it does not expire.

**Dating caveat, and why it matters here.** That write-up is from ~January 2026,
and it was re-checked on 2026-07-30 before being leaned on. Its two "con" items
have aged in opposite directions, so treat them differently:

- **Tooling — expired.** *"Lack of (mature) validation, RenderDoc support etc.
  Of course this will improve over time."* It did, within six months: SDK
  1.4.341.0 brought CPU-side validation for the extension and 1.4.357.0
  (2026-07-28) brought GPU-AV plus a GPU Dump layer. Do not carry this objection
  forward — ADR 0010's tooling-floor note supersedes it.
- **Driver support — held, and independently corroborated.** RDNA1/RDNA2 still
  did not get the extension in the Adrenalin branch that shipped it, there is an
  open AMD tracker for it, and DXVK 3.0 now advises that hardware class off
  Windows entirely. See ADR 0010's "Correction" section.

**The one correction it forced** is therefore in ADR 0010, not here: the driver
floor is narrower than that ADR's per-vendor list implied. It strengthens §2's
runtime-selection decision rather than weakening it — with a floor that narrow,
the OpenGL fallback has to be present on the player's machine, which is only
possible if both backends live in one binary.

---

## 2. Decision — backend selection is a **runtime switch**, not a CMake preset

**Decision: one binary contains every compiled-in backend; the choice is made at
process start from `--rhi=opengl|vulkan`, falling back to a config setting,
falling back to OpenGL. The editor exposes it as a dropdown that takes effect on
restart. There is no `vulkan` CMake preset that changes which backend runs.**

The issue's Phase 4 line says "new `vulkan` CMake preset". That conflates two
genuinely separate axes, and this ADR separates them:

- **Build-time — `OLO_WITH_VULKAN` (CMake option, default `ON` on desktop):**
  *is the Vulkan backend compiled in at all?* This is needed, because the SDK +
  volk + VMA vendoring is heavy, and `docs/agent-rules/asset-import-usd-alembic.md`
  already records what heavyweight vendored dependencies cost this build. It
  also lets `OloServer` (the WSL2/headless target) and a lean CI configuration
  build without a Vulkan SDK present. This is an *availability* switch.
- **Run-time — `--rhi=`:** *which compiled-in backend does this process use?*
  This is a *selection* switch, and it is the one the user interacts with.

A preset as the selection mechanism was rejected on three grounds. It doubles
the build matrix for the entire duration of a roadmap whose Phase 7 is explicitly
"the long tail". It makes the golden-image parity gate — the primary safety net
for Phases 2 and 7 — require building and shipping two binaries to compare, when
the natural form of that test is one binary rendering the same scene twice.
And it forces the same split onto shipped games, so `GameBuildPipeline` would
have to pick a backend at cook time for hardware it cannot see, which directly
contradicts ADR 0010's accepted consequence that Vulkan has a narrower hardware
floor: the fallback to OpenGL has to be available *on the user's machine*, which
means both backends in one binary.

**On "swap interactively":** most engines do behave the way the intuition
suggests, but the swap is startup-scoped, not live. Unreal takes `-vulkan` /
`-d3d12` at launch (the project setting requires a restart); Unity's Player
Settings graphics-API list restarts the editor when reordered; Godot takes
`--rendering-driver vulkan|opengl3`. None hot-swap a running device. What reads
as "interactive" is really "choosable without recompiling, applied on restart" —
which is exactly what is decided above.

A genuinely live, mid-session swap is out of scope, and in this codebase
specifically it is not a small extra step:

- `GLFW_CLIENT_API` must be `GLFW_OPENGL_API` or `GLFW_NO_API` **before**
  `glfwCreateWindow`. Swapping means destroying and recreating the OS window,
  which takes the ImGui context, the docking layout, and the editor's viewport
  framebuffers with it.
- Every GPU resource is backend-owned (`Ref<Texture2D>` *is* an
  `OpenGLTexture2D`). A live swap means re-creating all of them, and for
  GPU-only resources — transient targets, HZB pyramids, DDGI atlases,
  virtual-geometry pages, FFT ocean cascades — there is no retained CPU-side
  source to recreate them *from*. It is a full asset-system reload wearing a
  different hat.
- The editor's ImGui backend is `imgui_impl_opengl3`, including its font atlas.

None of that buys anything a two-second restart does not.

**The ordering contract, which is the part that can silently go wrong.**
`RendererAPI::s_API` is today a static initialised to `OpenGL` and *never
written by anything*. The runtime switch gives it a real setter, and that setter
has a hard ordering requirement:

`RendererAPI::s_API` must be set **before `Window::Create`**.

This is not hypothetical: `WindowsWindow::Init` already calls
`Renderer::GetAPI()` before `glfwCreateWindow` (to decide
`GLFW_OPENGL_DEBUG_CONTEXT`), and `Application`'s constructor calls
`Window::Create` at line 62 but `Renderer::Init` only at line 72. So the seam
already exists and is already load-bearing — the setter belongs in
`Application`'s constructor, parsed from `ApplicationCommandLineArgs` (which
already has a `Contains()` helper for exactly this kind of mode flag), before
line 62. Any code that reads `Renderer::GetAPI()` during static initialisation
will see `OpenGL` regardless of the flag; nothing does today, and nothing
should start.

Device selection then gates hard per ADR 0010: if `--rhi=vulkan` is requested
and `VK_EXT_descriptor_heap` is unavailable, **refuse to initialise with a clear
message** rather than silently degrading. Falling back to OpenGL automatically
would reintroduce, at runtime, precisely the "silent fallback" that ADR 0010
forbids at compile time — the user asked for Vulkan and must be told they did
not get it. The default-when-unspecified staying OpenGL is a different thing and
is fine.

---

## 3. Decision — PSO cache: reuse the SPIR-V tier unchanged, add one `VkPipelineCache`, invalidate through a shader→pipeline reverse index

### What exists today

The GL shader path has three cache tiers, all under `Utils::GetCacheDirectory()`:

1. `<shader>.cached_vulkan.<stage>` — SPIR-V from shaderc, currently targeting
   `shaderc_env_version_vulkan_1_2`.
2. `<shader>.cached_opengl.<stage>` — GLSL cross-compiled back from that SPIR-V
   by SPIRV-Cross, purely so it can run on GL.
3. `<shader>.cached_opengl.pgr` — the linked GL program binary, guarded by a
   side-car driver stamp (`program_binary_driver_stamp.txt`, maintained by
   `SyncProgramBinaryCacheDriverStamp`).

Tiers 1 and 3 are invalidated by **mtime** against the source *and every
transitive include* (`OpenGLShader::IsCacheStale` walks `m_IncludedFilePaths`).
Tier 3 additionally **soft-fails**: a rejected `glProgramBinary` logs and falls
back to recompilation, because vendor behaviour here is unreliable enough that
the code carries two Mesa/radeonsi crash workarounds.

Hot reload is `OpenGLShader::Reload()`: re-read, recompile, and `FinalizeProgram`
assigns a **fresh `glCreateProgram()` handle to `m_RendererID`** on the same
`Shader` object. Consumers hold `Ref<Shader>` and re-read the id via `Bind()`, so
object identity is preserved and nothing needs re-pointing.

### The decisions

**(a) Tier 1 is shared verbatim; Vulkan adds no new shader-source cache tier.**
SPIR-V is driver-independent, so the artefact the GL path already produces *is*
the artefact Vulkan consumes. Vulkan **deletes** tier 2 for its own backend
rather than adding anything. This was already ADR 0010's core argument for
Vulkan over D3D12; it is restated here as a cache-design commitment so Phase 6
does not quietly introduce a parallel `.cached_vk` tier.

**(b) The SPIR-V cache key must include the shaderc target environment.**
`vulkan_1_2` is hard-coded today. A `VK_EXT_descriptor_heap` backend will need a
newer target env — per ADR 0010 the extension is a Vulkan 1.4-era addition,
years past 1.2 — and tier 1 is a *shared* artefact, so bumping it in place would
silently change the SPIR-V that the GL path cross-compiles, putting the entire
OpenGL backend's shader pipeline (and the SPIRV-Cross round-trip that
`tests/Fuzzing/FuzzSpirvCross.cpp` pins as production behaviour) at risk for a
Vulkan-only reason. Encoding the target env in the cache filename makes the two
consumers independent: the GL path keeps whatever env it is validated against,
Vulkan uses its own, and bumping either is a local decision. This is cheap now
and very awkward later.

**(c) One process-wide `VkPipelineCache`, serialised to
`<cache>/pipeline_cache.vkpc`, with *no* side-car driver stamp.** The GL stamp
file exists because `glProgramBinary`'s rejection behaviour is vendor-dependent
and, on some drivers, crash-prone. Vulkan's contract is different and better:
the blob carries `VkPipelineCacheHeaderVersionOne` with vendor ID, device ID and
`pipelineCacheUUID`, and the driver is *required* to safely ignore a blob it
does not recognise. Hand-rolling a stamp would be reimplementing a guarantee the
API already gives. Keep the soft-fail *posture* (a bad cache costs compile time,
never correctness); drop the mechanism.

**(d) Hot reload needs a shader→pipeline reverse index, because the invalidation
granularity is fundamentally different — this is the constraint that shapes
Phase 6.** On GL, a `VkPipeline`'s worth of state is dynamic: blend, depth,
raster, stencil and the vertex layout are all set by separate calls, so a shader
reload produces exactly **one** new program object and everything downstream
keeps working. On Vulkan, all of that state is **baked into the pipeline**
alongside the SPIR-V, so one shader is the source of **N** pipelines — one per
state permutation, render-pass/attachment-format combination and vertex layout
it has been used with. A reload must invalidate every one of them.

So `Shader::Reload()` cannot stay a purely per-object operation on Vulkan. The
backend needs a `shader identity → {pipeline handles}` multimap, maintained at
pipeline-creation time, and `Reload()` becomes: recompile SPIR-V → look up every
dependent pipeline → destroy-and-forget them → let the next bind recreate
lazily. **Lazy, not eager**: eager recreation of every permutation on every
save would turn the iteration loop the shader hot-reload exists to serve into a
multi-second stall, and most permutations are not used in the frame being
iterated on. The lazily-recreated pipeline hits the `VkPipelineCache` for
everything that did not actually change, so the recreation is cheap.

Deferred destruction is mandatory: a pipeline being replaced may be referenced
by command buffers still in flight, so destruction goes through the existing
in-flight-frame retirement machinery (`InflightFrameManager`), not an immediate
`vkDestroyPipeline`.

**(e) Keep mtime + transitive-include staleness; do not switch to content
hashing.** `IsCacheStale` already walks `m_IncludedFilePaths`, and
`docs/agent-rules/glsl-shaders.md` plus the include-processing in `PreProcess`
mean the include set is known accurately. Content hashing is strictly better in
principle and would also fix the "touch a file, recompile the world" case — but
it is an orthogonal improvement that would change GL behaviour for a Vulkan
reason, and this ADR's whole posture is to avoid that. Recorded as a possible
follow-up, not a Phase 6 dependency.

---

## Consequences

- The renderer carries **four** boundary concepts where it carries one today
  (resource identity, view identity, binding address, native handle). That is
  more types, and it is the point: the compiler can then tell a pass author that
  a native handle is not a binding address. Phase 2 is where that cost is paid.
- **Phase 1 leaves eight named follow-ups, all inside #691** — five decided here
  and awaiting implementation, two identified but deliberately left open, one
  re-verification. §1.8 tables them with owning phase and the failure mode for
  each. None of them is optional; they are the parts of this design that a later
  phase has to carry, and the table exists so they cannot be quietly dropped.
- **Phase 2 must strip `RendererAPI`'s `GLenum`/`GLuint` virtuals before
  anything else**, or the include ratchet measures nothing. This is an ordering
  constraint the roadmap did not previously state.
- `Renderer/Debug/` (236 calls, 43% of the total) is Phase 8 work that
  Phase 2 explicitly does not have to carry — but it is *relocation*, not
  exemption, and Phase 7 cannot be verified per `CLAUDE.md`'s rendering rule
  until it is done.
- `ResourceTransition`'s read/write enum pair is a known, recorded defect for
  Vulkan (§1.5). It is currently harmless and must not be "cleaned up" into
  something that merely looks neutral.
- One binary ships both backends. Binary size and link time grow; `OLO_WITH_VULKAN=OFF`
  is the escape valve for `OloServer` and lean CI.
- Vulkan adds exactly one new on-disk cache artefact (`pipeline_cache.vkpc`) and
  changes the *name* of an existing one (the target-env-keyed SPIR-V). Existing
  caches are invalidated once, at the version that lands (b); this is a
  cold-start cost, not a correctness issue.
- Nothing here is load-bearing until Phase 2 begins. If Phase 2 discovers the
  three-way identity split is wrong, the cost of revisiting is this document
  plus a header — which is exactly why Phase 1 was worth doing before the sweep,
  and not after.
