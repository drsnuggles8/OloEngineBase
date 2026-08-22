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

**§4, §5, and §6 were added later (2026-08-08, ahead of Phase 6), not part of
the original Phase 1 output.** They decide the root-data model (§4, including
its GPU-driven-indirect consequence in §4.2), PSO permutation minimization
(§5), and split-barrier/timeline-semaphore signaling (§6) the same way §1–§3
decided Phase 2/3's shape before those phases started — pre-deciding a later
phase from this ADR is the established pattern, not a new one. All three were
prompted by cross-referencing Sebastian Aaltonen's *Reducing Graphics API
Complexity* (2026) against this document. §4/§4.2 close a gap: §1.2 already
assumed heap-bindless *texture* binding, but left buffer binding (UBO/SSBO)
and indirect-args staging on the conventional, CPU-round-tripping path. §6
closes a different gap: §1.5 designed same-submission barriers but never
addressed cross-pass/cross-frame latency hiding.

**No code motion.** This ADR ships alongside declaration-only headers under
`OloEngine/src/OloEngine/Renderer/RHI/` (the vocabulary, no implementations) and
a coverage ratchet (`OloEngine/tests/Rendering/RHIBoundaryRatchetTest.cpp`).
Nothing is converted; no `Ref<T>` changes; no `VkXxx` anywhere.

---

## 0. The survey the design is built on

Phase 1 measured the boundary before designing against it: **549 raw
`glXxx()` calls in 42 files outside `Platform/OpenGL/`** (the issue's ~620
and the handover's 724 were both wrong — the larger number matched `glfw*`
window calls, which are not GL). The bind-site count in §1.3 is corrected by
the boundary doc; the method, the three published numbers and why a call count
is the wrong measure at all are in
[rhi-abstraction-boundary.md](../agent-rules/rhi-abstraction-boundary.md) §1–§2.
The counters this survey motivated all reached 0 (§1.7).

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
| **Binding address** — "what integer does the shader index with?" | `RHI::HeapOffset { u32 Value; }`, obtained *only* via `Heap::OffsetOf(ViewHandle)` | Everyone, and it is *shader-visible data* — it goes into a UBO/SSBO field. Cacheable for persistent views, never held across a use for transients (§1.2). |
| **Native handle** — "what do I pass to the driver?" | backend-private (`GLuint`, `VkImage`, …) | `Platform/<Backend>/` only. |

**Does bindless still need a "binding address" at all?** Yes — bindless is what
makes it a *first-class* concept rather than an implicit one. Today the binding
address is a compile-time constant duplicated in two places: `TEX_SHADOW_ATLAS = 13`
in `ShaderBindingLayout.h` (one of 60 such `TEX_*` constants) and
`layout(binding = 13)` in the GLSL, re-established every frame by a bind call.
Under heap-bindless the shader instead does
`texture(g_ResourceHeap[material.albedoOffset], uv)`, and that offset is
allocated at runtime and must be *transported* into a UBO. What dies is the
**act of binding** — the 232 `BindTexture` / `BindImageTexture` calls (measured
in Phase 3; an earlier count of ~173 here was wrong because it counted only the
first family) and the `TEX_*` constants. What survives, promoted from constant to data,
is the number itself.

It cannot simply *be* the resource identity, for four reasons that are all
already live in this codebase: one resource maps to many views and therefore
many offsets (`CreateDepthArrayCompareOffView` is the existing proof); a
persistent view's offset is stable for the object's life while a transient's is
re-allocated every frame from the ring; `HeapOffset` must be a bare `u32` a
shader can index with, whereas `ResourceHandle` is 64 bits and the generation is
the entire point of it; and a resource that is only ever a colour attachment
needs no heap slot at all, so fusing them would size the heap by resource count.

`RHI::ResourceHandle` deliberately mirrors the `{ Index, Generation }` shape the
render graph already uses (`RGTextureHandle` / `RGBufferHandle` /
`RGFramebufferHandle` in `Renderer/ResourceHandle.h`), so the graph's existing
handle discipline generalises instead of acquiring a second, competing scheme.
The generation counter is not decoration: GL recycles names, so today two
different objects can compare equal via `Texture::operator==`
(`GetRendererID() == other.GetRendererID()`) if one was deleted and the other
created. A generation catches that; a raw `u32` cannot.

`RHI::HeapOffset` is a plain `u32` wrapper *on purpose*. It has to survive being
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

**Caching rules differ by slot lifetime — and the difference is the whole
point.** An earlier draft of this ADR said flatly "fetch it, never store it",
which contradicted §1.3's material-data story and would have thrown away the
performance argument for bindless. The actual rule has two halves:

- **Persistent slots may be cached**, including baked into material data — that
  stability *is* why bindless is faster, and re-fetching per draw would give it
  back. The cache must hold the `ViewHandle` alongside the offset and revalidate
  through `Heap::OffsetOf` whenever the view could have changed (asset reload,
  texture resize, streaming eviction). Caching the bare `u32` alone is what is
  forbidden, because nothing then detects the view's death.
- **Frame-transient slots must never be stored across a use.** They are
  re-allocated every frame from the ring that resets in
  `TransientPool::ReleaseAll()`, and under `WriteNewVersion` aliasing one
  physical object legitimately holds two offsets within a single frame. A
  transient offset held past the point of write is *already* stale — not "might
  go stale" — so it is re-acquired via `Heap::OffsetOf(ViewHandle)` at each use.

`Heap::OffsetOf` validates the view's generation, so both halves reduce to the
same discipline: **the `ViewHandle` is the durable thing; the `u32` is
derived.** Where they differ is only how long the derived value stays good —
indefinitely for persistent, until the next use for transient.

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

1. **Passes binding a texture for sampling** — **232** `BindTexture(...)` /
   `BindImageTexture(...)` sites outside `Platform/` (the issue's "~125 across
   ~35 passes" undercounts; so did this line's original **173**, which counted
   only `BindTexture(` in `.cpp` files — an image binding is a heap slot too;
   the measured figure is **232**. A handful are the facade's own declarations rather than
   pass calls, and they are counted deliberately: the conversion has to delete
   those too.)
   Under heap-bindless there is no slot to bind to: the pass writes
   `Heap::OffsetOf(texture->GetDefaultView())` into its UBO and the shader
   indexes the heap. (Via the view, not a `texture->GetHeapOffset()` accessor —
   a texture has one offset *per view*, and the offset has to come from
   something generation-checked. For a persistent texture the result is stable
   and the material may cache it alongside the `ViewHandle`; see §1.2.)
   `BindTexture` does not get abstracted, it **disappears**. This is the
   single biggest reason Phase 3's `ARB_bindless_texture` rehearsal is worth
   doing — it forces the GLSL-side and UBO-side change on the backend we can
   still debug easily, before any Vulkan-specific effort.
2. **Debug / introspection tools** (`Renderer/Debug/`, MCP capture) — these
   genuinely need the native object. They get `RHI::ResourceHandle` for identity,
   plus an escape hatch that is **named to be conspicuous**:
   `RHI::GetNativeHandleForDebug(RHI::ResourceHandle) -> RHI::NativeHandle`
   (a `u64` plus a backend tag). The naming is the mechanism: a
   `GetNativeHandleForDebug` call sitting in a render pass is a self-evident
   smell in review, where `GetRendererID()` reads as ordinary. The ratchet
   counts uses of this escape hatch outside `Renderer/Debug/` and
   `Platform/`, with a baseline of zero (§1.7).
3. **`TransientPool` bookkeeping** — `AcquiredInfo::RendererID` only ever answers
   "did these two plan entries get handed the same object?". `RHI::ResourceHandle`
   answers that *better*, because the generation distinguishes a recycled name
   from a genuine alias — which is exactly the distinction the transient-plan
   MCP tooling was built to investigate.

### 1.4 `TransientPool` describes a physical resource without a `u32`

`TransientPool::AcquiredInfo::RendererID` becomes `RHI::ResourceHandle Handle`.
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
`Platform/OpenGL/` as *that backend's* lowering — superseded by (44): the
facade carries both currencies in one virtual, so they stayed put, relabelled
as the GL lowering feeding only the flags field; no relocation — and a Vulkan
backend writes its own lowering to `(srcStageMask, srcAccessMask, oldLayout) →
(dstStageMask, dstAccessMask, newLayout)`.

**The neutral model still does not carry an image layout — but "layout is a pure
function of usage" is too strong, and an earlier draft of this ADR said exactly
that.** For the common cases it holds (`ShaderSample → SHADER_READ_ONLY_OPTIMAL`,
`RenderTarget → COLOR_ATTACHMENT_OPTIMAL`, `ShaderImage → GENERAL`,
`TransferDest → TRANSFER_DST_OPTIMAL`). Three cases in *this* engine break it,
and the fix is to carry the missing **inputs**, not to hoist the layout itself:

- **Depth/stencil aspect.** `D24UNormS8UInt` can transition its depth and
  stencil aspects independently
  (`DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL` and its mirror), and the usage
  enum cannot say which aspect an access refers to. `RHI::SubresourceRange`
  therefore gains an `Aspect` field; without it the range is not a complete
  subresource identifier at all, which is a defect independent of layouts.
- **Read-while-attached.** The PCSS blocker search samples raw depth through
  `CreateDepthArrayCompareOffView` while the same depth resource is in play as
  an attachment. That wants `DEPTH_STENCIL_READ_ONLY_OPTIMAL`, not
  `SHADER_READ_ONLY_OPTIMAL` — and the two accesses are individually legal, so
  no single access value distinguishes them. It needs the *pair* to be visible.
- **Feedback loops / input attachments.** `InputAttachmentRead` lowers
  differently depending on whether the resource is simultaneously an attachment
  (`ATTACHMENT_FEEDBACK_LOOP_OPTIMAL` or `GENERAL`).

So the contract Phase 5 owes is a **layout/access resolution function whose
inputs are `(Access, Aspect, is-also-attached-this-pass)`**, not a bare
`Access → layout` table. The neutral layer supplies those inputs and keeps
naming none of the layouts; the backend owns the mapping. That preserves the
original reason for the decision — a layout enum in the render graph is Vulkan
leaking upward — while dropping the claim that made it sound free.

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

The fix is a **single unified access enum** (`RHI::Access`) covering reads,
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
mandates — which is what #691 is already warning about.

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

**Status (2026-07-30): both Phase 2 counters reached zero.** Step 1 took
`sweep_glad_includes` 70 → 39 by stripping the `GLenum`/`GLuint` virtuals (which
is what made the counter meaningful at all, per the ordering constraint below);
step 2 took `sweep_gl_calls` 313 → 0 and `sweep_glad_includes` 39 → 0. See
"Amendments from Phase 2 step 2" for what the sweep cost — chiefly that the
facade had to grow ~60 virtuals, because it was not merely GL-typed but
incomplete. `tools_gl_calls` is unchanged at 236 and remains Phase 9's
relocation (deferred by #794), not an exemption (§1.6).

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

**All closed.** This section tabled eight named follow-ups — five decided
here and implemented later, two identified but left open, one
re-verification — so that no part of the design could be quietly dropped
between phases. Every row landed: the `RHI::Access` unification, the
`RGWriteUsage::Clear` split and `ProducerPass == "external"` in Phase 5
(amendment (43)); the `Renderer/Debug/` relocation in Phase 9;
`GLenum`/`GLuint` stripped from the facade first, in Phase 2; sampler
deduplication in Phases 3–4 (amendments (23), (70)); the
`GPUResourceInspector` currency question in Phase 8 (amendment (77)); and
the RDNA2 driver re-verification before Phase 4.

### 1.9 Outside corroboration, and the one thing it changed

The model was designed against this codebase rather than against other
engines' write-ups, so Phase 1 cross-checked it against an experienced
outside account (Philip Rebohle, DXVK — legacy bindful, descriptor
indexing, descriptor buffer and heap paths all shipped). It agreed with the
identity/binding-address split and with heap-from-day-one. **It changed
exactly one thing**, and that change lives in ADR 0010: the driver floor,
where `VK_EXT_descriptor_heap`'s youth — not its design — is the risk.

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
  already records what heavyweight vendored dependencies cost this build. This
  is an *availability* switch, and it is **whole-tree**: `OloServer`, the editor
  and the runtime are configured together, so it cannot be set "for the server".
  It also does **not** remove the Vulkan SDK requirement — see amendment (86)
  and the Consequences bullet, corrected by #811.
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

Device selection then gates hard on
[ADR 0010's capability contract](0010-vulkan-rhi-heap-bindless-only.md#capability-contract).
`--rhi=vulkan` is accepted only when a device satisfies that contract **in full**
— API version, SDK/validation floor, `VK_EXT_descriptor_heap`, and the
untyped-pointer shader dependency, plus whatever feature bits Phase 4 pins there.
Anything less: **refuse to initialise, naming the missing capability.**

The contract is referenced, not restated, on purpose. An acceptance test that
open-codes "is `VK_EXT_descriptor_heap` present?" would silently pass a device
missing the shader dependency, and the two definitions would then drift with
nothing to catch it. One list, two readers.

Falling back to OpenGL automatically would reintroduce at runtime precisely the
"silent fallback" that ADR 0010 forbids at compile time — the user asked for
Vulkan and must be told they did not get it. The default-when-unspecified
staying OpenGL is a different thing and is fine.

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
API already gives.

**Soft-fail is required, not merely inherited.** Dropping the stamp mechanism
must not drop the behaviour it provided, so this is explicit: if the
`pipeline_cache.vkpc` blob is missing, unreadable, truncated, or rejected — or
if `vkCreatePipelineCache` fails with the loaded data — **discard the data and
create an empty cache, then continue startup.** A pipeline cache is a
compile-time optimisation; a corrupt one must cost a slower first frame and a
log line, never a failed launch. The same applies on write: a failed serialise
at shutdown is logged and ignored. This mirrors the GL path, where a rejected
`glProgramBinary` falls back to recompilation rather than aborting.

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
by command buffers still in flight, so destruction goes through the
deferred-reclaim machinery (`VulkanDeferredReclaim` — see amendment (52), which
corrected the stale name this paragraph originally used and made the reverse
index and the lookup map one data structure), not an immediate
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

## 4. Decision — root data is one GPU pointer per draw/dispatch, not per-draw bound buffers

§1.2 said a persistent view's heap offset "goes into a UBO/SSBO field," which
quietly assumed conventional buffer *binding* survives for everything that
isn't a texture. It shouldn't: the Phase 2 sweep found "buffer binding
points (`glBindBufferBase`)" was **the single biggest gap category** — 26 call
sites, UBO and SSBO — and a Vulkan backend that lowers each of those to its own
`vkCmdBindDescriptorSets`-shaped call per draw reopens, for buffers, exactly
the per-draw CPU binding cost ADR 0010 rejected for textures. The heap made
texture binding free; nothing in the model so far makes buffer binding free.

**Decision: every draw/dispatch's per-object data — transforms, material
scalars, texture-heap base indices, and pointers to vertex/index/other GPU
buffers — is packed into one POD struct, allocated from a per-command-buffer
GPU-visible bump allocator, and reaches the shader as a single 64-bit pointer.
No other binding call happens per draw.** This is the direct engine-side
adoption of the "root data = one pointer" model Sebastian Aaltonen describes in
*Reducing Graphics API Complexity* (2026) — the outside corroboration is
recorded here on the same basis §1.9 records Philip Rebohle's account: it
independently reaches the same shape ADR 0010's heap-bindless commitment was
already pointed at, and the shader-side dependency it needs
(`VK_KHR_shader_untyped_pointers`) was *already* pinned in ADR 0010's
capability contract — for the narrower reason of heap-offset dereferencing.
Widening its use to carry the whole root struct is a design decision, not a
new dependency.

**Buffers become pointers, not heap slots.** Vertex, index, and storage data
are addressed by buffer device address embedded directly in the root struct,
mirroring how a texture is addressed via `HeapOffset` (§1.1). The resource heap
therefore continues to hold only texture descriptors, exactly as §1.2a already
decided for the unrelated reason that texture and buffer descriptors aren't
the same size — no analogous "buffer heap" is ever built, and the two
decisions reinforce rather than duplicate each other.

**Per-frame/per-view globals do not get a special case.** Camera and lighting
data change once per frame, not per draw, so binding them the old way is not
where the 26-site cost lives. They are threaded through the model anyway — as
a pointer field on the root struct, or on whatever root struct a pass's own
per-draw structs chain to — so there is exactly **one** binding operation per
draw/dispatch (pushing the root pointer) with no second, "important enough to
bind conventionally" tier. Uniformity is the point: a special case here is
where the next 26-site regrowth would start.

**The Vulkan-specific wrinkle the talk's slides gloss over: push constants are
small.** The guaranteed minimum is 128 bytes (desktop drivers on the ADR 0010
floor typically expose 256), which is nowhere near enough to hold a root
struct with several matrices and pointers inline. The push constant therefore
carries **only the 8-byte GPU pointer** to the bump-allocated struct — never
the struct's fields directly. This matches the talk's own CUDA-kernel-launch
framing (the "argument" is the pointer, not the payload) but is worth stating
explicitly because a first implementation reaching for `vkCmdPushConstants`
with an inline struct will silently truncate past 128/256 bytes rather than
fail loudly.

**The GPU temp/bump allocator does not exist yet and is new work, not a
reuse of `TransientPool`.** `TransientPool` (§1.2) allocates and aliases
*physical resources* (textures, buffers as objects); this allocator hands out
byte ranges *within* a resource for CPU-written, GPU-read scratch data — closer
to Vulkan's per-command-buffer push-data pattern or Metal's `setBytes` than to
anything in the render graph today. Backed by VMA (already vendored, Phase 4)
with a linear/ring strategy, reset at the same frame boundary
`TransientPool::ReleaseAll()` already uses, so the two lifetimes stay aligned
without inventing a third one (§1.2's "do not invent a third lifetime class"
rule applies here too). Because the allocation is persistently mapped
(ReBAR/UMA, per the driver floor), the CPU writes the struct's fields directly
into GPU memory — no staging buffer, no copy command. This is the one part of
the model that is *easier* than the texture-upload path (§1's texture-upload
discussion), not harder: root data has no tiling or compression to preserve.

### 4.1 What this replaces, and what stays

The two virtuals the Phase 2 sweep added for `glBindBufferBase` (the "buffer
binding points" row) stay in the facade **for GL**, unchanged —
this is a Vulkan-side simplification, not a neutral-layer one, following the
same pattern §1.6 used for `Renderer/Debug/`: a backend-specific improvement
does not have to be expressed as a shared abstraction change. The Vulkan
backend simply never calls them; per-draw data reaches its shaders exclusively
through the root-pointer path above.

**Owner: Phase 6**, and it should land *before* Phase 6's "one already-golden-
tested pass renders correctly" checkpoint — a pass converted to bound-UBO
Vulkan first and to root-data-pointer Vulkan second is strictly more work than
doing it once, and the checkpoint pass is what every later pass in Phase 7
will be copied from.

### 4.2 GPU-driven indirect root data: no separate shape for a compute-written draw

Because root data is just a GPU pointer, the struct an ordinary draw call
points to and the struct an *indirect* draw call points to are the same kind
of thing — the only difference is who writes it. **Decision: indirect
draw/dispatch takes the identical single-pointer root-data contract as a
direct call.** There is no separate "indirect root data" shape, no CPU-side
indirect-args staging step, and no driver-managed command-signature object
(the DX12-shaped `ID3D12CommandSignature` has nothing to abstract here,
because the pointer *is* the argument list already). A GPU-driven pass —
visibility/occlusion culling, LOD selection, virtual-geometry cluster culling
(`VirtualClusterCull.comp`, per `docs/agent-rules/cluster-lod-simplification.md`)
— writes its output root-data structs directly into GPU memory and issues the
indirect draw pointing at them; nothing round-trips through the CPU. This
codebase already has prior art for "a GPU-written buffer is the argument list,"
in the two-counter overflow contract issue #725's GPU debug draws use — the
same shape, applied here to indirect draw/dispatch arguments instead of debug
primitives.

The only new requirement is the ordinary one: a barrier between the writing
compute dispatch and the consuming indirect draw, using §1.5's existing
hazard-flag model (an indirect-args hazard, the same category §1.5 already
names for descriptor writes). No new barrier machinery — this is an
application of §1.5, not an extension of it.

**The failure mode worth naming for Phase 7:** a converted GPU-driven pass
that still stages its indirect args through the CPU is not incorrect, but it
is leaving §4's investment on the table — the same category of regression as
converting a pass to bindless textures while leaving one sampler on the slot
path (Phase 3's finding, ADR amendments (32)-(38)). Treat a CPU-staged
indirect-args path in a newly-ported GPU-driven pass as a defect to fix, not
a style choice, once §4 exists to make the alternative free.

**Owner:** the mechanism is established in Phase 6 alongside the rest of §4;
individual GPU-driven passes (culling, LOD, virtual geometry) adopt it
pass-by-pass as Phase 7 ports them.

---

## 5. Decision — minimize PSO permutation axes before Phase 6's first `VkPipeline`

§3 designs the PSO *cache* — invalidation, hot reload, the shader→pipeline
reverse index — but not what varies a `VkPipeline` in the first place. Left
undecided, Phase 6 will bake whatever is convenient into
`VkGraphicsPipelineCreateInfo` (the GL-shaped default: vertex layout, depth,
blend, and raster state all monolithic), and Phase 7's 35+-pass port
multiplies that decision by every state permutation each pass happens to use.
This is the same permutation-explosion mechanism ADR 0010/0011 already
diagnose as the cause of DX12/Vulkan's real-world PSO-hitch problems — the
difference is that this codebase gets to decide it *before* the first
pipeline exists, not retrofit it after 35 passes have already baked their own
assumptions in.

**Decision, per state axis, following Aaltonen's thin-PSO argument:**

| Axis | Decision | Why it's safe on this ADR's driver floor |
| --- | --- | --- |
| Vertex input layout | **Not baked at all.** Extend Phase 3's bindless-texture pulling pattern to vertex data: vertex shaders fetch through a buffer-device-address pointer with manual indexing, not `VkPipelineVertexInputStateCreateInfo`. | Removes the axis entirely rather than making it cheaper — the same move §4 makes for buffers generally, applied to the one buffer type (vertex) that still has a dedicated binding mechanism. |
| Depth/stencil state | Dynamic, via `VK_EXT_extended_dynamic_state` / `…state2` (core-promoted). | Universal on desktop; Metal has proven the same split safe across Nvidia/AMD/Intel/Apple since its first version (talk, §4.8). |
| Blend state | Dynamic where `VK_EXT_extended_dynamic_state3`'s blend-enable/equation states are available; baked into the PSO only where they aren't. | The talk's finding is that *only* Apple/PowerVR mobile GPUs genuinely require blend baked into the shader — irrelevant to this ADR's Vulkan desktop floor (NVIDIA/AMD), so the fallback path should never actually trigger here. Verify at Phase 6's device-capability audit rather than assume — same discipline ADR 0010 used for the heap extension's feature bits. |
| Rasterizer/render-target description | Stays minimal: target formats/count, depth/stencil format, sample count, dual-source blending flag. Nothing else. | This is already the shape §1's survey implies; stating it here makes it a decision instead of an accident. |

**Sequencing note:** vertex-pulling is comparable in size to Phase 3's
bindless-texture rehearsal (a real conversion across every draw call, not a
config flag), so it should be an explicit Phase 6 sub-step completed before
the "one pass renders correctly" checkpoint — not deferred to Phase 7, where
retrofitting it after 35 passes have been ported against a baked vertex-input
PSO would cost far more than doing it once, up front, on the one pilot pass.

**What is deliberately not pinned here:** the exact
`VkPhysicalDeviceExtendedDynamicState3PropertiesEXT` / feature bits Phase 6
should require. As with ADR 0010's capability contract, writing specific bit
names into an ADR with no Vulkan device code to validate them against would be
guessing; Phase 6 fills this in against the real driver floor, the same way
Phase 4 filled in ADR 0010's table.

**Owner: Phase 6**, alongside §4 and ahead of the same checkpoint.

---

## 6. Decision — split barriers are a GPU-pointer signal/wait pair, the same mechanism as timeline semaphores

§1.5 designs the *same-command-buffer* barrier: a producer/consumer pair
inside one recorded submission, expressed as `(FromUsage, ToUsage, Range,
hazard)`. It says nothing about hiding latency across a *longer* span — the
case DX12's split barriers and Vulkan's events each address today with their
own persistent, ceremony-heavy driver object, which is a large part of why,
per Aaltonen's talk, almost nobody actually uses either.

**Decision: a split barrier is a plain GPU-memory location plus two
operations — `Signal(pointer, value, op)` and `Wait(pointer, value,
compareOp)` — with `op`/`compareOp` covering at minimum `{Set/Equal,
AtomicMax/GreaterEqual}`.** The max/greater-equal pair is what generalizes
this from a one-shot flag into a monotonically increasing counter, i.e. a
timeline. This is deliberately not new Vulkan machinery: it is the existing
**timeline semaphore** primitive (core since Vulkan 1.2, well inside the ADR
0010 1.4 floor), promoted to the render graph's one mechanism for any
cross-pass or cross-frame dependency, rather than inventing a second,
Vulkan-events-shaped abstraction alongside it the way DX12 keeps both split
barriers and fences as separate concepts.

**How this plugs into the existing model.** `RenderGraphBarrierPlanner`
(§1.5) keeps computing ordinary same-submission dependencies unchanged — nothing
here replaces that. A dependency that spans further — a compute culling pass
whose tail latency can be hidden behind independent work later in the same
frame, or a cross-frame dependency the render graph currently expresses as an
ad hoc CPU/GPU fence — becomes an `RHI::GpuFence`: one GPU pointer, allocated
from the same per-frame lifetime discipline §4's temp allocator already
established, with `Signal` attached to the producing render-graph node and
`Wait` attached to the consuming one (or to nothing, if the wait is a CPU-side
frame-pacing check instead).

**One primitive, both sides of the API.** The same `Signal`/`Wait` pair
replaces ad hoc CPU-side fence chains too — a single persistently-mapped
counter, incremented by `Signal`, observed via a CPU `WaitGreaterEqual` — so
the render graph's existing CPU/GPU frame-pacing mechanism and its future
GPU-side split-barrier mechanism are the same code, not two.

**What this does not change:** §1.5's ordinary same-command-buffer barrier
model stays exactly as decided. This is additive, for the specific case where
a blocking barrier would leave the GPU idle because there is genuine
independent work available to fill the gap.

**Deferred to Phase 7:** which specific render-graph passes are worth
splitting rather than barriered inline is a per-pass profiling decision, not
one this ADR can make abstractly — §6 only decides that the primitive exists
and what shape it has. Applying it is opportunistic, not a requirement every
ported pass must satisfy.

**Owner: Phase 6** (the primitive, alongside §4/§5 — all three are the same
family of low-ceremony, pointer-shaped mechanism), **consumed pass-by-pass in
Phase 7** wherever profiling shows a real latency-hiding opportunity.

---

## Amendments from Phase 2 (2026-07-30)

> **How to read the amendments (note added in Phase 9).** Every numbered entry
> below is one of four genres: a **decision** (a new or changed contract,
> binding on later phases), a **correction** (an earlier section said something
> wrong; the amendment is the fix), a **postmortem kernel** (the rule that
> survived an incident, stated in a few lines, with a `Narrative:` pointer to
> [rhi-abstraction-boundary.md](../agent-rules/rhi-abstraction-boundary.md) —
> or, for (80), to
> [vulkan-command-ordered-buffer-writes.md](../agent-rules/vulkan-command-ordered-buffer-writes.md)
> — where the full story lives; the story is not duplicated here), or **phase
> status** (what a phase deliberately did or did not build, kept as a terse
> list). Superseded amendments keep their body or kernel plus a pointer to
> what superseded them.
>
> **Numbers are never REUSED or renumbered.** They are cited from code
> comments and other docs, so a citation must always resolve. An entry may
> be *removed* only when nothing outside this file references it — pure
> phase-status records, and corrections already folded into the section
> they corrected — and removal leaves a permanent gap. (#691
> removed (5), (18), (54), (55) and (69) on those grounds.) Anything still
> cited stays, even when dead, as a tombstone.
>
> **Amendment numbers are cited from
> code comments (68 citations of 23 distinct numbers at last count) and from
> other docs, and must NEVER be renumbered or deleted.** New amendments append
> at the next free number under their own phase header.


The bodies live in **[0011-amendments.md](0011-amendments.md)** — this section
keeps the index so "does (N) still bind, and what did it decide?" is answerable
without opening them.

### Index — what each amendment decided, and whether it still binds

Seek by number: a code comment citing `amendment (N)` lands here first. The rows
marked `live` are the contract, and their bodies are authoritative. Every other
row is kept only because something still cites the number — its body is a
tombstone or a dated record, not a rule to follow. Rows are in numeric order;
the headings below are in their original (occasionally out-of-order) sequence.

| # | One-line summary | Status |
| --- | --- | --- |
| (1) | `SetPolygonMode` drops its face parameter; `polygonFace` deleted | live |
| (2) | `SamplerDesc` gains `Filter`/`AddressMode` enums instead of two bools | live |
| (3) | `SetTextureParameter` splits into `SetTextureFilter` + `SetTextureWrap` | live |
| (4) | Upload's `(format, type)` becomes one `RHI::Format` for the SOURCE buffer | live |
| (6) | `RHI::NoAttachment` sentinel — "this draw slot writes nowhere" | live |
| (7) | `glGetError` is not abstracted; readbacks return `bool` instead | live |
| (8) | Draws from previously-bound geometry are the portable shape | live |
| (9) | Recorded debt: `SetProgramUniformFloat` has no Vulkan counterpart | live |
| (10) | Enum vocabulary rules: no `default:`, no `Count`, reuse prior-phase concepts | live |
| (11) | `ViewHandle`/`HeapOffset` deferred to Phase 3 as a matched pair | closed by Phase 3 |
| (12) | Identity is the C++ resource object, not the native name | live |
| (13) | Every `Delete*` must `Unregister` its handle, not just delete | live |
| (14) | `.data()` erases the type you just changed | live |
| (15) | Bind sites keep the call, change the currency; scope by dataflow | live |
| (16) | Convert subsystem-per-commit; scripted name-based edits compile while wrong | live |
| (17) | The graph's `ResourceHandle` is renamed `RGResourceHandle` | live |
| (19) | Bindless GLSL cannot enter SPIR-V; the rehearsal's shader work is throwaway | live |
| (20) | `ViewDesc` cannot describe a view well enough to create one | live |
| (21) | The bind-site ratchet targets a floor, not zero | live |
| (22) | A stable identity needs an explicit PUSH invalidation hook | live |
| (23) | Sampler heap: `SamplerDesc`-keyed, refcounted, slots never compacted | live |
| (24) | The bindless raw-GLSL route needs its own variant-keyed program cache | live |
| (25) | The offset table is indexed by the shader's own `TEX_*` slot | live |
| (26) | Storage images are a second descriptor KIND, not more call sites | live |
| (27) | `ViewUsage` goes on `ViewDesc` — one description, two descriptor kinds | live |
| (28) | One reserved null descriptor per KIND, not per heap | live |
| (29) | Image units take a disjoint region of the same offset table | live |
| (30) | (20) partially closed; only view dimension and inherited format remain | live |
| (31) | Compute needed no second compile route — measure a constraint's blast radius | live |
| (32) | Per-material offsets belong in the material UBO, not the shared table | corrected by (35) |
| (33) | An offset table must be re-based per heap EPOCH, per descriptor kind | live |
| (34) | A fixture displacing the heap singleton restores backend, desc AND flag | live |
| (35) | (32)'s cost argument was material-only; every draw publishes offsets | live |
| (36) | `Texture::Bind(slot)` is a second bind spelling the ratchet cannot count | live |
| (37) | Sweep "done" = converted or recorded, machine-checked both directions | live |
| (38) | `SamplerDesc{}` means INHERIT; stated intent gets the split-heap form | live |
| (39) | `RenderCommand`'s backend is built at static init, before `--rhi=` parses | closed by (48) |
| (40) | `config/renderer.yaml` is the selection fallback; degrade rules are asymmetric | live |
| (41) | Vendored Vulkan headers must out-rank the SDK's; three defaults invert it | live |
| (41a) | `vulkan-1.lib` must never be linked — its thunks replace volk's globals | live |
| (42) | What Phase 4 did not build, plus the present-sync shape to keep | history |
| (43) | Capture the consumer's access at barrier EMISSION, never by rescanning | live |
| (44) | One barrier virtual carries both GL flags and the neutral barrier span | live |
| (45) | Barrier handles resolve at EXECUTE time; the cached plan stays name-keyed | live |
| (46) | The tracker owns `oldLayout`; mixed ranges split into equal-layout runs | live |
| (47) | `VulkanRendererAPI` behind the existing dispatch table, in three loud tiers | live |
| (48) | (39) closed; a barrier stage mask may only name ENABLED features | live |
| (49) | VMA facts: format resolution, generation-counted reclaim, `GetRendererID()` 0 | corrected by (55) |
| (50) | Existing `layout(set, binding)` declarations map onto root data at PSO creation | live |
| (51) | Fold promoted feature structs into the `VulkanXYFeatures` aggregate | live |
| (52) | The pipeline reverse index and the lookup map must be one structure | live |
| (53) | The fence wait is the only legal reclaim-queue drain point | live |
| (56) | What Phase 6 did not build, and where each item landed later | history |
| (57) | A ported pass needs its defining property pinned analytically | live |
| (58) | Port an entry point's DEFAULT-ARGUMENT sentinels, not just its signature | live |
| (59) | The projection seam has three flavours; enumerate matrix READERS | live |
| (60) | One Vulkan descriptor set collapses GL's separate binding namespaces | live |
| (61) | Bare uniforms migrate into `std140` blocks; the no-op setter hides failure | live |
| (62) | Three standalone-graph fixture contracts that read like backend bugs | live |
| (63) | Attachment transitions are the draw front-end's job, batched before rendering | live |
| (64) | Factories and process-wide guards belong in neutral TUs with backend switches | live |
| (65) | Enable device feature bits WHEN SUPPORTED, never from a constant | live |
| (66) | Descriptor mappings carry per-kind `resourceMask`s, never ALL | live |
| (67) | Orientation, extent and present layout are window-only proofs | live |
| (68) | A diagnostic naming a cause must exclude the look-alike causes | live |
| (70) | The Vulkan sampler heap retires the embedded sampler; sampler state is not PSO state | live |
| (71) | A deferred clear binds to the identity that requested it | live |
| (72) | One-shot submits execute BEFORE the still-recording frame's work | live |
| (73) | The two projection flavours need SEPARATE `CameraUBO` members | live |
| (74) | A sweep keyed on the live log has an invisible shadow | live |
| (75) | GL's cube-face z is a Vulkan array layer; transition the whole image | live |
| (76) | A vertex-pull branch's stride comes from the DRAW SITE's layout | live |
| (77) | Inspector shows both currencies; Vulkan tenants ride the nightly GPU cadence | live |
| (78) | A zero root address is device loss; a UBO must outlive its draw | live |
| (79) | Vulkan off-screen row order recorded as per-TARGET; `flipY` knob added | superseded by (85) |
| (80) | Life-stable addresses replay GL's command-ordered writes as last-write-wins | live |
| (81) | Unfed sampler bindings resolve to a typed null image, not slot 0 | live |
| (82) | Patch pipelines must chain the `LOWER_LEFT` tessellation domain origin | live |
| (83) | Gate on RHI identity, never the native id; size a framebuffer to its attachments | live |
| (84) | Buffer-device-address reads have no bounds; enable `VK_EXT_device_fault` | live |
| (85) | One row order per backend; the per-target `flipY` knob retires | live |
| (86) | `OloServer` stays backend-less; GPU flags fall back to CPU paths | live |
| (87) | (77)'s cadence confirmed wired; the nightly GPU lane was silently red | live |
| (88) | Inspector's Vulkan arm discovers from `ResourceRegistry::Snapshot()`; the snapshot clone mints its own identity, so every MCP render tool answers on both backends | live |

Phase 1 said explicitly that "nothing here is load-bearing until Phase 2
begins," and that if the sweep discovered a decision was wrong the ADR should be
amended rather than silently diverged from. Four things were discovered while
stripping `RendererAPI`'s GL-typed virtuals (§1.7's first task). None of them
changes the model in §1.1–§1.5; all four are corrections to the *vocabulary*
§1.7 promised.

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
- `Renderer/Debug/` (236 calls, 43% of the total) is Phase 9 work (deferred by
  #794) that Phase 2 explicitly does not have to carry — but it is
  *relocation*, not exemption, and Phase 7 cannot be verified per `CLAUDE.md`'s
  rendering rule until it is done.
- `ResourceTransition`'s read/write enum pair was a known, recorded defect for
  Vulkan (§1.5) — fixed in Phase 5 by (43): the transition record is unified
  onto `RHI::Access`, with the WAW fix applied at barrier emission.
- One binary ships both backends. Binary size and link time grow; `OLO_WITH_VULKAN=OFF`
  drops the volk / vulkan-headers / VulkanMemoryAllocator ports and compiles the
  backend object code out. **It is not an escape valve from the Vulkan SDK, and
  not a per-target one.** `find_package(Vulkan REQUIRED)` is deliberately ungated
  (the *shader* toolchain — shaderc / glslang / SPIRV-Tools / SPIRV-Cross — lives
  in the same SDK and every build needs it), and the option is whole-tree, so
  `OloServer` cannot be configured lean while the editor is not. Corrected in
  place by amendment (86); #811 then decided the follow-on question — **an
  SDK-free lean server build is not a goal** (the server is already backend-less
  per (86), and its deployment image needs no GPU stack) — and added the
  `vulkan-off.yml` CI job so the OFF configuration is compiled somewhere other
  than a developer's machine.
- Vulkan adds exactly one new on-disk cache artefact (`pipeline_cache.vkpc`) and
  changes the *name* of an existing one (the target-env-keyed SPIR-V). Existing
  caches are invalidated once, at the version that lands (b); this is a
  cold-start cost, not a correctness issue.
- Nothing here is load-bearing until Phase 2 begins. If Phase 2 discovers the
  three-way identity split is wrong, the cost of revisiting is this document
  plus a header — which is exactly why Phase 1 was worth doing before the sweep,
  and not after.
