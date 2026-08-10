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
  count.** 79 files outside `Platform/OpenGL/` include `<glad/gl.h>`; 42 call
  GL. Those two sets are not nested — 39 files both include and call, 3 call
  while relying on the transitive include through `RendererAPI.h` (next bullet),
  and the remaining **40 include the entire API while calling none of it**,
  purely to name the `GLenum` / `GL_*` constants they pass into `RendererAPI`'s
  own virtuals — `SetBlendFunc(GLenum, GLenum)`,
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
in Phase 3; this said ~173, which counted only the first family — see amendment
(18)) and the `TEX_*` constants. What survives, promoted from constant to data,
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
   only `BindTexture(` in `.cpp` files — an image binding is a heap slot too.
   See amendment (18). A handful are the facade's own declarations rather than
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
`Platform/OpenGL/` as *that backend's* lowering; a Vulkan backend writes its own
lowering to `(srcStageMask, srcAccessMask, oldLayout) → (dstStageMask,
dstAccessMask, newLayout)`.

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

**Status (2026-07-30): both Phase 2 counters reached zero.** Step 1 took
`sweep_glad_includes` 70 → 39 by stripping the `GLenum`/`GLuint` virtuals (which
is what made the counter meaningful at all, per the ordering constraint below);
step 2 took `sweep_gl_calls` 313 → 0 and `sweep_glad_includes` 39 → 0. See
"Amendments from Phase 2 step 2" for what the sweep cost — chiefly that the
facade had to grow ~60 virtuals, because it was not merely GL-typed but
incomplete. `tools_gl_calls` is unchanged at 236 and remains Phase 8's
relocation, not an exemption (§1.6).

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

## 4. Decision — root data is one GPU pointer per draw/dispatch, not per-draw bound buffers

§1.2 said a persistent view's heap offset "goes into a UBO/SSBO field," which
quietly assumed conventional buffer *binding* survives for everything that
isn't a texture. It shouldn't: amendment (5)'s sweep found "buffer binding
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

The two new virtuals amendment (5) added for `glBindBufferBase` (the "buffer
binding points" row, 2 virtuals) stay in the facade **for GL**, unchanged —
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

Phase 1 said explicitly that "nothing here is load-bearing until Phase 2
begins," and that if the sweep discovered a decision was wrong the ADR should be
amended rather than silently diverged from. Four things were discovered while
stripping `RendererAPI`'s GL-typed virtuals (§1.7's first task). None of them
changes the model in §1.1–§1.5; all four are corrections to the *vocabulary*
§1.7 promised.

**(1) `SetPolygonMode` loses its face parameter entirely.** §1.7 listed the
neutral replacement for `SetPolygonMode(GLenum, GLenum)` as
`RHI::CullMode` (face) + `RHI::PolygonMode`. That is wrong, and preserving it
would have re-exported a GL wart that GL itself deprecated: **core-profile
`glPolygonMode` accepts only `GL_FRONT_AND_BACK`** — `GL_FRONT` or `GL_BACK`
raises `GL_INVALID_ENUM` — and all 47 call sites in the engine passed
`GL_FRONT_AND_BACK`. Vulkan's `VkPipelineRasterizationStateCreateInfo::polygonMode`
has no face either. The signature is therefore `SetPolygonMode(RHI::PolygonMode)`
and `PODRenderState::polygonFace` is deleted along with it.

*Generalisable:* when translating a legacy parameter, check whether the source
API still accepts more than one value for it. A parameter with exactly one legal
value is not an abstraction to preserve — it is a fossil, and carrying it forward
makes the neutral layer harder to implement on the backend that never had it.

**(2) `SamplerDesc`'s two bools are not expressive enough; it gains real enums.**
§1.2a modelled sampler state as `bool LinearFilter` / `bool ClampToEdge`. Those
describe the typical texture but cannot describe the ones the sweep actually had
to replace: `SSAORenderPass`'s noise texture is **Nearest + Repeat**, and
`CreateDepthArrayCompareOffView` is **Nearest + ClampToBorder**. Under the
two-bool model those call sites would have had to keep a GL escape hatch, which
would have left `sweep_glad_includes` unable to reach zero for a reason that is
purely a modelling shortfall. `RHI::Filter` and `RHI::AddressMode` are added to
`RHITypes.h`, and `SamplerDesc` carries `MinFilter` / `MagFilter` /
`AddressU|V|W`. This does not affect §1.2a's actual decision (sampler
*deduplication* is still Phase 3/4 and still has no GL counterpart to port).

**(3) `SetTextureParameter` decomposes into intent-named setters.** §1.7 flagged
this as the one virtual that "resists a mechanical translation" and warned
against mirroring GL's `pname` space with an `RHI::TextureParameterName`. The
resolution: every call site in the engine sets exactly min filter, mag filter,
and wrap S/T/R — and every one of them uses a single wrap value for all axes.
So `SetTextureFilter(id, min, mag)` + `SetTextureWrap(id, mode)` covers 100% of
usage with no open-ended enum. `SetTextureWrap` sets all three axes because
`GL_TEXTURE_WRAP_R` is part of every texture object's sampler state and is inert
on a 2D target, so doing so is a faithful reproduction rather than a widening.
**There was no Phase 2 design gap here** — the escape hatch §1.7 held open (a
comment on #691) was not needed.

**(4) `UploadTextureSubImage2D`'s `(format, type)` pair collapses into one
`RHI::Format` describing the *source buffer*.** Worth recording because the
naming invites a bug: this parameter is **not** the texture's storage format.
The engine relies on GL converting on upload — SSAO's noise texture is `RG16Float`
storage fed from `RG32Float` host data — so a future backend must treat this as a
staging-buffer layout, not a format reinterpretation.

Also added to `RHITypes.h` for completeness of the sweep: `RHI::IndexType`
(so the POD draw commands can describe their index buffer without a `GLenum`)
and `RHI::Format::R32UInt` (used by five image bindings and absent from Phase 1's
list).

**One new guard, which is the real lesson.** The failure mode this phase is
gated on is a *wrong enum mapping*: `GL_SRC_ALPHA → RHI::BlendFactor::SrcAlpha`
has to be right ~270 times, and a wrong entry renders subtly wrong while every
existing test stays green. Two mitigations are now in the tree, and the second
matters more than the first:

- `OloEngine/tests/Rendering/RHIEnumLoweringTest.cpp` asserts every enumerator
  against the literal `GL_*` token it names.
- That test also carries a `static_assert` on each enum's **last enumerator
  ordinal**. Without it, adding a member without extending `ToGL()` falls through
  the switch's `default:` and returns a *plausible* value — a silent wrong
  mapping that a table-of-expectations cannot catch, because the new member has
  no row in the table. A test that enumerates known values can only guard the
  values it already knows about; pinning the count is what makes it guard the
  ones it does not.

---

## Amendments from Phase 2 step 2 (2026-07-30) — the call-site sweep

Step 1 converted the facade's *vocabulary*; step 2 swept the 313 raw `glXxx()`
call sites in the sweep bucket to zero. The headline finding is that **the
facade was not merely GL-typed, it was incomplete**: 84 distinct GL entry points
appear at those call sites, and **54 of them had no `RendererAPI` equivalent at
all**. Closing that gap took **60 new virtuals**.

Those two numbers are deliberately not folded into one percentage, because they
count different things: an entry point can expand into more than one virtual
(`glClearTexImage` becomes a float clear and a uint clear, mirroring
`VkClearColorValue`'s union; the two readbacks each gained a `bool` return). 54
is the size of the *gap*; 60 is the size of the *fix*. Quoting 60 against 84 as a
ratio would silently compare an operation count to an API count.

### (5) The facade grows 60 virtuals, and that number is the real measurement

§1.7 framed Phase 2 as "strip the `GLenum`s, then sweep". That undersells it.
Stripping the enums (step 1) touched 74 existing virtuals; the sweep needed
**60 new ones**, because whole categories of GPU work had simply never been
abstracted and every pass reached past the facade to do them:

| Category | New virtuals | Why it had no facade entry |
| --- | ---: | --- |
| Buffer binding points (`glBindBufferBase`) | 2 | The single biggest gap — 26 call sites, UBO and SSBO |
| Buffer lifecycle (create / storage / map / copy / clear / readback / delete) | 9 | `UniformBuffer` / `StorageBuffer` wrap *their* buffers; `VirtualMeshRegistry` hand-rolls an arena + a persistent-mapped upload ring |
| Named-framebuffer state (draw/read attachment, clear, blit, attach, completeness) | 10 | `SetDrawBuffers` existed but only for the *bound* FBO; every call site names a specific one via DSA |
| Queries (occlusion + timer) | 7 | `BeginConditionalRender` existed; the pools that feed it did not |
| Fences | 4 | `FrameResourceManager` used `GLsync` directly |
| Draws from bound geometry | 4 | The `*Raw(vaoID, …)` family binds its own VAO; `CommandDispatch` keeps a redundant-bind cache and needs a draw that does not re-bind |
| Program / VAO / framebuffer binding | 5 | `Shader::Bind()` exists, but the POD dispatcher holds only a `u32` program id |
| Texture clear / offset upload / readback / dimensions / barrier | 8 | — |
| Vertex-array lifecycle | 3 | — |
| Debug groups, device idle, sample-count caps, separate blend func, front face, clear depth, patch count | 8 | — |

*Generalisable, and the thing to carry into Phase 5:* **an abstraction's
completeness is not measured by how many call sites it already serves, but by
how many distinct operations the layer above performs.** 74 virtuals looked like
a thorough facade while 60 operations went around it, because the ones that went
around it were each rare enough (1–3 sites) to feel like a special case. The
`glBindBufferBase` count (26 sites, one missing pair of virtuals) is the
counter-example that shows frequency was never the signal either.

### (6) Named framebuffers need a "writes nowhere" sentinel

`glNamedFramebufferDrawBuffers` is 24 of the 313, and the interesting half of
them (`DecalRenderPass`) pass arrays containing `GL_NONE` — *slot i writes
nothing* — to steer a decal into exactly one G-Buffer attachment. The existing
`SetDrawBuffers(std::span<const u32>)` maps `attachments[i] →
GL_COLOR_ATTACHMENT0 + attachments[i]` and **cannot express that**.

`RHI::NoAttachment` (a `u32` sentinel, `numeric_limits<u32>::max()`) is added and
honoured by every draw-attachment lowering. This matters beyond GL: a Vulkan
backend maps the same list onto `VkSubpassDescription::pColorAttachments` where
the equivalent is `VK_ATTACHMENT_UNUSED` — also a sentinel, also not
representable as an index. Both APIs need it; only the neutral layer was missing
it.

`glNamedFramebufferDrawBuffer` (singular) folds into the same virtual as a
one-element span — it sets draw slot 0 to the named attachment, which is exactly
what a one-element list does.

### (7) `glGetError` disappears rather than being abstracted

`ThumbnailCapture` reads a texture back and then checks `glGetError()`. A
neutral `GetError()` would be the wrong shape twice over: GL's error model is a
global sticky flag, Vulkan's is a per-call `VkResult`, and exposing either forces
the other backend to fake it.

The readback virtuals therefore **return `bool`** and swallow the check inside
the backend. One entry point vanished from the sweep with no replacement, which
is the outcome to prefer whenever a GL call exists only to interrogate a
GL-specific mechanism. Same reasoning as amendment (1)'s `SetPolygonMode` face:
check whether the parameter/call is a fossil before translating it.

### (8) Draws that do *not* bind their geometry are the Vulkan-shaped ones

`CommandDispatch` keeps a `CurrentBoundVAO` cache and calls `glDrawElements`
directly, so routing it through the existing `DrawIndexedRaw(vaoID, …)` family
would have made the backend re-bind on every draw and defeated the cache.

The new `DrawBoundIndexed` / `DrawBoundIndexedInstanced` / `DrawBoundArrays` draw
from *previously bound* geometry — which is not a GL-ism to be apologised for,
it is the **native Vulkan shape** (`vkCmdBindVertexBuffers` +
`vkCmdBindIndexBuffer` then `vkCmdDrawIndexed`). The combined `*Raw(vaoID, …)`
form that binds-and-draws is the less portable of the two. They also carry
`RHI::PrimitiveTopology` and `RHI::IndexType` explicitly rather than hard-coding
`GL_TRIANGLES`/`GL_UNSIGNED_INT` as the `*Raw` family does.

`SetPatchVertexCount` is split out rather than folded into a patch-draw variant,
because the tessellation call sites set it once and draw many times.

### (9) One recorded debt: `SetProgramUniformFloat` is not portable, deliberately

`CommandDispatch::DrawInfiniteGrid` does `glGetUniformLocation(program,
"u_GridScale")` + `glUniform1f`. A name-keyed default-block uniform has **no
Vulkan counterpart** — SPIR-V has push constants and UBO members, not a
queryable default uniform block.

Three options were weighed: move `u_GridScale` into the camera/grid UBO (a
shader change, and this branch is a call-site sweep whose safety net is
golden-image parity — a shader edit forfeits that), reach for the `Shader` class
(the dispatcher holds a `u32` program id by design, not a `Ref<Shader>`), or add
the virtual and record the debt. The third is taken: `SetProgramUniformFloat(u32
programID, std::string_view name, f32 value)` exists, has exactly one call site,
and is **the one virtual on the facade that a Vulkan backend cannot implement
faithfully.** Phase 6 must fold `u_GridScale` into a UBO and delete it. It is
called out here rather than left as a surprise, because a single unimplementable
virtual discovered during Phase 7 bring-up reads as a design failure when it is
actually a scheduled one.

### (10) New `RHITypes.h` vocabulary

`RHI::QueryType` (`OcclusionAnySamples`, `TimeElapsed` — the two the engine
actually uses; deliberately not a mirror of GL's target space),
`RHI::FenceStatus` (`AlreadySignaled` / `ConditionSatisfied` / `TimeoutExpired` /
`Failed`, matching `glClientWaitSync`'s four returns and `vkWaitForFences`'
`VK_SUCCESS`/`VK_TIMEOUT` split), `RHI::BlitAspect`, and `RHI::NoAttachment`.

**`MemoryResidency` moved from `RHIResources.h` to `RHITypes.h`, and the near-miss
is the lesson.** `AllocateBufferStorage` needs to say how a buffer's memory is
used, and the sweep started inventing a `RHI::BufferUsage` enum
(`DynamicDraw`/`DynamicCopy`/`DynamicRead`) for it — a straight transcription of
GL's usage hints. Phase 1 had **already designed exactly this concept**, better,
as `MemoryResidency` (`DeviceLocal` / `HostToDevice` / `DeviceToHost`): named by
intent rather than by GL's spelling, and the three members map one-to-one onto
what the sweep needed. It was invisible because it sat next to `BufferDesc` in a
header nothing consumed yet, and because `RendererAPI.h` includes only
`RHITypes.h`.

What surfaced it was not review — it was a **name collision**: `RHIResources.h`
already had a `BufferUsage`, a *bind-flags* enum (`Vertex`/`Index`/`Uniform`/
`Storage`/…), and the two could not coexist. The engine library compiled fine
(nothing in it includes `RHIResources.h`); only the ratchet test, which includes
that header precisely so the declaration-only vocabulary keeps compiling, caught
it.

*Generalisable:* **a declaration-only header from an earlier phase must be read
for the vocabulary you are about to invent, not just for the types you consume.**
Phase 1 wrote that header so Phase 2 would have "a fixed target to convert
toward"; the sweep nearly added a second, worse spelling of one of its concepts
anyway. The collision was luck. The habit that would not need luck is: before
adding an enum to `RHITypes.h`, grep `Renderer/RHI/` for the concept, not the
name.

Note this also resolves what would otherwise have been recorded as debt against
`StorageBufferUsage` (`StorageBuffer.h`, `DynamicDraw`/`DynamicCopy`): that
engine-wrapper option and `MemoryResidency` are now the only two spellings, and
Phase 5 collapses them when `StorageBuffer` moves onto `RHI::ResourceHandle`.

Every new enum is pinned by the same last-ordinal `static_assert` + literal-token
table in `RHIEnumLoweringTest.cpp` that the "One new guard" paragraph above
established (not amendment (4), which is about `UploadTextureSubImage2D`'s
source-buffer format).

One correction to that guard's stated reach, found in step 2: the last-ordinal
`static_assert` catches an enumerator being **inserted, removed or reordered**,
but *not* one **appended** after the current last member — appending leaves the
asserted ordinal unchanged. Appends are caught by the compiler instead: the
lowering switches in `OpenGLRHIConversions.h` deliberately carry no `default:`
label, so `-Wswitch` errors on the unhandled enumerator. That makes the absence
of `default:` load-bearing rather than an oversight, and makes the clang-cl CI
job the one that enforces it (MSVC's C4062 is off by default even at `/W4`).
A `Count` sentinel per enum was considered and rejected: it makes an invalid
value representable in the neutral vocabulary and forces a dead `case` in every
lowering switch.

---

## Amendments from Phase 2 step 3, part 1 (2026-07-31) — the mint

Steps 1 and 2 removed GL's *vocabulary* and GL's *calls* from the sweep bucket.
What remains is GL's *currency*: `u32 GetRendererID()` and its aliases, which
§1.1 identified as the one integer conflating three concepts.

**This part ships the producer, not the conversion.** `Renderer/RHI/RHIResourceRegistry.{h,cpp}`
mints generation-checked `RHI::ResourceHandle`s and finally *defines* the
`GetNativeHandleForDebug` that Phase 1 declared and left undefined. Nothing is
converted onto it yet — which is deliberately the same shape Phase 1 shipped in
(a vocabulary plus a ratchet), and the three new counters in
`rhi_boundary_baseline.json` measure the remaining distance rather than
asserting zero: `sweep_renderer_id` 700, `facade_native_id_params` 68,
`backend_resolve_hatch` 0.

> **Status note (post-part-1).** The paragraph above describes what *part 1*
> shipped and is kept as written for that reason. Conversion has since begun:
> SSAO migrated end-to-end and `sweep_renderer_id` now stands at **699**. The
> live values are always the ones in `rhi_boundary_baseline.json`; numbers
> quoted in prose here and in
> [rhi-abstraction-boundary.md](../agent-rules/rhi-abstraction-boundary.md) date
> their paragraph rather than tracking the ratchet.
>
> Note also that `sweep_renderer_id` matches the *accessor name* `RendererID`,
> not the currency, so it is a regression ratchet rather than a burn-down: a
> resource whose call sites never spell `RendererID` can migrate completely
> without moving it. `facade_native_id_params` reaching 0 is the completion
> criterion.

**Why part 1 and part 2 rather than one change.** A full sweep was attempted and
abandoned. It reached 127 compiler errors from an initial 1163 and would have
converged, but it touched ~230 files and was driven largely by name-based
scripted edits, and four defect classes surfaced along the way (§(16)). The
decisive one rewrote a comparison on `WaterComponent::m_NoiseTexture` — an
`AssetHandle`, not a GPU handle — because `SSAORenderPass` has a member of the
same name that genuinely is one. It reached `Scene.cpp` and the editor's
property panel: code with no relationship to the RHI boundary. The compiler
caught it only because `UUID` happens to lack an `IsValid()` method.

The conversion is therefore redone **subsystem by subsystem against this
foundation**, each step small enough to read.

**Ordering is forced by an asymmetry**, and it is the opposite of what an
all-at-once conversion suggests. `handle -> native` is a registry lookup;
`native -> handle` is **not recoverable**, because a driver name does not
identify a registry slot. A facade that takes handles therefore *cannot* serve a
caller still holding a `u32`, while a facade offering both can serve either. So:

1. add handle-taking overloads beside the `u32` ones, with the backend resolving
   internally (resolution stays inside `Platform/<Backend>/`);
2. migrate callers one subsystem per commit — `sweep_renderer_id` falls;
3. delete the `u32` forms once nothing calls them — `facade_native_id_params`
   drops to zero in one step, and from then on a TU holding a native name
   *cannot* call the facade at all.

Only step 3 makes the boundary compiler-enforced; steps 1–2 are grep-enforced by
the counters. That is the price of keeping every intermediate state compiling and
reviewable, and it is worth paying — the alternative is the single atomic change
that was already attempted and abandoned.

### (11) `ViewHandle` is deferred to Phase 3, paired with `HeapOffset` — decided, not skipped

§1.1 names three types. Step 3 builds one of them. The reasoning, recorded here
because the handover flagged it as the decision most likely to be got wrong:

**A `ViewHandle` with no heap behind it has no behaviour that distinguishes it
from a `ResourceHandle`.** Today every sampled resource is sampled through its
whole self; there is no view cache, and the only view-producing API in the engine
(`CreateDepthArrayCompareOffView`, 2 call sites) returns a *separate GL texture
object* with its own name. Introducing a second handle type now would mean one
view per resource, minted and retired in lockstep with it — a type whose
generation can never disagree with the resource's, and therefore a type that
detects nothing. That is precisely the "rename, not an abstraction" failure §1.1
warns about, repeated one level up.

**Deferring costs Phase 3 nothing**, which is the load-bearing half of the
argument. The ~230 `BindTexture`/`BindImageTexture` sites are *already* Phase 3's
to delete (§1.3 item 1): they become `Heap::OffsetOf(view)` written into a UBO.
Phase 3 therefore edits every one of them regardless of whether its argument is
spelled `ResourceHandle` or `ViewHandle` today. The only sites that would take a
`ViewHandle` and are *not* bind sites are the two above. So deferral adds two
call sites to Phase 3's work, not a second sweep.

`HeapOffset` stays deferred for the same reason and is the matched pair: a heap
offset without a heap is a `u32` with a wrapper. Both remain **declared** in
`RHITypes.h` and unbuilt, which is exactly their Phase 1 status —
`RHIResourceRegistryTest` keeps the mutual-non-convertibility `static_assert`
alive so the property Phase 3 depends on cannot rot in the meantime.

### (12) Identity is the C++ resource object, not the native name

The registry entry is minted per resource *object* and survives that object's
storage being recreated (`ResourceRegistry::UpdateNative`, reached through
`ScopedResourceHandle::Sync`). Two consequences, and the second is a small
correctness win rather than plumbing:

- Two distinct objects never compare equal, which is the `Texture::operator==`
  defect §1.1 names — it compared `GetRendererID()`, and GL recycles names.
- One object stays equal to itself across an in-place hot-reload. That closes a
  documented sharp edge: `TextureInPlaceReloadTest`'s header told consumers they
  "must read the RendererID off the object each frame rather than caching it",
  because GL may hand recreated storage a different name. A handle survives the
  reload and resolves to the new name, so caching it is now safe.

A framebuffer's colour/depth **attachments** get their own handles, separate from
the FBO's. They are separate GL texture objects that the engine samples through
`ResolveTexture`, so "the framebuffer's handle" would have been the wrong answer
to "which texture is this".

**A recreate zeroes the native name transiently, and that nearly inverted the
whole feature.** `ScopedResourceHandle::Sync` originally treated
`nativeHandle == 0` as "released" and retired the identity. That reads as
obviously right and is wrong: `OpenGLTexture2D::InvalidateImpl` zeroes
`m_RendererID` *between* deleting the old GL object and creating the new one, so
every in-place reload retired the handle and minted a fresh one — leaving exactly
the cached-handle-goes-stale behaviour this amendment claims to fix, while the
amendment said otherwise.

`Sync` cannot distinguish a transient zero from a final one; the two are
textually identical at the call site and only the caller knows which it meant. So
`Sync` is now non-destructive ("this object's native name is now X") and
retirement is RAII-only, with `Reset()` for a deliberate early release — the
destructive act has to be named. **Generalisable: when a helper cannot infer
intent from its arguments, do not let it guess; make the rarer, more dangerous
intent the one that must be spelled out.**

Found by the GL-gated `RHIHandleNativeIdentityTest` reload case. Four other
identity assertions (registration, resolution, staleness, distinctness) passed
while this was broken — only a real reload against a real driver showed it.

**Identity-stability has a cost, and it lands on redundant-bind caches.**
`CommandDispatch::InvalidateTextureBinding` existed to stop a recycled GL name
from being skipped by the cache. That failure is now structurally impossible — a
new object always gets a new handle. But the call is still load-bearing, for the
opposite reason: on an in-place reload the handle *deliberately does not change*,
so a cache keyed on it can no longer detect that the storage underneath was
replaced, and the skipped rebind would leave the texture unit empty. Any cache
keyed on a handle needs an explicit invalidation hook on the reload path. This
generalises to §1.2's persistent-slot caching rule: "revalidate through
`Heap::OffsetOf` whenever the view could have changed" is the same obligation,
and a reload is one of the changes it means.

### (13) Retiring the identity is a separate act from destroying the object

**An obligation part 2 must honour.** Every `Delete*` virtual needs *two*
changes, and only the first is obvious. It must resolve its handle to a native
name to call `glDelete*` — and it must then `Unregister` the handle. Omitting the
second leaves the slot's generation unchanged, so a handle to a destroyed object
keeps resolving to a name the driver is free to reissue: the recycled-name
failure the layer exists to prevent, silently reintroduced at the one place most
likely to be treated as mechanical. The abandoned sweep's scripted pass did the
first and not the second, which is exactly how easy it is to miss.

### (14) `.data()` erases the type you just changed

`CreateQueries(RHI::QueryType, std::span<u32> outQueryIDs)` passes
`outQueryIDs.data()` straight to `glCreateQueries`. Re-typing the span to
`std::span<RHI::ResourceHandle>` **keeps that compiling** — and a handle is 8
bytes where a `GLuint` is 4, so GL writes names over the first half of the handle
array and leaves the rest zeroed. No diagnostic, no test failure, a corrupted
query pool. (The abandoned sweep hit this; the fix is to stage through a scratch
`std::vector<GLuint>` and register each result.)

Everywhere else in step 3 the compiler was an exhaustive checker: a type change
to a facade virtual breaks every call site at once, which is the same
"provable rather than measured" property §1.7 credits `sweep_glad_includes` with.
**A `.data()` boundary is where that property stops holding**, because the
pointer type is erased at exactly the place the payload layout matters. Any
future re-typing of a container whose contents reach a C API by pointer needs
that call site inspected by hand; grep for `.data()` on the changed container
before trusting a green build.

### (15) The bind sites keep the call and change the currency

§1.3 says `BindTexture` "does not get abstracted, it **disappears**", and that
disappearance is Phase 3's. Step 3 must nevertheless re-type its parameter,
because the producers (`ResolveTexture`, `GetRHIHandle`) and the consumer
(`BindTexture`) are the two ends of one dataflow — leaving the consumer on `u32`
means resolving a handle back to a native name in every pass, which is the
boundary breach the whole phase exists to close. **Scope a currency change by
dataflow, not by the phase table.**

This is worth stating precisely because it looks like scope creep and is not: the
~230 call sites stay **textually unchanged**; only the type flowing through them
moves. Phase 3 still deletes the call, from exactly the same sites, with exactly
the same operand.

### (16) Why the sweep is not automatable by name, and what part 2 does instead

The abandoned attempt is worth recording as a negative result, because the
obvious way to do a 700-site currency change — script it — has a failure mode
that this codebase makes concrete.

**The compiler is an exhaustive checker for *types* and no checker at all for
*intent*.** Re-typing a facade virtual breaks every caller at once, which is the
property that makes the conversion tractable. But every diagnostic it produces
has more than one valid repair, and the four defect classes below are all cases
where the tooling picked a valid-looking wrong one. Three of the four were found
by reading output, not by a failing test.

| Class | Concrete instance | Blast radius | Found by |
| --- | --- | --- | --- |
| Unanchored identifier match | `Sort.h` matched `IntroSort.h`; `Components.h` matched seven `*Components.h` | 15 legitimate `#include`s deleted across 9 files unrelated to the task, incl. `Scene/Components.h` | reading the diff |
| Rewrite that mangles the expression | `X.field == 0` → `X.!field.IsValid()` — negation inside the member access | 6 sites | syntax error (but the same rewrite on a form that *parses* silently inverts a guard) |
| Wrong side of a type mismatch | `DrawKey::SetShaderID` (packs into a sort-key bit field) and `GPUResourceInspector` (calls `glBindBuffer` on the value) re-typed to take handles | 2 confirmed | one loud, one by reading the code |
| **Cross-file name collision** | `m_NoiseTexture` is an RHI texture in `SSAORenderPass` and an **`AssetHandle`** in `WaterComponent` | 3 sites in `Scene.cpp` and the editor's property panel — the asset system, not the renderer | compiler, *by luck*: `UUID` has no `IsValid()` |

The last row is the one that decided the split. A name-keyed transformation
cannot distinguish two members that share a spelling and mean different things,
and this codebase has at least one such pair on the exact axis being converted
(GPU identity vs asset identity). Had the colliding type carried an `IsValid()`,
the change would have compiled and altered a conditional in scene rendering.

**Two consequences for how part 2 is done:**

1. **Subsystem at a time, with a readable diff.** Not because it is faster — it
   is not — but because the failure mode is a change that compiles and passes
   tests. Review has to be able to see it.
2. **Review the conditionals first.** Every defect above is either a boolean
   guard or an include; none is a type. The type changes are the part the
   compiler already checked.

The one place the type system stops helping is recorded separately in (14); it
is not a scripting artefact and applies however the sweep is done.

### (17) Namespacing was necessary but not sufficient — the graph's type is renamed

§1.7 chose `RHI::`-namespacing partly so that *"`RHI::ResourceHandle` cannot be
confused with the render graph's existing `OloEngine::ResourceHandle`"*. That
reasoning was right about **call sites** and incomplete about **headers**.

During step 3, `RGCommandContext.h` declared a member returning
`RHI::ResourceHandle` without including `RHITypes.h`. The compiler did not report
an unknown type: `RHI::` was unresolvable, so the name degraded to the
unqualified `ResourceHandle` — which *was* in scope and *was* a valid type, the
graph's string-named resource handle. A completely different concept silently
substituted for the intended one. It surfaced only because a separately-compiled
definition disagreed about the return type; in a header-only path (an inline
function, or a member defined in the same header) it would have compiled.

**The graph's type is therefore renamed `ResourceHandle` -> `RGResourceHandle`**,
which also makes it consistent with every one of its siblings in the same header
(`RGTextureHandle`, `RGBufferHandle`, `RGFramebufferHandle`, `RGResourceDesc`,
`RGResourceFormat`, …) — it was the sole unprefixed type there. 664 uses across
117 files; `Renderer/RHI/` deliberately excluded, because inside
`namespace OloEngine::RHI` the unqualified name means the RHI type.

The generalisable point, worth more than the rename: **two types with the same
unqualified name in overlapping scopes make a missing include a silent
type-substitution rather than an error.** Namespacing one of them fixes the
call-site confusion and leaves the header confusion in place. Where both names
can be in scope, they need to *differ*, not merely be qualifiable.

A secondary lesson from applying it, consistent with (16): a flat identifier
rename is the reliable scripted edit **in code only**. The same token appears in
`#include` paths (105 files broke loudly) and in prose — three comments in the
RHI tests were rewritten to say the opposite of their point, silently, in the
very files that exist to prevent this confusion. Comments are the dangerous half,
because nothing checks them.

---

## Amendments from Phase 3 (2026-08-02) — the bindless rehearsal

Phase 3 builds `ViewHandle` and `HeapOffset` — the pair amendment (11) deferred
— as `RHI::DescriptorHeap` (`Renderer/RHI/RHIDescriptorHeap.{h,cpp}`) plus an
`ARB_bindless_texture` backend (`Platform/OpenGL/OpenGLDescriptorHeap.{h,cpp}`).
The full working notes are in
[docs/agent-rules/rhi-abstraction-boundary.md §4b](../agent-rules/rhi-abstraction-boundary.md);
what follows is only what amends a decision recorded above.

### (18) §1.3's bind-site count is wrong in both directions, and the definition is why

§1.3 item 1 says **173** `BindTexture(...)` sites; the issue body says ~125. The
measured figure at `2f0503b6`, counting `BindTexture(` **and** `BindImageTexture(`
outside `Platform/` after blanking comments and string literals, is **232**.
Both older numbers are corrected in place. An image binding is a heap slot too,
so excluding it understated the phase; and as in §1's `glXxx(` story, the number
moves with the definition, so the ratchet publishes the definition alongside the
count.

### (19) The rehearsal's SHADER work is throwaway, not a dry run — and this changes Phase 6

§1.3 item 1 argues Phase 3 is worth doing because "it forces the GLSL-side and
UBO-side change on the backend we can still debug easily". **The UBO-side half of
that holds and is the valuable half. The GLSL-side half does not transfer at
all**, for a reason that is a property of the toolchain rather than of the
design:

`GL_ARB_bindless_texture` is a GLSL-only extension predating SPIR-V, with no
representation in the Vulkan target environment. Every production shader enters
the pipeline through `shaderc(target = vulkan 1.2)` (§3), so bindless GLSL is
rejected at the first hop and can only reach the driver through a
`glShaderSource` route that bypasses SPIR-V and SPIRV-Cross entirely. Vulkan
reaches the same shape through descriptor indexing, which *is* expressible in
SPIR-V and needs no second route.

Two consequences worth carrying forward:

- **The expense is in the PASSES, not the shaders.** One `.glsl` behind
  `#ifdef OLO_BINDLESS` serves both paths (measured — with the define absent the
  `#extension` line is preprocessed away before glslang sees it), so the source
  is not duplicated; only the compiled artefact is. What each REMAINING site
  needs is a "write an offset or bind a texture" fork, for as long as the
  slot-based fallback exists — which is indefinitely (see (21)).
- `BindlessShaderPipelineTest` pins the constraint and fails **in either
  direction**, because a toolchain that started accepting it would make a whole
  compile route deletable, and a silently-dead route is the thing worth
  catching.

### (20) `ViewDesc` cannot describe a view well enough for EITHER backend to build one

§1.2's `ViewDesc` carries `SubresourceRange` and `FormatOverride`, which is
enough to *key a cache* and not enough to *create the view*: GL's
`glTextureView` needs the source target and internal format, and Vulkan's
`VkImageViewCreateInfo` needs the same. The GL backend therefore declines any
view that is not the whole resource and counts the refusal.

Deliberately **not** fixed in Phase 3. The two candidate fixes — widen the
neutral desc, or let the heap ask the registry for resource metadata — differ in
where backend knowledge ends up, and choosing against a single backend is
precisely how `GLenum` reached the facade in the first place. It is a Phase 4
input, and the useful part is that the gap belongs to the neutral model rather
than to GL, so it was going to surface at Vulkan bring-up regardless.

### (21) Phase 3's ratchet counter targets a floor, not zero — a different shape from Phase 2's

`sweep_bind_texture_sites` is monotone-down with a floor guard rather than a
completion criterion, and the reasoning is a genuine departure from §1.7's:

- `ARB_bindless_texture` is not universally available, so the slot-based path is
  a permanent fallback. Zero would mean the engine had stopped working on those
  devices.
- There is **no type-system equivalent of `facade_native_id_params` available**.
  That counter could be a proof because deleting the `u32` forms made the old
  currency unrepresentable. Here both paths must compile by design, so nothing
  can make binding unrepresentable and the counter can only ever be a measure.

### (22) §1.2's caching rule needs a third clause: a STABLE identity needs an explicit invalidation hook

§1.2 says a persistent offset may be cached provided the cache holds the
`ViewHandle` and revalidates through `OffsetOf` "whenever the view could have
changed (asset reload, texture resize, streaming eviction)". Building it showed
that `OffsetOf` **structurally cannot answer that question** for a reload.

Amendment (12) made a `ResourceHandle` deliberately survive an in-place reload —
that is what makes caching one safe. A descriptor does not inherit the property:
an `ARB_bindless_texture` handle names the underlying object, so recreated
storage leaves the descriptor dangling while the view's generation is unchanged.
Revalidating through `OffsetOf` returns a perfectly valid offset to a dead
descriptor.

So the rule is: **a reload must push (`DescriptorHeap::InvalidateResource`), it
cannot be pulled.** This is the mirror of the slice-6 finding where a stable
identity made the redundant-bind cache skip a needed bind — same root cause,
opposite symptom, and it means every site that recreates a resource's storage now
owes *two* calls, `InvalidateTextureBinding` and `InvalidateResource`.

Generalisable past this issue: when a layer deliberately makes an identity
outlive its storage, every cache keyed on that identity needs an explicit
invalidation hook, because the identity can no longer report the change.

**CORRECTION (2026-08-03) — this amendment as written is dangerous, and was
followed literally into a process-killing bug.** "Every site that recreates a
resource's storage must call `InvalidateResource`" does not distinguish a
resource whose storage is REPLACED from one that is DESTROYED, and the two need
opposite handling:

| | Call | Why |
| --- | --- | --- |
| Hot reload — object lives, storage replaced | `InvalidateResource` | Releases the old descriptor and **acquires a new one**, so the view keeps working |
| **Destruction — object goes away** | **`RetireResource`** | Drops residency, poisons the slot, advances generations |

`InvalidateResource`'s re-acquire is the whole point on a reload and is fatal on
a delete: under `ARB_bindless_texture` it makes the handle **resident again**,
and `glDeleteTextures` on a texture with a resident handle is undefined. A
framebuffer resize deletes every attachment, so this is an ordinary path, not an
exotic one.

Observed as the editor exiting **silently on every maximise** — no assertion, no
queued GL error, no log line, because the fault is inside the driver. The
companion symptom was a flickering viewport. Neither is reachable by anything
this repo tests automatically: the suite never enables the heap, and a
screenshot A/B is blind to a temporal artifact by construction. It took a human
resizing a window, which is exactly the dependency
`RetiringAResourceDropsResidencySoTheTextureCanBeDeletedSafely` now removes.

Both calls are wired into the texture/framebuffer lifecycle beside their
slot-path sibling `InvalidateTextureBinding`, rather than left to call sites to
remember. That siting is the actual fix: the heap hook had **two** hand-written
call sites in the whole engine while the slot-path hook was automatic for every
texture, and an invalidation contract that depends on every future author
remembering it is one that will be broken again.

### (23) §1.2a's sampler deduplication is the one piece of new machinery Phase 4 inherits directly

§1.2a predicted the engine has "no existing concept to migrate" for the sampler
heap, and that held. It is now built: `SamplerDesc`-keyed, value-deduplicated,
refcounted, with slots deliberately **not** compacted on release — compacting
would move every later sampler slot and so rewrite offsets that live views have
already published, which is the mid-frame rewrite the transient ring exists to
avoid, reintroduced in the second heap.

One unplanned dividend, and it is a simplification rather than a cost: with a
separate sampler object, `CreateDepthArrayCompareOffViewHandle`'s **second GL
texture object becomes unnecessary**. One depth array plus two sampler objects
(compare on / compare off) yields two distinct handles and two heap slots — the
one-resource-two-views case §1.1 cites as the model's motivating example turns
out to be *cheaper* under the model than under the workaround it motivated.

### (24) The bindless compile route is a fourth shader path, and its cache must be variant-keyed

Amendment (19) established that bindless GLSL cannot enter the SPIR-V pipeline.
The route around it — `OpenGLShader::CreateProgramFromRawGLSL`, feeding the
include-resolved source to `glShaderSource` — is now built, and two of its
decisions are contracts rather than implementation details:

- **The engine injects `#extension GL_ARB_bindless_texture : require` after
  `#version`, not `include/BindlessHeap.glsl`.** GLSL requires every
  `#extension` to precede all non-preprocessor tokens, so putting it in the
  include would impose an invisible per-shader rule (*the include must sit above
  your first declaration*) on all ~35 shaders, at exactly the place authors will
  naturally put it — next to the samplers it replaces.
- **`.cached_opengl.pgr` and `.cached_opengl.bindless.pgr` are separate files.**
  A driver stamps a program binary with its own version, not with which GLSL
  branch produced it, so a shared cache would let a bindless binary load into a
  slot-based run, link cleanly, and sample nothing. This is §3's "encode the
  target env in the cache filename" one variant axis over.

### (25) The offset table is indexed by the `TEX_*` slot, and that is load-bearing

`RGCommandContext::BindTextureOrHeapOffset` records into a shared std140 UBO
(`UBO_HEAP_OFFSETS`) **at the index of the very `TEX_*` constant the pass would
have bound to**. That reuse is what §1.1 means by "the number survives, promoted
from a compile-time constant to runtime data", and it buys a property worth more
than the tidiness: the bindless and slot-based variants of one shader become
structurally unable to disagree about which texture is which, because both name
the same constant.

The measured consequence on the first converted pass (SSAO) is that **the shader
body did not change by a single character** — only the declaration block moved
inside `#ifdef`. A conversion is therefore a diff a reviewer can actually read.

The table is `uvec4[16]`, not `uint[64]`: std140 pads a `uint` array to a
16-byte stride, and the `uint[64]` spelling would read every fourth entry and
sample three wrong (but real, and plausible) textures out of four.
`OffsetsReachTheShaderThroughTheRealSeamAtTheirSlotIndices` pins it through the
real seam using slots that differ in both group and component.

### (26) Storage images are a SECOND heap-side feature, not more call sites

The heap produces sampler descriptors only. Of the 208 remaining bind sites,
**38 are `BindImageTexture`** — `imageLoad`/`imageStore` bindings, which need
`glGetImageHandleARB` with its own residency and a format/layered/level key that
`ViewDesc` does not carry.

Both APIs support it (`ARB_bindless_texture` has image handles;
`VK_EXT_descriptor_heap` treats a storage image as just another descriptor type),
so this is a gap in *this model*, not in either backend. It is recorded as an
amendment rather than a TODO because it changes how the remaining work should be
estimated: ~18% of the surface does not fall out of the sampler path at any
price, and "full bindless" costs a second descriptor kind before it costs another
call-site sweep.

---

## Amendments from Phase 3, bucket 3 (2026-08-03) — the storage-image kind

Amendment (26) predicted the shape and the prediction held: the descriptor kind
cost more than the 30 call sites it unblocked. What follows is only what amends a
decision above; the working notes are in
[docs/agent-rules/rhi-abstraction-boundary.md §4c](../agent-rules/rhi-abstraction-boundary.md).

### (27) `ViewUsage` goes on `ViewDesc` — one view description, two descriptor kinds

`RHI::ViewUsage { Sampled, Storage }` is a member of `ViewDesc`, not a separate
`StorageViewDesc` type, and the choice is decided by Vulkan rather than by GL.

Under `VkImageViewCreateInfo` **one view description serves both**: a
`COMBINED_IMAGE_SAMPLER` and a `STORAGE_IMAGE` differ in the descriptor written,
not in how the view is described. GL is the backend that makes them look like
different things, because its sampler-handle form
(`glGetTextureSamplerHandleARB`) cannot express a subresource at all without a
`glTextureView`, while its image-handle form (`glGetImageHandleARB`) takes
level/layered/layer/format inline. Splitting the *description* would have
modelled that GL implementation detail as if it were part of the neutral
contract — §1.7's `GLenum`-in-the-facade mistake, one layer up.

`StorageAccess` is the one field whose justification is backend-asymmetric, and
it is on the desc anyway: Vulkan puts read/write intent in shader qualifiers and
barriers, GL puts it in **residency**. Since `glGetImageHandleARB` takes no
access, two views differing only there are the same driver handle — so the
backend folds them and widens residency, exactly as it already folds sampler
state into a texture handle. The neutral key stays honest; the folding stays in
the backend.

### (28) A reserved null is needed per DESCRIPTOR KIND, not per heap

Amendment (26)'s companion finding, and it generalises the "unbind has no
translation" rule rather than repeating it.

Heap slot 0 holds a sampler descriptor. Constructing an `image2D` from a sampler
handle is undefined in **exactly** the way constructing one from zero is, so
pointing a cleared or failed *image* binding at slot 0 would have traded a
stale-read bug for an undefined-behaviour bug. Slot 1 is therefore reserved as
the null storage-image descriptor (`RHI::kNullStorageHeapOffset`), and
`IDescriptorHeapBackend::NullDescriptor` takes a `ViewUsage`.

**The generalisation:** when converting a binding model to an indexed one,
enumerate the operations with no index equivalent (amendment (26)'s rule) **and
then enumerate the kinds each of those operations has to answer for**. One null
per heap is only correct while the heap holds one kind.

### (29) Image units are a second index space in the same offset table

`glBindImageTexture(unit, …)` and `glBindTextureUnit(slot, …)` are separate GL
namespaces that both start at zero, and Vulkan's descriptor indexing has the same
property (a storage image and a sampled image are distinct binding arrays). The
single offset table therefore reserves a disjoint region: image unit `u` lives at
`ShaderBindingLayout::HEAP_IMAGE_SLOT_BASE + u`, and
`include/BindlessHeap.glsl`'s `OLO_HEAP_IMAGE_*` macros apply the identical base
from the same constant.

Without it, image unit 0 and `TEX_DIFFUSE` collide and each silently publishes
over the other — a wrong *real* resource, which is this model's worst failure
shape. Amendment (25)'s "both variants name the same number" property survives
because the base is applied on both sides from one place, so a converted shader
still names the image unit its bind named.

### (30) Amendment (20) is PARTIALLY closed, and the remaining half is smaller and better specified

The storage path supplies two of (20)'s three missing pieces and **exercises**
them: the concrete **format**, and the **subresource selection** (GL's
`layered`/`layer` pair and Vulkan's `baseArrayLayer`/`layerCount` pair are the
same statement twice, so `SubresourceRange` needed no new fields — see
`RHI::MakeStorageViewDesc`).

What remains is genuinely narrower:

- the **view dimension** (GL's `glTextureView` target, Vulkan's
  `VkImageViewType`), which the image path does not need; and
- resolving `FormatOverride == Unknown` against the resource's own format, which
  needs resource metadata the heap does not hold.

Both were deliberately **not** added. A neutral field with no consumer and no
test is the invented vocabulary Phase 2 step 2 paid for, and adding one now would
have meant guessing its shape against the only backend in the tree — the exact
failure (20) was recorded to avoid. So (20) stays open, with its scope reduced
from "the desc cannot describe a view" to "the desc cannot describe a view's
*shape*, and cannot resolve an inherited format".

### (32) A SHARED slot-indexed offset table is the wrong vehicle for PER-DRAW material textures

Bucket 2 (`Renderer/Commands/`) routed its binds through the seam and then stopped
short of converting the material shaders, and the reason is a property of the
offset table rather than of the material path.

`FlushOffsets()` publishes the staged offsets **and re-establishes the heap's SSBO
binding**. Per pass that is free; the table is written once and read by every draw
in the pass. The material path changes textures **per draw**, so a converted
material shader needs a flush per draw — which gives back exactly the cost §1.2
names as the performance argument for bindless ("bound once per frame, never per
draw"). Converting it that way would be a measurable regression dressed as
progress, and the ratchet would happily record it as a win.

§1.2 already describes the right shape and it is a *different* mechanism, not a
different call site: *"the offset is stable for the object's life, so it can be
baked once into material data and never touched."* Per-material offsets belong in
`PODMaterialData` / the material UBO, fetched once when the material is built,
not restaged into a shared table per draw.

So bucket 2 splits in two, and only the first half is done:

- **Done:** every bind in `Renderer/Commands/` goes through the seam
  (21 counted sites → 2, and both survivors are the `CommandDispatch::BindTexture`
  *handler's own name* — a declaration and a definition, the same
  name-collision-inflates-the-counter case §1.3's survey hit with
  `ShaderResourceRegistry::BindTexture`). With no bindless material program in
  flight the seam falls back, so this is inert until the second half lands and
  costs nothing meanwhile.
- **Deferred with a reason:** baking per-material offsets into material data. That
  is the change that makes the material path actually bindless, and it is a
  data-layout change to `PODMaterialData` rather than a binding change.

Two smaller findings from the same bucket:

- **A redundant-bind cache must not short-circuit an offset write.**
  `BoundTextures[slot]` means "this slot's GL binding is already correct", which is
  sound because a binding persists until something rebinds it. It does **not** mean
  "this slot's offset is already correct": the offset table is shared with every
  pass that binds through the seam, and those passes do not update that array.
  Skipping saves nothing either — the write is a CPU array store, not a driver
  call. `HeapBinding::WritesOffsetsForBoundProgram()` exists for exactly this
  distinction and for nothing else.
- **The fallback has to go through the CALLER'S `RendererAPI&`.** The dispatch
  handlers receive one by reference and the suite executes packets against
  `MockRendererAPI`, which records every `BindTexture`. Routing the fallback
  through the static `RenderCommand` facade instead keeps compiling, keeps working
  in the editor, and silently stops the mock ever seeing the call — the `.data()`
  trap of amendment (14) in a new place: a redirection the type system cannot
  object to.

### (31) Compute shaders needed no second compile route — check the constraint's blast radius

Amendment (19) established that bindless GLSL cannot enter the
`shaderc(target = vulkan)` pipeline, and the graphics side needed
`CreateProgramFromRawGLSL` to get around it. The natural inference is that
compute needs the same.

It does not. `OpenGLComputeShader::Compile` has always fed include-resolved GLSL
straight to `glShaderSource` and never travelled that pipeline at all, so the
whole conversion was the same `#extension` + `#define` prologue injection. **A
constraint that forced a workaround in one subsystem may simply not apply to the
next** — check before budgeting from the workaround's cost.

The genuine finding there was elsewhere and was a latent bug:
`Shader::SetBoundProgramBindless` was published by `OpenGLShader::Bind()` only,
so a bindless graphics program followed by a compute program left the flag set —
and the first converted compute pass would have written offsets for a program
that declares no offset table while binding nothing. Both `Bind()`s now publish
and both `Unbind()`s retract.

### (33) An offset is meaningless without the heap that minted it — the table must be re-based, not just the buffer

The offset table (`HeapBindingSeam.cpp`) is a process-lifetime static. It
already knew that the *UBO* had to be recreated across a heap re-initialisation,
because the GL name belonged to the previous context — amendment (27)'s epoch
comparison. What it did not do was reset the **contents**.

Offsets are indices into the heap that minted them. After an
`Initialize`/`Shutdown` pair, every slot the next pass does not re-stage still
holds a number addressing the *previous* heap's descriptor, and the bindless
shader samples it without complaint. A pass re-stages the slots it binds, so
this is invisible for those; the damage lands on slots a pass reads but does not
write.

**This is the same recurring family as (22) and (28) — state outliving the thing
that gives it meaning — and it is the third time it has appeared in this phase.**
The epoch check was in place, aimed at the buffer, and read as if it had covered
the problem. It had covered the half that produces a *loud* failure (offsets that
never arrive) and missed the half that produces a silent one.

Two further defects fell out of the same read:

- The dirty check ran **before** the epoch check, so a pass that staged nothing
  returned early and never rebuilt the buffer at all.
- A default-constructed `Scratch{}` is all zeros, and offset 0 is the **sampler**
  null. The image region therefore started life pointing `image2D` at a sampler
  descriptor — the exact UB `BindImageOrOffset`'s own fallback path takes care to
  avoid. The reset is per-kind for that reason, not a `memset`.

Pinned by `HeapGpuFixture.OffsetTableIsRebasedWhenTheHeapIsReinitialised`.

**This was real but it was NOT what the six failing suites were suffering from** —
see (34), and read the two together. Fixing it changed the failure count by zero.

### (34) A test fixture that displaces the heap singleton must put it back — and the damage never appears where the bug is

The actual cause of the six failing visual-evidence suites under
`OLO_RHI_BINDLESS=1` was four lines in a *non-GPU* unit test.
`HeapFixture::TearDown` (`RHIDescriptorHeapTest.cpp`) stood a fake backend over
`DescriptorHeap::Get()` and, when done, called `Shutdown()` — leaving the
process-wide heap **off** for everything that ran afterwards.

Every test in that file passes either way, because they all drive the heap
directly. The victims were Fog, VolumetricFog, ContactShadow, EASU, SSAO and
GTAO, all of which run later in the suite and all of which pass in isolation.

The mechanism is amendment (33)'s sibling and the same asymmetry `SetEnabled`
now warns about: a shader's bindless-or-not variant is decided at COMPILE time
and cached, so switching the heap off afterwards does not rebuild those
programs — it only stops the seam publishing the offsets they still read.

**Three diagnostic traps, all of which cost time here:**

1. **A moving failure set means one shared-state bug, not N independent ones.**
   Excluding the heap fixtures changed *which* suites failed rather than how
   many. That was read as evidence about the individual suites; it was evidence
   about the ordering. Attributing per-test was measuring the wrong thing.
2. **A two-test repro can hide an order bug a full suite exposes.** Running
   `HeapFixture` immediately followed by the Fog test PASSES — with no earlier
   tests, Fog's shaders compile *after* the heap is already down, so they compile
   slot-based and stay self-consistent. The bug requires shaders compiled
   bindless FIRST. A minimal repro that passes is not proof of innocence when the
   thing being tested is order.
3. **Correlate against execution order before theorising.** Every failing suite
   sat after the fixture's line in the log and the one failure before it had an
   unrelated known cause. That correlation was available from the first full run
   and would have pointed straight at the culprit.

Both fixtures now capture the engine's backend, desc and enabled flag on entry
and restore them on exit, via the new `DescriptorHeap::GetDesc()` — restoring the
*flag* rather than forcing bindless on, since a run that did not ask for it must
stay on the slot path. `GLStateGuard::kUboSlots` was also raised from
`UBO_FLUID_RENDER` (48) to `UBO_HEAP_OFFSETS` (56): the offset table — the one
binding every converted pass depends on — sat outside the leak detector's
tracked range, so the detector built for this class of bug could not see it.

---

## Amendments from Phase 3, closing bucket 1 (2026-08-07)

### (35) Amendment (32) was half right, and the wrong half blocked every remaining conversion

(32) declined to convert the per-draw paths because "a converted material shader
needs a flush per draw, which gives back exactly the cost §1.2 names as the
performance argument for bindless." That reasoning then generalised past
materials: terrain patches, foliage layers, decals and water were all left
slot-based on the same grounds, and with them roughly two thirds of the shader
tree.

**The cost it names is the TABLE UPLOAD, and that cost was avoidable in four
lines.** `HeapBinding::StageOffset` marked the table dirty on every write,
including a write of the value already there — and consecutive draws in a bucket
overwhelmingly restage the *same* offsets, because they share textures. The
redundant-bind cache cannot suppress those writes (amendment (32)'s own second
finding is that it must not), so the only place the distinction can be made is
the stage itself. Guarding the dirty flag there turns a per-draw flush into a
bool test in the common case.

What genuinely remains per draw is `DescriptorHeap::Flush`'s unconditional
`BindHeap()` — one `glBindBufferBase` against the up-to-nine `glBindTextureUnit`
calls the conversion removes. That is a win, not a give-back.

**Two things generalise past the four lines:**

- **A cost argument that blocks a design should be re-derived when it starts
  blocking more than it was measured on.** (32) was written about the material
  path, where per-material offsets in the material UBO were the right answer and
  still are. It was then reused as a general reason, and nothing re-checked
  whether the mechanism it described was actually load-bearing outside the case
  it came from.
- **Every draw in `CommandDispatch` now publishes, and the uniformity is the
  point.** Which handler owes a flush is a property of the SHADER it dispatches,
  and that changes as shaders convert — so pairing them per site is a rule that
  has to be re-derived on every `.glsl` edit. That is §5c's failure mode one
  level up, and it fails the quiet way: the pass keeps rendering and reads the
  previous flush's offsets. Making the publish unconditional removes the pairing
  rather than documenting it.

### (36) A texture bind has TWO spellings, and the ratchet counts one

`sweep_bind_texture_sites` counts `BindTexture(` and `BindImageTexture(` — the
`RendererAPI` facade's spelling. `Texture::Bind(slot)` is the same act through
the resource object, and the counter is structurally blind to it: `Bind` is an
overloaded name across `SoundGraph`, `Templates/Function.h` and every buffer
type, so no text rule tight enough to exclude those is also loose enough to catch
`m_CSMTextureArray->Bind(slot)`.

Twenty-five such sites existed when this bucket started. Seventeen were the
reason seven shaders looked "unconvertible": `IBLPrecompute`,
`AssetPreviewRenderer`, `ImpostorBaker` and `FoliageRenderer` bound their inputs
with `Texture::Bind` and the seam never saw them, so a converted shader would
have read an offset nobody staged. All seventeen now route through the seam.

The remaining eight are **deliberate** and three of them are load-bearing:
`ShadowMap` (×2), `WindSystem` and `SnowAccumulationSystem` are exactly the
"a direct `Texture::Bind()` that never consults the seam at all" mechanism the
§5c allowlist depends on — a shared `include/` header's declaration cannot be
converted per-includer, so its slot must be bound unconditionally instead.
`ShaderResourceRegistry` (×2) is the generic shader-resource path,
`Renderer2D` the excluded 2D batcher, `VideoTexture` a video frame.

**No counter was added for this**, and the reason is the finding: a ratchet whose
rule is "the identifiers I could think of" produces exactly the false confidence
§1.3's survey was written to prevent. The honest artefact is this list plus the
completeness test, which measures the thing that actually matters — whether every
shader is converted or a recorded decision — rather than a proxy for it.

### (37) "Done" for a sweep is every item converted OR a recorded decision, and it has to be machine-checked

Phase 3's shader half had no end condition. A shader left slot-based renders
correctly (the seam forks per program and an unconverted one gets a real bind),
so it is indistinguishable from one nobody reached — and "39 remaining" stayed
true across three rounds because nothing forced the number to mean anything.

`BindlessShaderPipeline.EveryShaderIsOnTheRouteOrExplicitlyExcluded` supplies the
end condition: every shader declaring a sampler or image is either on the route
or carries a reason. Four reasons proved genuinely distinct, which is why a
single "not yet" bucket would have been worse than none —

1. **a shared `include/` header**, whose own `#ifdef OLO_BINDLESS` *is* the route
   opt-in token, so converting a declaration there drags every includer onto the
   raw-GLSL route and unbinds all of THEIR slot-based samplers;
2. **a sampler target with no reserved null** (`sampler2DMS`), where converting
   would hand-reintroduce the wrongly-typed-null defect that cost four wrong
   diagnoses on the rebase pop;
3. **a mechanism bindless replaces rather than wraps** — the 2D batcher's
   32-element sampler array plus its 32-case switch, whose real conversion is a
   per-quad heap offset in the vertex format;
4. **a harness fixture**, driven outside the render graph inside
   `ScopedSlotBasedShaders`, where the heap's frame-scoped lifetimes have no
   frame to be scoped to.

The table is checked in **both** directions — an entry naming a shader that is
now converted, or that declares no samplers, fails too. An exception list that is
only checked one way decays into a way to silence the test, which is the same
trap the §5c allowlist records for itself.

### (38) `SamplerDesc{}` had to become an INHERIT, because no table of defaults is right for four targets

A bindless descriptor bakes the sampler in; the slot path samples with the
**texture object's** parameters. So `SamplerDesc{}` is not a style choice — it
decides what a converted shader sees while an unconverted reader of the same
texture keeps seeing the object's state, and the two have to agree.

**The first attempt was to fix the defaults, and it was wrong in an instructive
way.** `OpenGLTexture2D` and every framebuffer attachment are `GL_REPEAT` (GL's
own default) while the struct said `ClampToEdge`, so converting `Water.glsl` —
whose FFT displacement field is *tiled* — collapsed the wave field into a handful
of enormous flat terraces. Changing the default to `Repeat` fixed that and
**broke the terrain arrays**, because `OpenGLTexture2DArray` is `CLAMP_TO_EDGE`
for colour and `CLAMP_TO_BORDER` for depth. A follow-up review found two more
faces of the same defect:

| target | what the object carries |
|---|---|
| `Texture2D`, framebuffer attachments | `REPEAT` |
| `Texture2DArray` (colour) | `CLAMP_TO_EDGE` |
| `Texture2DArray` (depth) | `CLAMP_TO_BORDER`, opaque-white border |
| `TextureCubemap` | `CLAMP_TO_EDGE` |
| `Texture3D` | whatever the caller passed |
| **any integer format** | **`NEAREST`, mandatory** |

That last row is not a preference. GL makes an integer texture with a `LINEAR`
filter *incomplete*, and an incomplete texture samples as **zero** — `texelFetch`
included. `Texture.h::IsIntegerFormat` already records what it cost the first
time: every Slug glyph vanished, on AMD only, with the draw calls and the logs
looking healthy. The heap had reintroduced it for three textures — the `RG16UI`
font band texture, the `R16UI` GTAO Hilbert LUT and the `R32I` entity buffer.
There is also a fifth face: `LinearMipFilter` defaults true, which resolves to
`GL_LINEAR_MIPMAP_LINEAR` and makes a *single-level* texture incomplete — the
hazard `HeapBinding::ShadowDepthSampler` works around **by hand**.

**So the answer is not a better table, it is not having one.** A caller that
expresses no sampling intent is asking for what the slot path would have used, so
`AcquireSampledDescriptor` mints those views with `glGetTextureHandleARB`, which
bakes the object's own state — parity *by construction*. A caller that states
intent still gets `glGetTextureSamplerHandleARB`, which is what models a split
heap and is what the sites that genuinely differ from their texture use
(`ShadowDepthSampler`'s comparison-on/comparison-off pair over one depth array,
SSAO's `Nearest`+`Repeat` noise). All five faces close at once, and
`HeapBinding::CubeSampler()` — invented during the first attempt — was deleted
rather than kept, because dead vocabulary is worse than none.

**"No stated intent" needed a discriminator, and the discriminator needed a
backstop.** Inferring it from *equality with the defaults* leaves one thing
inexpressible: a caller wanting `Linear`+`Repeat` **explicitly** on a colour
`Texture2DArray` — whose object is `ClampToEdge` — would silently get the object.
`SamplerDesc::Source` says which it is, and since it is part of the defaulted
`operator==` it flows into the view memo key and the sampler-slot dedup for free.

But making `Source` the *sole* test recreates the same class of bug one level
over: set `MinFilter`, forget `Source = Explicit`, and your whole sampler is
replaced by the texture's state. That is not hypothetical — deleting the line
from `HeapBinding::ShadowDepthSampler` and running the suite gave **136 passing
tests with the shadow comparison sampler quietly inheriting**. So a desc whose
FIELDS say something is treated as explicit whatever its `Source` says, and the
disagreement is warned about. The fallback is the pre-discriminator behaviour, so
a forgotten line costs a log line rather than a frame. Both halves are pinned by
`HeapGpuFixture.SamplerSourceDistinguishesInheritFromExplicitAndSurvivesAForgottenDiscriminator`,
verified to fail when the backstop is removed.

**What it costs, stated plainly.** The plain form re-admits the GL-ism the neutral
`SamplerDesc` exists to keep out. Vulkan has no "inherit" — a `VkSampler` must be
described — so every site passing a default desc today is a site Phase 4 has to
give real sampler state. That is **199 of 209** seam call sites, and
`Stats::DefaultSamplerInherits` counts them at runtime so the work is *measured*
rather than discovered. Recording the size of that debt is the point; hiding it
behind a defaults table that is wrong for two targets out of five is not.

**The measurement is the transferable part.** §4e of
rhi-abstraction-boundary.md had already retired the *live* ON-vs-OFF A/B, because
on an animated scene the same-config noise floor is as large as the signal. The
suite's fixed-camera visual-evidence PNGs are bit-identical across consecutive
same-config runs — a floor of exactly **0.000** — so the same comparison there
resolves anything at all:

| | RMSE | pixels >8 |
|---|---|---|
| noise floor (heap ON, two consecutive runs) | **0.000** | **0.000%** |
| heap OFF vs ON, before | 22.670 | 39.93% |
| heap OFF vs ON, after | 2.783 | 1.76% |

And the residual is characterised rather than waved off: amplified 6x it is
**hairline contours around the foam boundaries**, max 58 with nothing above 64 —
a `smoothstep` threshold crossing differently because the two compile routes round
the last bit differently. That is §7a-bis's phenomenon applied to a threshold
instead of to depth, and it is a property of having two compilers.

Four things generalise:

- **When a neutral struct's default has to agree with a backend, the honest
  design is often to ask the backend rather than to guess well.** The table above
  has five rows and no majority answer; any default is wrong somewhere.
- **A fix that turns one bug into another is a signal about the shape**, not about
  the value. `ClampToEdge` -> `Repeat` moved the failure from 2D to arrays, which
  is what said the mechanism was wrong rather than the constant.
- **A green suite is not evidence about pixels.** Every image in the table came
  from a passing test, in both configurations, before and after.
- **A zero noise floor is what makes a small number readable.** 2.783 would be
  invisible against the live scene's floor of 9.74; against 0.000 it is a
  measurement, and it is what let the residual be identified rather than assumed.

---

## Amendments from Phase 4 (2026-08-07) — Vulkan bring-up

### (39) §2's "nothing reads `GetAPI()` during static init" was wrong — `RenderCommand`'s backend is constructed before the flag is parsed

`RenderCommand::s_RendererAPI` is initialised at static init by calling
`RendererAPI::Create()` (`RenderCommand.cpp:6`), which switches on `s_API` —
i.e. the one read this section said did not exist has been there all along.
It is benign *today* for a narrow reason: `s_API` is constant-initialised to
`OpenGL`, so the static-init read is well-defined and always sees the default —
which means the constructed backend is **always the OpenGL one**, regardless of
what `--rhi=` later selects. Phase 4 tolerates this deliberately: the Vulkan
bring-up never routes through `RenderCommand` (`Renderer::Init` is skipped
entirely under `--rhi=vulkan`), so the dormant `OpenGLRendererAPI` object is
never `Init()`ed and makes no GL calls. **Phase 5 must not inherit this
silently**: the moment Vulkan routes command execution, `s_RendererAPI` has to
be (re)created *after* selection — either lazily on first use or explicitly
from `Application`'s constructor after `SetAPI`. The setter's doc comment
(`RendererAPI.h`) carries the same warning at the place the next reader will
actually look.

One visible symptom, benign but worth recognising: every `--rhi=vulkan` run
ends with `[RHI/GL] OpenGLRendererAPI destroyed without ShutdownGpuResources()`
in the log — that is this dormant object being destroyed at atexit, tripping a
pre-existing guard that was written for a *real* GL run shutting down out of
order. Under Vulkan it never held resources; the message goes away when Phase 5
fixes the construction timing.

### (40) The "config setting" fallback had no home — no engine-level config is readable before `Window::Create`

§2's selection chain assumes a config setting exists to fall back to. It did
not: the editor's preferences are **project-scoped** and load inside
`EditorLayer`, long after `Window::Create` — unusable for a decision the window
creation itself depends on. Phase 4 introduced the minimal honest form:
`config/renderer.yaml` (relative to the process working directory, i.e.
`OloEditor/config/renderer.yaml` for the editor), read by
`SelectRendererBackend` (`Renderer/BackendSelection.{h,cpp}`) — a pure function
(no logging, no static writes) so the chain is unit-testable headlessly
(`BackendSelectionTest`). The editor dropdown, when it lands (Phase 7+), writes
this file and applies on restart. Two degrade rules worth restating because
they are asymmetric on purpose: an *unavailable* backend (unknown name, or
`OLO_WITH_VULKAN=OFF`) degrades to OpenGL **with an error logged** — the binary
genuinely cannot honour the request; an *incapable device* (below ADR 0010's
contract) **refuses to initialise** inside `VulkanContext::Init` — degrading
there is what ADR 0010 forbids.

### (41) Vendored Vulkan headers must out-rank the installed SDK's, and TWO defaults do the opposite

The engine already carries an SDK include dir on every TU's path — the shaderc
toolchain's `find_package(Vulkan)` — so vendoring Vulkan-Headers 1.4.357 only
pins the backend's compile if the vendored dir reliably *wins the include
search*. Three traps, all hit:

- **volk's `VOLK_PULL_IN_VULKAN` (default ON) prefers `find_package(Vulkan)` —
  the installed SDK — over an existing vendored `Vulkan-Headers` target.** On a
  machine with an older SDK installed, volk (and everything including `volk.h`)
  would silently compile against that SDK's headers. It is OFF in
  `vendor/CMakeLists.txt` and the vendored `Vulkan::Headers` is linked
  explicitly instead.
- **Link-propagated INTERFACE include dirs come AFTER the target's own.** The
  vendored dir arriving "via the volk link" loses the /I order to
  `${Vulkan_INCLUDE_DIRS}`, which sits in `OloEngine`'s own PUBLIC include list
  — so the SDK's `vulkan_core.h` still won. The fix is explicit:
  `${vulkan-headers_SOURCE_DIR}/include` is listed in that same include list
  **before** `${Vulkan_INCLUDE_DIRS}` (and the vendor propagation loop exports
  the variable). Both are plain `/I` paths; only order decides.
- **`Vulkan::Headers` is an ALIAS resolved per directory scope.** The vendor
  scope resolves it to the vendored target; `OloEngine/CMakeLists.txt`'s later
  `find_package(Vulkan)` can mint its own, SDK-pointing `Vulkan::Headers` in
  *its* scope. Engine code must therefore link `volk`/`vma` and never
  `Vulkan::Headers` by name.

The backstop for all three is a
`static_assert(VK_HEADER_VERSION >= 357)` in `VulkanContext.cpp` — if the SDK's
headers ever win the search on a below-floor SDK, the build fails naming the
mechanism instead of failing to declare `VkPhysicalDeviceDescriptorHeapFeaturesEXT`.

### (41a) `vulkan-1.lib` must not be linked at all — under `/FORCE:MULTIPLE` its thunks silently REPLACE volk's globals

The engine had linked `Vulkan::Vulkan` (vulkan-1.lib) since the shaderc
toolchain arrived, harmlessly: no engine code called a Vulkan function, so no
import-library member was ever pulled. Phase 4's first run crashed with an AV
one millisecond into instance creation, and the mechanism is worth recording
because every ingredient was pre-existing and individually reasonable:

- volk declares the core entry points as **data** (`PFN_vkCreateInstance
  vkCreateInstance`) and populates them at `volkInitialize`; an import library
  declares the same names as **function thunks** (`jmp [__imp_…]`).
- The repo links with **`/FORCE:MULTIPLE`** (the assimp/MaterialX pugixml
  collision workaround), so this duplicate did not fail the link — the linker
  silently picked the import thunk for the backend's data references.
- A load through that "pointer" reads the thunk's *instruction bytes* and calls
  the result: instant `0xc0000005`, with a useless stack.

Diagnosis that worked, in order: a standalone volk repro compiled clean and ran
clean (→ not the loader/layers/headers); `dumpbin /imports:vulkan-1.dll` on the
test exe showed **exactly the backend's own call list imported** (→ the
references resolved to the import lib); `dumpbin /symbols` on
`VulkanContext.obj` showed the references were correctly data-typed (→ the
substitution happened at link, not compile). Fix: `Vulkan::Vulkan` is removed
from the link entirely (volk's own contract — the loader is dlopen'd);
`find_package(Vulkan)` stays for the SDK include dir + shaderc/glslang libs.
Generalisable: **under `/FORCE:MULTIPLE`, a name collision between a data
symbol and an import thunk is not a link error — it is a runtime AV** — so
every new dependency that self-loads an API (volk-style) must audit the link
line for that API's import library.

### (42) What Phase 4 deliberately did NOT build, so Phase 5 doesn't go looking

- **No `VulkanRendererAPI`.** ~100 no-op virtuals inviting silent fallthrough;
  every factory switch instead carries a loud `case API::Vulkan:` assert naming
  Phase 5/6. The `-Wswitch`-with-no-`default:` convention (amendment table in
  `RHIEnumLoweringTest`) is what forced every switch to take an explicit
  position — adding the enum member was self-enforcing.
- **No ImGui under Vulkan.** `imgui_impl_opengl3` until Phase 8; `Application`
  skips the ImGui layer + overlays and both apps skip their main layers, so
  `--rhi=vulkan` shows exactly the cleared window the checkpoint asks for.
- **No split graphics/present queue families** (refused at the gate, recorded
  in ADR 0010's contract fill-in), **no swapchain-maintenance1 present fences**
  (recreation uses `vkDeviceWaitIdle` — fine at bring-up frame rates), **no
  `…PropertiesEXT` heap minima** (deferred to the phase that first allocates a
  heap; pinning unallocated minima would be ADR 0010's "authoritative-looking
  guess").
- **Present-sync shape that Phase 5 should keep:** per-frame acquire semaphore
  + fence, per-swapchain-IMAGE present semaphore (the
  `VUID-vkQueueSubmit-pSignalSemaphores-00067` reuse rule), FIFO-only.

---

## Amendments from Phase 5 (2026-08-08) — the render graph's execution layer

Phase 5 landed the three §1.8 deliverables (the unified access enum, the Clear
split, external→Undefined) plus the barrier translation, the VMA-backed
transient pool, and the command dispatcher on Vulkan. Seven findings correct
or sharpen what earlier phases predicted.

### (43) The WAW fix belongs at barrier EMISSION, not in the transition build

§1.5's plan was "unify the transition record's enum pair". Implementing it
showed the record was downstream of the defect: `BuildResourceTransitions`
re-derived the consumer's access by scanning its READ declarations, and no
record type can recover what that scan already dropped. The fix that works is
one line earlier — `PlannedBarrier` now captures `ToAccess` AT EMISSION, where
the planner is holding the exact consuming declaration (a read for RAW, a
WRITE for WAW). The rescan is deleted, not patched. Everything else landed as
designed: `ResourceTransition` carries `RHI::Access FromAccess/ToAccess`,
`ProducerPass == "external"` yields `Access::Undefined` (Vulkan discards),
`RGWriteUsage::Clear` maps to `ClearAsLoadOp` (its only engine producer,
OITPrepare, is a load-op clear — an explicit transfer clear must be declared
`TransferDest`), and the §1.5 read-while-attached input is a computed
`ReadWhileAttached` flag (consumer also declares an overlapping attachment
write). Pinned by `RenderGraphResourceTransitions.WriteAfterWriteTransition
CarriesTheConsumersWriteAccess` and siblings.

*Generalisable:* when a derivation is lossy, fix the producer to carry the
fact, not the consumer to guess better — the guess was the bug.

### (44) The barrier facade is ONE virtual carrying BOTH currencies

`RendererAPI::IssueBarrierBatch(MemoryBarrierFlags, span<RHI::Barrier>)` is
the §1.5 demotion made literal: the flags are the GL lowering, the barrier
span is the neutral truth, and each backend consults exactly one. The GL
implementation delegates to the pre-existing `MemoryBarrier(flags)` —
byte-identical behaviour, which is what kept the goldens out of play. The
planner still derives the flags (`Resolve*BarrierFlags` did NOT physically
relocate to `Platform/OpenGL/`; they are now labelled as the GL lowering and
feed only the flags field — the §1.8 table never listed the move, and doing
it would have rebuilt the proven GL path for zero behaviour change).
`TextureAspect` / `SubresourceRange` / `Barrier` moved `RHIResources.h` →
`RHITypes.h` for the facade's include set — the amendment-(5) MemoryResidency
move-don't-duplicate precedent, third use.

### (45) Handle resolution is an EXECUTE-time act; the plan stays name-keyed

The submission IR's `MemoryBarrier` commands carry the per-consumer
transitions (deduplicated on (resource, range, from, to)), but resolution to
`RHI::Barrier` happens per frame in `RenderGraph::ResolveTransitionsToBarriers`
— a transient's physical changes across frames under pooling, so a handle
baked into the cached plan would be the stale-pool-read archetype
(render-graph-transient-aliasing.md) built into the IR. Dedup keeps
LAST-writer semantics: earlier writers are ordered transitively through the
WAW barriers between the writers themselves. **Recorded caveat (Phase 7
hardening item):** two prior writers touching DISJOINT subresources of one
resource collapse to the last writer's source masks — no WAW barrier chains
them (their ranges don't overlap), so the earlier writer's pipe is
under-synchronised. No current graph hits it; sync validation is the
instrument that will name the pass that first does. This closed a second
identity gap on the way: `PhysicalBuffer` now carries `Handle` alongside
`BufferID` (set by materialization/alias fan-out, CLEARED by native imports —
the item-4 grep-every-assignment rule found one more site-class).

### (46) The layout state machine: the tracker owns oldLayout, exactness per run

GL has no image layouts, so this state machine is new, and its two rules are
the phase's core correctness claims. (1) The TRACKER is authoritative for
`oldLayout`, never the transition's `FromAccess` — a pooled transient
re-acquired this frame is in whatever layout its previous tenant left, and
first use is UNDEFINED (which also forces the source masks to NONE: discarded
contents have nothing to make available). `FromAccess` contributes only
source stage/access masks. (2) A range whose subresources sit in DIFFERENT
layouts (HZB: mip 0 attachment-written, mips 1..N storage-written) is split
into maximal equal-layout runs, one `VkImageMemoryBarrier2` per run — one
guessed layout for a mixed range is a validation error at best and a silent
hazard at worst. Granularity is per (mip, layer) with registered extents.
Pinned headlessly by `VulkanBarrierLoweringTest` (fabricated handles) and
on-device by `VulkanRenderGraphExecutionTest`'s two-frame sequence (frame 2
must transition FROM tracked layouts, not UNDEFINED).

### (47) The "Vulkan-side dispatcher" is the backend BEHIND the existing table

The Molecular-Matters dispatch table was already backend-neutral — its
functions take `RendererAPI&` — so Phase 5 added no second table:
`VulkanRendererAPI` behind the same packets is the dispatcher. Amendment
(42)'s "~100 no-op virtuals inviting silent fallthrough" objection is met
with three tiers, none silent: REAL implementations (barriers, transient
clears, viewport/scissor dynamic state, device queries, debug labels);
RECORDED pipeline-key state for the ~33 state setters
(`VulkanRecordedPipelineState` — Vulkan bakes this into the PSO, so recording
IS the correct Phase 5 semantics and hands Phase 6 its pipeline-key material);
and warn-once + counted stubs for everything pipeline-shaped
(`GetPhase6StubHitCount` — the execution test pins that state packets never
touch it). Recording context: `BeginRecording(VkCommandBuffer)`/`EndRecording`
brackets, because Vulkan has no implicit current context. The two
`RenderCommand::` calls inside CommandDispatch's bind-cache helpers were NOT
rerouted: post-(48) they reach the same selected backend as the injected
`api`, and re-plumbing ~30 call sites for identity's sake is churn — recorded
as a known quirk instead.

### (48) Amendment (39) closed — and the first live run caught a feature-gate rule

`RenderCommand::RecreateForSelectedBackend()` runs in `Application`'s
constructor immediately after `SetAPI`, before `Window::Create` — the only
moment the swap is legal (nothing has routed, nothing holds a captured
reference). The dormant-OpenGL-object atexit warning under `--rhi=vulkan`
dies with it. The finding the validation gate then produced on its FIRST
device run: **a barrier stage mask may only name stages whose device
features are ENABLED** (VUID-VkImageMemoryBarrier2-dstStageMask-03929/-03930)
— the graphics shader-stage union named tessellation/geometry stages the
bring-up device never enabled. Resolution: `VulkanDevice` enables
`tessellationShader`/`geometryShader` WHEN SUPPORTED (never required — they
are not ADR 0010 contract rows, and requiring them would silently widen the
gate), and `VulkanRendererAPI` ANDs every per-resource barrier's stage masks
with the enabled-stage mask (a disabled stage can hold no work, so narrowing
loses no synchronisation; the catch-all ALL_COMMANDS/ALL_TRANSFER bits pass
through). Sync validation is now always on in debug builds
(`VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT` chained at
instance create), and the debug messenger counts ERROR-severity messages —
`VulkanDevice::GetValidationErrorCount() == 0` is an ASSERTABLE property, not
a log to eyeball. Its second catch, minutes later, was in PHASE 4'S OWN
frame loop: the swapchain UNDEFINED→TRANSFER_DST transition used
`srcStageMask = TOP_OF_PIPE`, which does not order the layout transition
against the acquire semaphore's wait at the CLEAR stage — a WRITE_AFTER_READ
hazard against `vkAcquireNextImageKHR` that core validation cannot see (the
structure is legal; only the timeline is wrong). The rule, now in the code
comment: **the first barrier that writes an acquired swapchain image must
carry the acquire-wait stage in its srcStageMask.** Phase 4's "zero
validation errors" bar was real but ran without the sync layer; Phase 5's
bar includes it.

### (49) VMA resource notes a later phase will otherwise rediscover

- `DEPTH24STENCIL8` resolves to `VK_FORMAT_D32_SFLOAT_S8_UINT`, not
  `D24_UNORM_S8_UINT` — Vulkan mandates only one of the two and AMD ships
  only the D32 variant. 3-channel formats widen to RGBA (no mandated
  optimal-tiling support). STORAGE usage is dropped for sRGB-resolved and
  multisampled images (guaranteed format-feature validation errors
  otherwise).
- Deferred reclaim is generation-counted (destroy at ≥ 2
  `NotifyFrameCompleted`s, tied to `kFramesInFlight = 2`) because
  `TransientPool::Clear()/Trim()` destroy pooled objects while prior frames
  may still execute — on GL the driver refcounts; on Vulkan inline
  destruction is UB. `FlushAll()` is the device-idle path.
- A Vulkan resource's `GetRendererID()` is 0 — the diagnostics-only field has
  no native GL name to report, and `olo_render_transient_plan`'s `glId` shows
  0 until Phase 8's capture parity (the identity fields beside it answer the
  alias question).
- Full-frame graph execution through the `RenderCommand` facade inside a
  real window loop is deliberately NOT wired this phase: pass `Execute()`
  bodies need PSOs, so the editor's `--rhi=vulkan` path still ends at the
  Phase 4 clear+present. The execution layer's on-device proof is the
  device-gated test (headless `VulkanDevice`, real queue submits, zero
  validation errors) — Phase 6 owns moving it into the frame loop with the
  first rendered pass.

---

## Amendments from Phase 6 (2026-08-09) — the shader path

Phase 6 landed §3 (the `VkPipelineCache` + target-env-keyed SPIR-V tier), §4
(root data as one pushed pointer, the frame arena), §5 (thin PSOs, vertex
pulling, EDS3 dynamic blend) and §6 (`RHI::GpuFence` on timeline semaphores),
and moved the frame loop from clear-only to its first rendered pass. The
checkpoint held: `PostProcess_FXAA.glsl` — the golden-tested pass — renders on
Vulkan **bit-identical to the GL golden (RMSE = 0)** through the full
root-pointer + thin-PSO + vertex-pulling shape
(`VulkanShaderPipeline.FxaaGoldenPassRendersCorrectlyOnVulkan`), with zero
validation errors, sync validation included. Seven findings.

### (50) The MAPPING model: classic binding declarations survive — §4 is a pipeline-creation contract, not a GLSL rewrite

The part of §4 this ADR could not know before implementation:
`VK_EXT_descriptor_heap`'s `VkDescriptorSetAndBindingMappingEXT` (chained per
`VkPipelineShaderStageCreateInfo`) maps a shader's EXISTING `layout(set,
binding)` declarations onto root-data sources — `INDIRECT_ADDRESS` reads a
buffer block's GPU address from `rootPtr + offset`, `HEAP_WITH_INDIRECT_INDEX`
reads a texture's heap slot index the same way and scales it by the heap's
descriptor stride. The push carries exactly 8 bytes (`vkCmdPushDataEXT`, the
root struct's address), per §4's budget warning.

Consequence: **UBO and sampler declarations need NO per-backend GLSL branch.**
The same SPIR-V serves slot-based GL and heap-bindless Vulkan; which bytes
feed binding 7 is decided at pipeline creation. This is amendment (25)'s
"both variants name the same constant" property promoted one level: the
`VulkanRootDataLayout` builder assigns field offsets AND emits the mapping
array, so the draw-time writer and the pipeline cannot disagree. The one
authoring-side change left is vertex pulling (§5): a vertex stage grows an
`#ifdef OLO_VULKAN` branch reading an SSBO at the reserved binding 57 (the
root struct carries its address), because removing
`VkPipelineVertexInputStateCreateInfo` removes the attribute path. Samplers
are EMBEDDED per pipeline for now (`pEmbeddedSampler`); the §1.2a sampler
heap composes in when the engine heap runs on this backend (see (56)).

Also learned here: descriptors are written from **view descriptions**
(`VkImageDescriptorInfoEXT::pView` is a `VkImageViewCreateInfo*`), not view
objects — `VkImageView`s exist on this backend only as dynamic-rendering
attachment views.

### (51) Once `VkPhysicalDeviceVulkan12Features` exists, promoted-feature structs must fold into it

Enabling `timelineSemaphore` (§6) and `bufferDeviceAddress` (§4) added the
1.2 aggregate struct to the device chain — and validation immediately flagged
Phase 5's standalone `VkPhysicalDeviceShaderAtomicInt64Features`
(VUID-VkDeviceCreateInfo-pNext-02830: a feature promoted into an aggregate
may not be chained alongside it). `shaderBufferInt64Atomics` moved into the
aggregate. The rule for every later phase: the FIRST time a
`VkPhysicalDeviceVulkanXYFeatures` struct joins the chain, sweep the chain
for standalone structs of features that version promoted. Also enabled here,
same defaults-OFF class as amendment (48)'s `synchronization2`:
`dynamicRendering` (required-in-1.3, backs `VkPipelineRenderingCreateInfo` —
no `VkRenderPass` objects exist in this backend), and VMA gained
`VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT` (without it,
`vkGetBufferDeviceAddress` on a VMA buffer is undefined).

### (52) Pipeline destruction has ONE owner — and §3(d)'s "InflightFrameManager" was a stale name

The first device run double-destroyed every pipeline: §3(d)'s shader→pipeline
reverse index (in `VulkanPipelineCache`) and the builder's key→pipeline cache
map each enqueued the same `VkPipeline` for reclaim — and worse, after a
reverse-index invalidation the builder's map still held the dead pipeline and
would have handed it straight back to the next draw. The fix is structural,
not a flag: **the reverse index and the lookup map must be one data
structure** (`VulkanPipelineBuilder::InvalidateShader` erases and enqueues in
the same act; `VulkanPipelineCache` is the disk blob only). Relatedly, §3(d)
names `InflightFrameManager` as the deferred-destruction vehicle — that class
is dead GL-era code (`MAX_FRAMES_IN_FLIGHT = 3`, no Vulkan wiring); the real
machinery is `VulkanDeferredReclaim` (`kFramesInFlight = 2`), which Phase 6
generalised beyond VMA pairs to `VkPipeline` and `VkSemaphore` entries.

### (53) Phase 5's reclaim queue had NO production drain — the fence wait is the only legal drain point

`VulkanDeferredReclaim::NotifyFrameCompleted()` had exactly one caller: the
test fixture. A live `--rhi=vulkan` session enqueued forever and leaked until
exit. It is now called at the ONE point that proves the GPU finished a frame
slot — immediately after `vkWaitForFences` in `VulkanContext::SwapBuffers` —
alongside `VulkanFrameArena::BeginFrame(slot)` (the root-data arena's cursor
rewind is gated on the same proof). `FlushAll()` is likewise now wired at
context teardown (nothing called it either). *Generalisable:* a
generation-counted queue is only as real as its production tick; a queue the
tests tick and the frame loop doesn't is a leak with green tests.

### (54) Measured on the first live run

- **FXAA parity is exact** (RMSE = 0 vs the GL golden, 128×128): the same
  GPU's compiler consuming shaderc SPIR-V from the same source produces
  identical arithmetic on both routes. The #734 property invariants
  (blend fraction, complementary pairing) pass on the Vulkan output too.
- **The pipeline cache works across processes**: cold FXAA pipeline ~1.4 s
  (driver compile), warm ~150 ms with `pipeline_cache.vkpc` loaded (37 KB
  after two pipelines). §3(c)'s soft-fail load was exercised by design, not
  yet by corruption.
- The 4090/610.88 floor has **all 31 EDS3 feature bits**; the three blend
  states are enabled and blend is fully dynamic (`BakedBlendHash` stays 0 —
  §5's fallback column is compiled, branch-tested via
  `IsDynamicBlendStateEnabled()`, and expected to stay cold on desktop).
- NVIDIA's image descriptor stride is **32 bytes**; the resource heap's
  reserved range is ~94 KiB (`minResourceHeapReservedRange`) — heap layout
  cannot be assumed, always derive from
  `VkPhysicalDeviceDescriptorHeapPropertiesEXT`.
- The SPIR-V tier renames landed: the GL path's shared tier is
  `.cached_vulkan12.<stage>` and the Vulkan backend's own tier is
  `.cached_vulkan14.<stage>` (§3(b)); existing caches invalidate once.

### (55) The live frame loop renders through Phase 6 scaffolding, NOT through the dispatch table — deliberately

Amendment (49) said "full-frame graph execution moves into the window loop
with the first rendered pass." What Phase 6 wired is narrower on purpose: a
pilot block in `VulkanContext::SwapBuffers` renders the FXAA checkpoint pass
(uploaded golden hard-edge pattern → dynamic rendering into the swapchain,
with per-frame root data from the arena), falling back to the Phase 4 clear
when assets are unavailable. `CommandDispatch`'s draw packets still hit
Phase 6 stubs — converting the POD dispatch path is Phase 7's pass-by-pass
port, which now has a proven template to copy (the pilot + the device-gated
test are line-for-line the target shape). The acquire-semaphore wait stage
rule from amendment (48) recurred here: the render path waits at
`COLOR_ATTACHMENT_OUTPUT`, the clear path at `CLEAR`, and the first
swapchain barrier's `srcStageMask` must match whichever ran.

### (56) What Phase 6 deliberately did NOT build, so Phase 7 doesn't go looking

- **`RHI::DescriptorHeap` does not run on Vulkan yet.** The engine-side heap
  singleton (slot lifetime, generations, poisoning, the offset-table seam)
  initialises with the GL renderer, which `--rhi=vulkan` skips.
  `VulkanResourceHeap` is the backend primitive (heap buffer, descriptor
  writes, `vkCmdBindResourceHeapEXT`); a `VulkanDescriptorHeapBackend :
  RHI::IDescriptorHeapBackend` composes over it when render-graph execution
  arrives — the interface was audited for fit (`AcquireDescriptor` → write,
  `UploadSlots` → slot-region memcpy, `BindHeap` → `CmdBind`).
- **`RHI::GpuFence`'s render-graph attachment point** is
  `SubmissionCommand`'s `BatchBegin`/`BatchEnd` (whose backend-mapping
  comment has promised exactly this semaphore lowering since Phase 5) — the
  primitive is live (staged ops drain into the frame submit; device-gated
  test), the graph wiring is §6's stated Phase 7 profiling work.
- **Compute shaders still have no SPIR-V route** (`OpenGLComputeShader` goes
  straight to `glShaderSource`; §3(a)'s "tier 1 is shared" holds for graphics
  stages only). The Vulkan compute path is Phase 7 work alongside the first
  compute pass, on the `VulkanShader` pattern.
- **The Y-flip decision** (negative viewport height vs. projection flip) is
  still open — a fullscreen pass is orientation-symmetric end-to-end, so the
  pilot could not force it. It must be decided with the first 3D pass.
- The frame arena's capacity is a fixed 16 MiB per slot with counted
  overflow; growth/chaining policy is deferred until Phase 7 has real
  per-frame numbers.

---

## Amendments from Phase 7 (2026-08-09/10) — the pass suite

Phase 7 ported the render-pass suite: every Wave A post-process pass, every
Wave B compute pass, and the Wave C geometry/deferred set, each pinned by a
device-gated tenant in `VulkanPassSuiteTest` that runs the **unmodified pass
body** through the **real render graph** on the process-global
`VulkanRendererAPI`. The checkpoint held at the first tenant and never moved:
`FXAARenderPass` — the pass Phase 6's pilot proved by hand — matches the same
GL golden through the graph machinery that replaced the pilot, with zero
validation errors and sync validation on.

The phase's dominant lesson is methodological, so it comes first.

### (57) A ported pass is not proven by "it ran" — the tenant contract is what finds the bugs

Phase 7's unit of work is not "convert a pass" but "convert a pass **and pin
its defining property analytically**": the vignette darkens corners, chromatic
aberration splits channels at an off-centre edge (it is zero at the centre —
an on-centre probe passes for a broken implementation), an identity LUT is a
byte-exact passthrough, DOF's focus **gates** the blur in both directions, the
contact shadow darkens the crease while the near side of the depth step stays
lit (sign correctness), TAA's two-frame history round trip lands the 0.1/0.9
blend, deferred lighting matches a CPU mirror of `PBRCommon` term for term.

That bar is what turned eleven silent backend bugs into named failures. The
weaker bars all pass while the frame is wrong: "the test ran" passed with a
**legal zero-index draw** rendering nothing; "zero validation errors" passed
with a `Clear()` wiping the previous target; "the pass didn't early-return"
passed with every sample collapsing to texel (0,0). Every wave fixture
therefore carries the same instrument set, and each instrument exists because
it once separated two indistinguishable failures:

- **seam probes** (`DidDraw`, `GetTarget()`) — a pass body's guard chain
  early-returns silently by design;
- **`graph.GetResolveFailures()`** — the graph records every failed resolve
  with pass name and reason;
- **prepared/dropped draw counters** on the backend, reset per recording —
  they catch a dropped draw, though *not* a recorded-but-empty one;
- **`GetTransientPlan()` dump on failure** — separates "planned but not
  materialized" from "never planned";
- **an intermediate readback** between producer and consumer — separates
  "producer drew black" from "consumer read black";
- **`GetPhase6StubHitCount() == 0`** — nothing fell through to a stub;
- **`GetValidationErrorCount() == 0`** in TearDown, sync validation included.

### (58) Port the facade's DEFAULT-ARGUMENT semantics, not just its signatures

`RendererAPI::DrawIndexed(va, indexCount)` carries an unwritten contract: GL
derives `indexCount ? indexCount : va->GetIndexBuffer()->GetCount()`, and
**every pass body calls the no-count form**. The Vulkan arm passed 0 straight
into `vkCmdDrawIndexed` — a perfectly legal draw of zero indices. No
validation error, no warning, `PrepareDraw` succeeding, root data pushed,
nothing rasterized. It was masked twice over: the facade unit test passed an
explicit count, and Phase 6's pilot hand-recorded `vkCmdDraw`.

This is the archetype for a facade sweep. A signature match proves nothing
about a defaulted or sentinel argument whose meaning lives in the *other*
backend's implementation. Enumerate the sentinels (`0` = all, `NoAttachment`,
a null handle meaning "unbind") before declaring an entry point ported.

### (59) The two-flavour projection seam: a Y-flip is not one matrix

Vulkan's clip-space Y points the other way and its depth range is [0,1]. The
decision (§5's deferred item) is a **projection-seam flip at GPU upload**, not
a negative viewport — a negative viewport breaks every shader reconstructing
position from `uv → ndc → invProj`, which this engine does throughout.

The non-obvious half, found analytically rather than by a failing pixel: the
full flip `F = diag(1,-1,½,1)` with `F[3][2] = ½` makes Vulkan's **stored
depth equal GL's** (`d = (ndc_z+1)/2`). So every shader doing `depth*2-1`
reconstruction — motion blur, deferred, fog, SSAO, SSGI, SSR, contact shadow —
is already fed GL-shaped z and needs **no** z remap. Composing `inverse(F·VP)`
for those consumers double-applies it and mis-reconstructs everywhere except
the far plane.

The seam therefore has two documented flavours behind one helper
(`RHIProjectionSeam`, identity on GL):
- **full `F`** for matrices a rasterizer consumes (`gl_Position`);
- **row-flip only**, with any inverse **recomputed from the flipped matrix**
  (never invert-then-flip), for matrices shader math consumes.

Corollaries: `PrevViewProjection` must travel the same path as its current
sibling or velocity/TAA breaks; front-face winding is translated in the
pipeline builder's state translation, **not** at call sites — and that
translation is the **identity**, because Vulkan computes the facing
determinant in framebuffer coordinates (y down) where GL uses window
coordinates (y up), so that inversion and `F`'s clip-y negation already cancel
(adding a third swap turned every solid mesh inside-out once); and
direction-addressed captures (sky/IBL/DDGI cube faces) take a **third**
flavour, `AdjustCaptureProjectionForBackend` — the z remap **without** the y
flip. Compensating "at the face bases" is *not* expressible: `lookAt` derives
`right = cross(forward, up)`, so negating `up` also flips `right`, which is a
180° roll rather than a mirror. Omitting the y flip is what keeps a captured
face's rows byte-identical to the GL bake, which is what direction→texel
addressing requires.

A **third** consumer class therefore exists, and the deciding question is not
who *writes* a matrix but who *reads* it: any shader doing `ndc.xy * 0.5 + 0.5`
or `uv * 2 - 1` against an uploaded matrix needs a flavour decision, including
consumers that live far from the `CameraUBO` writers (shadow *sampling*
matrices, decal inverse-VP, HZB reprojection, the planar-reflection lookup,
the debug-draw VP). Enumerating by writer misses exactly those.

### (60) One Vulkan descriptor set collapses GL's separate binding namespaces

GL gives uniform blocks, storage blocks, samplers and image units their own
number spaces; this engine used that freedom. Binding **57** was
simultaneously `UBO_DEBUG_DRAW`, `TEX_DDGI_VISIBILITY`, and — as of §5's
vertex pulling — the engine-wide vertex-pull SSBO. Under one Vulkan
descriptor set that is a genuine collision wherever two of them meet **inside
one shader**, which is the precise test (per-shader reflection, not global
uniqueness): a shader that pulls vertices *and* includes the DDGI sampler
header is broken; a shader that pulls vertices while some *other* shader uses
UBO 57 is fine.

Two consequences worth carrying: renumber **before** branches multiply (the
renumber forced a knock-on chain through the shader-graph slot base, the
`MAX_ENGINE_TEXTURE_SLOTS` bound, the heap-image base, and the heap-offset
table's uvec4 rounding — all guarded by existing static asserts, which is why
it was a mechanical morning rather than a hunt); and reserve the pull bindings
as a **documented pair** — 57 for vertex stream 0, 63 for stream 1 (the
skinned bone-influence buffer is its first tenant, foliage/particle instance
streams its second and third), because a VAO with two vertex buffers has no
other way to reach the second stream when there is no vertex-input state.

### (61) Bare uniforms cannot enter SPIR-V, and the no-op setter makes the failure silent

GL compute never travelled the SPIR-V route (`glShaderSource` takes raw GLSL),
so compute shaders accumulated bare `uniform float x;` declarations fed by
`ComputeShader::SetFloat`. On Vulkan those declarations are illegal and
`VulkanComputeShader::Set*` is a deliberate no-op — the values read as zero,
silently. The migration is mechanical (move them into a pass-owned `std140`
block, legal on both routes since GL compute at 460 core takes UBO blocks) but
the *diagnosis* is not, because nothing errors.

Two findings sharpen it. First, the persistent-state amplifier: ToneMap's
auto-exposure writes into an SSBO that **survives frames**, so a single NaN
latches forever — the no-op setters would have produced exactly that. Second,
**per-dispatch** values (HZB's per-mip-batch parameters, the denoiser's
ping-pong axis) migrate the same way with no ring machinery: a `SetData` per
dispatch is a `glBufferSubData` on GL and a fresh arena-versioned address on
Vulkan (§4), so both backends see per-dispatch values without a race.

### (62) The graph's own contracts bite first — three of them

Three engine-side behaviours cost more debugging time than any Vulkan detail,
and every future pass-suite fixture inherits them:

- **Transient materialization is opt-in.** `SetTransientMaterializationEnabled`
  defaults false and production enables it once, in the renderer's graph
  setup. A standalone-graph fixture that forgets it gets empty physicals and
  null in-pass resolves — which reads exactly like a backend bug.
- **A pass node's `m_Enabled` defaults false** for passes the pipeline drives
  from settings. A disabled pass declares no output in `Setup` (while still
  running its input scan) and early-returns in `Execute`.
- **Bind every binding the shader declares.** An unbound UBO is a zero root
  address on Vulkan, i.e. a zero-filled block — `min(uv, bounds)` against a
  zeroed DRS block collapses every sample to texel (0,0). Grep the shader for
  `binding =` and feed all of them; the real pipeline always does.

### (63) The layout tracker needs transitions the render graph never emits

The graph's barrier planner does not lower a **framebuffer-kind write** to an
image barrier — attachment layout transitions are the draw front-end's job.
Without them every first use renders into an `UNDEFINED`-layout image and the
next sample barrier lowers with an empty source scope. So the lazy
dynamic-rendering scope, at open, transitions each attachment through the
tracker (guessing the source access from the tracked layout: exact for
`UNDEFINED` and attachment-to-attachment, conservative otherwise) and batches
those transitions **before** `vkCmdBeginRendering` — a rendering scope cannot
contain `vkCmdPipelineBarrier2`.

Three more layout lessons from the same surface:

- **An attachment WRITE access lowers to READ|WRITE.** `loadOp LOAD` reads the
  attachment; a transition into the attachment scope that promises only WRITE
  trips sync validation at begin-rendering. As a *source* scope the extra read
  bit is ignored, so one spelling serves both directions.
- **Attachment fan-out is aspect-blind unless you make it aspect-aware.** The
  graph fans one framebuffer transition into per-attachment barriers reusing a
  single access prototype — depth attachment included — with the documented
  contract that the backend derives each attachment's aspect and layout from
  its own image. A lowering that maps `ColorAttachmentWrite` blindly emits
  `COLOR_ATTACHMENT_OPTIMAL` on the depth image (illegal for its usage) *and*
  poisons the tracker so the next scope-open emits the inverse error. Remap
  the access by aspect at the top of the lowering, where `LowerAccess` and
  `LayoutFor` both inherit it.
- **GL's implicit mid-pass visibility is real work here.** Sampling a
  just-rendered attachment (or just-copied image) inside one `Execute` has no
  barrier in the GL-shaped body. Bind time is the seam: transition
  ATTACHMENT/TRANSFER layout runs to `SHADER_READ_ONLY` there. `GENERAL` is
  deliberately left alone — compute store-then-sample chains keep it by
  design.

### (64) Backend-blind singletons and factories are the port's quiet failures

Three unrelated crashes shared one shape: engine-level code that had never
needed to ask which backend was running.

`Texture3D::Create` and `Texture2DArray::Create` lived in
`Platform/OpenGL/*.cpp` and unconditionally built the GL class — fine with one
backend, a **null glad function pointer** in a process with no GL context.
`GLStateGuard`'s constructor called GL state queries unconditionally, so every
guard-carrying pass body faulted the moment it executed on Vulkan. The rule
that falls out: a factory or a process-wide guard must live in a neutral TU
with an explicit backend switch, and `rhi-abstraction-boundary.md`'s include
audit should treat "constructs a GL object" and "queries GL state" as the same
leak.

The fixture-side sibling: `RenderCommand::RecreateForSelectedBackend()` only
**constructs** the API object — it does not `Init()` it. A fixture that swaps
the process-global backend and restores it leaves a fresh GL object whose caps
were never queried (`GL_MAX_DRAW_BUFFERS` reads 0), and every MRT-shaped test
downstream fails in a way that looks nothing like the cause. Amendment (34)'s
restore-what-you-displaced discipline has a second half: **re-initialise what
you recreate.**

### (65) Device features are discovered by validation, one shader at a time

The capability contract (ADR 0010) covers the extension floor. The *core*
feature bits are a different list, and each one surfaced only when a specific
shader reached pipeline creation: `shaderDemoteToHelperInvocation` (glslang at
`vulkan1.4` lowers `discard` to `OpDemote` — every discard shader fails module
creation without it), `vertexPipelineStoresAndAtomics` (the debug-draw
channels write SSBOs from the vertex stage), `fragmentStoresAndAtomics`
(virtual geometry's fragment debug images), `multiDrawIndirect`,
`drawIndirectCount`, `shaderDrawParameters`, `tessellationShader`.

All are enabled **when supported** rather than demanded, keeping the
device-selection floor where ADR 0010 put it. The generalisation: expect the
feature list to grow with each shader family a port reaches, and enable from
`vkGetPhysicalDeviceFeatures2`'s answer rather than from a hand-maintained
constant.

### (66) A descriptor mapping must name its resource KIND

`VkDescriptorSetAndBindingMappingEXT` entries carry a `resourceMask`.
Emitting `ALL` for every mapping works right up until one shader declares a
sampler and a storage image at the **same numeric binding** — two ALL-masked
mappings at one (set, binding) violate VUID-11244, `vkCreateComputePipelines`
*fails*, and the dispatch silently drops. Every earlier shader had disjoint
numbers by luck; the froxel-fog pair was the first to collide.

Per-kind masks (UBO → uniform buffer, SSBO → storage buffer, combined sampler
→ sampled image, storage image → image) express amendment (29)'s
disjoint-namespace model the way the extension intends, and defuse the same
landmine for every later shader.

### (67) Only a window can prove orientation and extent — the offscreen tenant is blind to both

Forty-five device-gated tenants rendered the pass suite correctly and could
not have caught either defect the first live frame showed, for one structural
reason: **a tenant reads its output back in the same order it wrote it**, so a
uniform vertical flip cancels exactly. The swapchain is the first asymmetric
consumer in the entire system. Phase 6 recorded the same shape from the other
side — a fullscreen pass is orientation-symmetric end-to-end, so the pilot
could not force the Y-flip decision — and Phase 7 confirms the general rule:
**orientation, extent and present-layout are window-only proofs.**

The extent half is the subtler one. `FinalRenderPass` sizes its viewport from
the *graph's* spec, which the editor shrinks to its viewport panel. On GL the
rest of the default framebuffer keeps its previous content and the editor's UI
covers it; a swapchain image is **undefined outside what the frame writes**, so
the same code presented the frame in a corner surrounded by garbage. The
acquired image's extent — not the graph's — is the authority for a backbuffer
draw.

Two more live-only findings worth carrying: the engine **presents during
startup** (shader-warmup progress frames swap from inside renderer init), so a
frame recorder driven off the layer stack runs `OnUpdate` on layers that are
still attaching; and a nested present resets the command buffer the outer call
is recording into. Both need explicit latches, and neither is reachable from a
test.

### (68) A diagnostic that cannot distinguish two states will misdiagnose one of them

`CommandDispatch` warns "material texture has no heap descriptor — it will
sample the reserved null and render BLACK". On Vulkan that message fires
whenever the heap path merely **is not live** — which is the deliberate design
there, since `VulkanShader::Bind` reports non-bindless and materials travel the
slot path. The warning cannot tell "the heap is off" from "acquisition failed",
so it confidently reports the wrong cause, and a reader (this one included)
follows it to the wrong layer.

The rule: a diagnostic that names a *cause* must be able to exclude the other
causes that produce the same symptom. When it cannot, name the **symptom** and
list the candidates. This one costs little to fix and was actively misleading
during live bring-up.

### (69) What Phase 7 leaves for Phase 8, with the reasons

The port is complete and the live frame renders; these are the named gaps
between "renders" and "at parity", each with a tenant, a warn-once, or a
ratchet that fails the moment it is addressed:

- **Materials render untextured.** The heap path is deliberately not live on
  Vulkan, so materials must travel the slot path — and
  `CommandDispatch::DrawMesh`'s material half (`UploadMaterialForDirectDraw` +
  the UBO references) never reaches the backend, so the bindings arrive with no
  staged slot. The highest-value remaining item, and the one that turns lit
  grey geometry into a textured scene.
- **No skybox / IBL / reflection probes**: the cubemap CPU face upload is
  unimplemented (six layers of the existing 2D staging shape).
- **No editor UI under Vulkan**: `imgui_impl_vulkan` is Phase 8; ImGui runs
  platform-only so the editor's ImGui-touching update code can execute.
- **Entity picking is GL-only** (PBO readback).
- **The raw texture/FBO facade family** is still stubbed, which blocks the
  fluid splat/thickness body; SSAO's noise import and explicit per-binding
  `SamplerDesc` state hang off the same gap.
- **Six more `.comp` files** carry bare default-block uniforms (cloud noise,
  cloud shadow, three ocean FFT stages, precipitation feed) — the ratchet test
  names them explicitly so they cannot be mistaken for an exemption.
- **Mid-pass `GENERAL` store-then-sample** relies on GL-shaped `MemoryBarrier`;
  content-safe on desktop, spec-level debt.
- **No `R32F` colour framebuffer format** exists engine-wide — a GL-path bug the
  port surfaced.

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
