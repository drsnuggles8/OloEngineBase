# ADR 0011 — amendment bodies

The amendment **bodies** for
[0011-rhi-neutral-resource-and-binding-model.md](0011-rhi-neutral-resource-and-binding-model.md).
Split out in #691 for one reason: the decisions in §0–§6 are what bind
a reader, the amendments are what a reader *seeks into*, and keeping ~1,900
lines of the second in front of the first made every agent pay for the whole
corpus to read one contract.

**Start with the index in the parent ADR** — it lists all 88 amendments, one
line each, with whether each still binds. Come here for the body of the one you
need. `#691 amendment (N)` in a code comment means the entry numbered `(N)`
here.

The numbering rules and genre conventions live with that index and apply
verbatim to this file: **numbers are cited from code comments and must never be
renumbered or deleted**, and new amendments append at the next free number
under their own phase header.

---

### (1) `SetPolygonMode` loses its face parameter entirely

§1.7 listed the neutral replacement for `SetPolygonMode(GLenum, GLenum)` as
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

### (2) `SamplerDesc`'s two bools are not expressive enough; it gains real enums

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

### (3) `SetTextureParameter` decomposes into intent-named setters

§1.7 flagged this as the one virtual that "resists a mechanical translation"
and warned
against mirroring GL's `pname` space with an `RHI::TextureParameterName`. The
resolution: every call site in the engine sets exactly min filter, mag filter,
and wrap S/T/R — and every one of them uses a single wrap value for all axes.
So `SetTextureFilter(id, min, mag)` + `SetTextureWrap(id, mode)` covers 100% of
usage with no open-ended enum. `SetTextureWrap` sets all three axes because
`GL_TEXTURE_WRAP_R` is part of every texture object's sampler state and is inert
on a 2D target, so doing so is a faithful reproduction rather than a widening.
**There was no Phase 2 design gap here** — the escape hatch §1.7 held open (a
comment on #691) was not needed.

### (4) `UploadTextureSubImage2D`'s `(format, type)` pair collapses into one `RHI::Format` describing the source buffer

Worth recording because the naming invites a bug: this parameter is **not**
the texture's storage format.
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

Step 2 added `RHI::QueryType`, `RHI::FenceStatus`, `RHI::BlitAspect` and
`RHI::NoAttachment`, and moved `MemoryResidency` from `RHIResources.h`. The
enum-by-enum registry is not repeated here — **the live list is
`OloEngine/src/OloEngine/Renderer/RHI/RHITypes.h`**, whose own header comment
points back at this amendment. What this amendment decides is how that
vocabulary is added to and guarded:

- **Read the previous phase's declaration-only header for the *vocabulary* you
  are about to invent, not just for the types you consume.** The sweep started
  inventing a `RHI::BufferUsage` (`DynamicDraw`/`DynamicCopy`/`DynamicRead`, a
  transcription of GL's usage hints) when Phase 1 had already designed the same
  concept better as `MemoryResidency` (`DeviceLocal`/`HostToDevice`/
  `DeviceToHost`) — invisible because it sat in a header nothing consumed yet.
  What caught it was luck, a name collision with `RHIResources.h`'s *bind-flags*
  `BufferUsage`, and only in the ratchet test. The habit that needs no luck:
  before adding an enum to `RHITypes.h`, grep `Renderer/RHI/` for the **concept,
  not the name**. (Same move-don't-duplicate call as `MemoryResidency`'s: it
  also retires the `StorageBufferUsage` debt, collapsed in Phase 5.)
- **Every new enum is pinned by the last-ordinal `static_assert` + literal-token
  table in `RHIEnumLoweringTest.cpp`** established by the "One new guard"
  paragraph above (not amendment (4), which is about `UploadTextureSubImage2D`'s
  source-buffer format). One correction to that guard's reach: the last-ordinal
  assert catches an enumerator **inserted, removed or reordered**, but not one
  **appended** — appending leaves the asserted ordinal unchanged.
- **Lowering switches carry no `default:` label**, so an appended member is
  caught by `-Wswitch` instead. That absence is load-bearing rather than an
  oversight, and it makes the clang-cl CI job the enforcer (MSVC's C4062 is off
  even at `/W4`).
- **No `Count` sentinel per enum.** It makes an invalid value representable in
  the neutral vocabulary and forces a dead `case` in every lowering switch.

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

> **Closed by Phase 3.** Of §1.1's three types, step 3 built only
> `ResourceHandle`, deferring `ViewHandle` and `HeapOffset` **as a matched
> pair**: a `ViewHandle` with no heap behind it is minted and retired in lockstep
> with its resource, so its generation can never disagree — a type that detects
> nothing, §1.1's "rename, not an abstraction" failure one level up — and a
> `HeapOffset` with no heap is a `u32` with a wrapper. Deferral cost Phase 3
> nothing, because the ~230 bind sites were already its to delete and only two
> non-bind sites take a view. Both stayed **declared** in `RHITypes.h`, with
> `RHIResourceRegistryTest`'s mutual-non-convertibility `static_assert` keeping
> the property alive. Phase 3 built the pair as `RHI::DescriptorHeap` (see the
> Phase 3 preamble below). Kept because code comments cite this number.

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

The hazard rule: a `.data()` call erases the type you just changed. Re-typing
a container whose contents reach a C API by pointer keeps compiling while the
payload layout is wrong — `CreateQueries`' out-span re-typed to
`std::span<RHI::ResourceHandle>` let GL write 4-byte names over 8-byte
handles, silently. After any currency change, grep for `.data()` on the
changed containers and inspect each call site by hand; the fix is to stage
through a scratch native-typed vector and register each result.
Narrative: docs/agent-rules/rhi-abstraction-boundary.md §4 "Queries become
identities, and the `.data()` trap was real".

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

The decision: conversion goes subsystem-per-commit with a readable diff, never
scripted name-based edits — because the failure mode compiles. The compiler is
an exhaustive checker for *types* and no checker at all for *intent*, and a
name-keyed edit cannot distinguish two members that share a spelling and mean
different things (`m_NoiseTexture` is a GPU handle in `SSAORenderPass` and an
`AssetHandle` in `WaterComponent` — the exact axis being converted; only
`UUID`'s lack of an `IsValid()` made the compiler catch it). Review the
conditionals first: every defect found was a boolean guard or an include,
never a type — the types are the part the compiler already checked. The one
place the type system stops helping is (14).
Narrative: docs/agent-rules/rhi-abstraction-boundary.md §4 "Three
scripted-edit defects, and what caught each".

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

### (19) The rehearsal's SHADER work is throwaway, not a dry run — and this changes Phase 6

There is no bindless-GLSL route into SPIR-V: `GL_ARB_bindless_texture`
predates SPIR-V, shaderc rejects it at the first hop, and it reaches the
driver only through a raw `glShaderSource` route. The rehearsal's shader work
is therefore throwaway, not a dry run — Vulkan reaches the same shape through
descriptor indexing, expressible in SPIR-V with no second compile route, and
Phase 6 compiles classic-binding GLSL via the mapping model instead (see
(50)). The UBO-side half of the rehearsal is the half that transfers, and the
expense is in the PASSES, not the shaders — each remaining site needs a
"write an offset or bind a texture" fork for as long as the slot-based
fallback exists, which is indefinitely (see (21)).
`BindlessShaderPipelineTest` pins the constraint in both directions.
Narrative: docs/agent-rules/rhi-abstraction-boundary.md §4b "DISPROVED: the
shader side of the rehearsal does not transfer, and could not".

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

The heap produces sampler descriptors only; the 38 `BindImageTexture` sites
(`imageLoad`/`imageStore`) need image handles with their own residency and a
format/layered/level key `ViewDesc` does not carry. Both APIs support it, so
this is a gap in *this model*: storage images are a second heap-side
descriptor KIND, not more call sites — ~18% of the surface does not fall out
of the sampler path at any price, and "full bindless" costs that second kind
before it costs another call-site sweep.
Narrative: docs/agent-rules/rhi-abstraction-boundary.md §4b "How far "full
bindless" actually reaches — measured, not estimated".

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

The decision, still standing for materials: per-material offsets belong in
`PODMaterialData` / the material UBO, baked once when the material is built —
§1.2's "stable for the object's life" shape — not restaged into the shared
slot-indexed table per draw. Two contracts from the same bucket: **a
redundant-bind cache must not short-circuit an offset write** ("this slot's
GL binding is already correct" does not imply "this slot's offset is", and
the write is a CPU array store — `HeapBinding::WritesOffsetsForBoundProgram()`
exists for exactly that distinction); and the seam's fallback must go through
the CALLER'S `RendererAPI&`, never the static facade, or the mock silently
stops seeing the call — (14)'s trap in a new place. **(35) retracts the
generalisation** of this amendment's per-draw cost argument beyond the
material case.
Narrative: docs/agent-rules/rhi-abstraction-boundary.md §4f "The cost argument
that blocked everything was measured on a different case".

### (31) Compute shaders needed no second compile route — check the constraint's blast radius

The negative finding: `OpenGLComputeShader::Compile` has always fed
include-resolved GLSL straight to `glShaderSource` and never travelled the
SPIR-V pipeline, so (19)'s constraint simply does not apply to compute and the
whole conversion was the same prologue injection. The rule: **before building
a parallel mechanism, measure the constraint's actual blast radius** — a
constraint that forced a workaround in one subsystem may not apply to the
next. (The genuine finding nearby was a latent bug: `SetBoundProgramBindless`
was published by graphics `Bind()` only; both `Bind()`s now publish and both
`Unbind()`s retract.)
Narrative: docs/agent-rules/rhi-abstraction-boundary.md §4c "Compute needed no
second compile route — check the constraint's blast radius".

### (33) An offset is meaningless without the heap that minted it — the table must be re-based, not just the buffer

An offset is an index into the heap that minted it, so across a heap
`Initialize`/`Shutdown` pair the offset table's CONTENTS must be re-based,
not just its UBO recreated — a slot the next pass does not re-stage otherwise
addresses the *previous* heap's descriptor, silently. Staleness is a
heap-EPOCH comparison, never a pointer comparison; the buffer-recreate half
was already guarded (the epoch check that (22)'s invalidation-hook clause
motivated) and covered only the loud failure, missing the silent one. The
reset is per descriptor KIND, not a `memset` — offset 0 is the *sampler*
null, which is the wrong null for the image region (28). Same recurring
family as (22) and (28): state outliving the thing that gives it meaning.
Pinned by `HeapGpuFixture.OffsetTableIsRebasedWhenTheHeapIsReinitialised`.
Real, but NOT what the six failing suites were suffering from — see (34);
fixing it changed the failure count by zero.
Narrative: docs/agent-rules/rhi-abstraction-boundary.md §4e "Bucket 1
resumed: the revert was aimed at the wrong layer".

### (34) A test fixture that displaces the heap singleton must put it back — and the damage never appears where the bug is

The rule: a fixture that stands a fake backend over the process-wide
`DescriptorHeap` must capture the engine's backend, desc AND enabled flag on
entry and restore all three on exit (via `DescriptorHeap::GetDesc()`) —
restoring the *flag*, never forcing bindless on. A shader's bindless-or-not
variant is decided at COMPILE time and cached, so a heap switched off
afterwards leaves earlier-compiled shaders reading offsets the seam no longer
publishes — the failures land on unrelated suites that run later, never on
the fixture's own file. Diagnostic corollaries: a moving failure set means
one shared-state bug, not N independent ones; a passing two-test repro is not
proof of innocence when the subject is order; correlate against execution
order before theorising. And the leak detector's tracked UBO range must cover
`UBO_HEAP_OFFSETS` (`GLStateGuard::kUboSlots`), or the instrument built for
this class of bug cannot see it.
Narrative: docs/agent-rules/rhi-abstraction-boundary.md §4d "Converting a
shader silently changes what its TESTS have to do".

---

## Amendments from Phase 3, closing bucket 1 (2026-08-07)

### (35) Amendment (32) was half right, and the wrong half blocked every remaining conversion

The correction: (32)'s cost argument was measured on the material path and
then reused as a general reason that blocked roughly two thirds of the shader
tree — and the cost it names is the TABLE UPLOAD, avoidable in four lines by
guarding the dirty flag in `HeapBinding::StageOffset` (consecutive draws
overwhelmingly restage the same offsets, and the redundant-bind cache must
not suppress the writes, per (32) itself). What remains per draw is one
`BindHeap()` against up to nine removed `glBindTextureUnit` calls — a win.
The decision: **every draw in `CommandDispatch` publishes offsets
unconditionally** — which handler owes a flush is a property of the SHADER it
dispatches, so pairing them per site must be re-derived on every `.glsl` edit
and fails the quiet way. Re-derive a cost argument when it starts blocking
more than it was measured on; per-material offsets in the material UBO remain
right for materials.
Narrative: docs/agent-rules/rhi-abstraction-boundary.md §4f "The cost argument
that blocked everything was measured on a different case".

### (36) A texture bind has TWO spellings, and the ratchet counts one

`sweep_bind_texture_sites` counts the facade's spelling (`BindTexture(` /
`BindImageTexture(`); `Texture::Bind(slot)` is the same act through the
resource object, and the counter is structurally blind to it — `Bind` is an
overloaded name engine-wide, so no text rule can catch one without the
others. Seventeen such sites were why seven shaders looked unconvertible; the
eight that remain are deliberate, three of them load-bearing for the
shared-include allowlist. The decision: **no second counter was added** — a
ratchet whose rule is "the identifiers I could think of" is exactly the false
confidence §1.3's survey exists to prevent, so the sweep list was finished
instead, and the honest artefact is the deliberate-remainder list plus (37)'s
completeness test.
Narrative: docs/agent-rules/rhi-abstraction-boundary.md §4f "The bind-site
counter is blind to `Texture::Bind(slot)`".

### (37) "Done" for a sweep is every item converted OR a recorded decision, and it has to be machine-checked

A shader left slot-based renders correctly, so it is indistinguishable from
one nobody reached — which is why "39 remaining" stayed true across three
rounds. The end condition:
`BindlessShaderPipeline.EveryShaderIsOnTheRouteOrExplicitlyExcluded` — every
sampler- or image-declaring shader is either on the route or carries a
recorded reason, checked in **both** directions (an entry naming a shader now
converted, or declaring no samplers, also fails — a one-way exception list
decays into a silencer). The four exclusion reasons are genuinely distinct —
a shared `include/` header, a sampler target with no reserved null, a
mechanism bindless replaces rather than wraps, and a harness fixture — which
is why a single "not yet" bucket would have been worse than none.
Narrative: docs/agent-rules/rhi-abstraction-boundary.md §4f ""Done" needs an
end condition, and it has to be a test".

### (38) `SamplerDesc{}` had to become an INHERIT, because no table of defaults is right for four targets

The decision: `SamplerDesc{}` means INHERIT. A caller expressing no sampling
intent gets the texture object's own sampler state (`glGetTextureHandleARB`)
— parity with the slot path *by construction* — while stated intent still
gets the split-heap form (`glGetTextureSamplerHandleARB`). No table of
defaults is right across the five target classes, and any integer format
forces NEAREST regardless. `SamplerDesc::Source` is the discriminator, with a
field backstop: a desc whose fields say something is treated as explicit
whatever its `Source`, warned on disagreement — a forgotten
`Source = Explicit` costs a log line, not a frame. The debt is counted, not
hidden: Vulkan has no inherit, and `Stats::DefaultSamplerInherits` measures
the 199-of-209 seam sites that state no intent. Superseded in Phase 8 by (70)
for the heap path. The per-target defaults table and the RMSE evidence live
in the boundary doc.
Narrative: docs/agent-rules/rhi-abstraction-boundary.md §4f "`SamplerDesc{}`
means INHERIT, and getting there took two goes".

---

## Amendments from Phase 4 (2026-08-07) — Vulkan bring-up

### (39) §2's "nothing reads `GetAPI()` during static init" was wrong — `RenderCommand`'s backend is constructed before the flag is parsed

> **Closed by (48).** Corrected §2's claim that nothing reads `GetAPI()` during
> static init: `RenderCommand::s_RendererAPI` is initialised at static init by
> `RendererAPI::Create()`, which switches on a `s_API` that is
> constant-initialised to `OpenGL` — so the constructed backend is **always the
> OpenGL one**, whatever `--rhi=` later selects. Phase 4 tolerated it (the
> Vulkan bring-up never routed through `RenderCommand`, so the dormant object
> was never `Init()`ed) and the one visible symptom was a
> `[RHI/GL] OpenGLRendererAPI destroyed without ShutdownGpuResources()` line at
> atexit on every `--rhi=vulkan` run. (48) fixed the timing —
> `RenderCommand::RecreateForSelectedBackend()` in `Application`'s constructor,
> immediately after `SetAPI` and before `Window::Create` — and the atexit
> warning went with it. Kept because code comments cite this number.

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

The decision: the vendored Vulkan-Headers must win the include search over
the SDK dir that the shaderc toolchain's `find_package(Vulkan)` puts on every
TU's path — and the defaults do the opposite. volk's `VOLK_PULL_IN_VULKAN`
prefers the installed SDK (OFF in `vendor/CMakeLists.txt`, vendored headers
linked explicitly), link-propagated INTERFACE include dirs lose the `/I`
order to the target's own list (the vendored dir is listed explicitly before
`${Vulkan_INCLUDE_DIRS}`), and `Vulkan::Headers` is an ALIAS resolved per
directory scope, so engine code links `volk`/`vma` and never
`Vulkan::Headers` by name. Backstop:
`static_assert(VK_HEADER_VERSION >= 357)` in `VulkanContext.cpp` — a wrong
winner fails the build naming the mechanism.
Narrative: docs/agent-rules/rhi-abstraction-boundary.md §10 "Vendored headers
must out-rank the installed SDK's — three traps, all hit".

### (41a) `vulkan-1.lib` must not be linked at all — under `/FORCE:MULTIPLE` its thunks silently REPLACE volk's globals

The decision: `vulkan-1.lib` (`Vulkan::Vulkan`) must never be on the link
line — volk owns the entry-point pointers and dlopens the loader itself;
`find_package(Vulkan)` stays only for the SDK include dir and the
shaderc/glslang libs. The mechanism: volk declares the core entry points as
DATA, an import library declares the same names as function thunks, and under
`/FORCE:MULTIPLE` (the pugixml collision workaround) the duplicate is not a
link error — the linker silently picks the thunk, and a call through the
"pointer" executes the thunk's instruction bytes (`0xc0000005`, useless
stack). Every new dependency that self-loads an API must audit the link line
for that API's import library.
Narrative: docs/agent-rules/rhi-abstraction-boundary.md §10 "`vulkan-1.lib`
and the `/FORCE:MULTIPLE` thunk replacement — the diagnosis recipe".

### (42) What Phase 4 deliberately did NOT build, so Phase 5 doesn't go looking

Phase status, kept as a list. The one forward-pointing item is the last.

- **No `VulkanRendererAPI`** — ~100 no-op virtuals invite silent fallthrough;
  every factory switch carries a loud `case API::Vulkan:` assert instead. That
  objection is what (47)'s three-tier answer had to meet, and the "deliberately
  loud, never silent" contract is still cited from `VulkanRendererAPI.h`.
- **No ImGui under Vulkan** (Phase 8), **no split graphics/present queue
  families** (refused at the gate per ADR 0010), **no swapchain-maintenance1
  present fences** (recreation uses `vkDeviceWaitIdle`), **no `…PropertiesEXT`
  heap minima**.
- **Present-sync shape that later phases keep:** per-frame acquire semaphore
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

- `DEPTH24STENCIL8` resolves to `VK_FORMAT_D32_SFLOAT_S8_UINT` (AMD ships
  only the D32 variant); 3-channel formats widen to RGBA; STORAGE usage is
  dropped for sRGB-resolved and multisampled images.
- Deferred reclaim is generation-counted (destroy at ≥ 2
  `NotifyFrameCompleted`s, tied to `kFramesInFlight = 2`) — inline
  destruction of a pooled object prior frames may still execute is UB on
  Vulkan. `FlushAll()` is the device-idle path.
- A Vulkan resource's `GetRendererID()` is **0** — the diagnostics-only field
  has no native GL name to report; the identity fields beside it answer the
  alias question.
- Full-frame graph execution in a real window loop is deliberately NOT wired
  this phase — the `--rhi=vulkan` editor still ends at the Phase 4
  clear+present, and **the frame loop is Phase 6's own** (moved there with
  the first rendered pass); the execution layer's on-device proof is the
  device-gated test.

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
were EMBEDDED per pipeline for now (`pEmbeddedSampler`); the §1.2a sampler
heap composes in when the engine heap runs on this backend (see (56)) —
**that clause is superseded by (70)**, which built `VulkanSamplerHeap` and set
`pEmbeddedSampler = nullptr`. The rest of this amendment stands.

Also learned here: descriptors are written from **view descriptions**
(`VkImageDescriptorInfoEXT::pView` is a `VkImageViewCreateInfo*`), not view
objects — `VkImageView`s exist on this backend only as dynamic-rendering
attachment views.

### (51) Once `VkPhysicalDeviceVulkan12Features` exists, promoted-feature structs must fold into it

The rule: the FIRST time a `VkPhysicalDeviceVulkanXYFeatures` aggregate joins
the device chain, sweep the chain for standalone structs of features that
version promoted and fold them in — chaining both violates
VUID-VkDeviceCreateInfo-pNext-02830. Also enabled here (same defaults-OFF
class as (48)'s `synchronization2`): `dynamicRendering` (no `VkRenderPass`
objects exist in this backend), and VMA gained
`VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT` — without it,
`vkGetBufferDeviceAddress` on a VMA buffer is undefined.
Narrative: docs/agent-rules/rhi-abstraction-boundary.md §10 "Fold promoted
feature structs into the version aggregate".

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

The rule: a generation-counted queue is only as real as its production tick —
a queue the tests tick and the frame loop doesn't is a leak with green tests.
The drain point is the ONE place that proves the GPU finished a frame slot:
immediately after `vkWaitForFences` in `VulkanContext::SwapBuffers`
(alongside `VulkanFrameArena::BeginFrame`, whose cursor rewind is gated on
the same proof), with `FlushAll()` at context teardown.
Narrative: docs/agent-rules/rhi-abstraction-boundary.md §10 "A reclaim queue
nobody drains leaks with green tests".

### (56) What Phase 6 deliberately did NOT build, so Phase 7 doesn't go looking

Phase status, kept as a list because three of these deferrals are cited by name
from the Phase 7 code that closed them.

- **`RHI::DescriptorHeap` does not run on Vulkan yet** — `VulkanResourceHeap`
  is the backend primitive; a `VulkanDescriptorHeapBackend :
  RHI::IDescriptorHeapBackend` composes over it when render-graph execution
  arrives (interface audited for fit). Built in Phase 7.
- **`RHI::GpuFence`'s render-graph attachment point** is `SubmissionCommand`'s
  `BatchBegin`/`BatchEnd` — primitive live, graph wiring is §6's Phase 7 work.
- **Compute shaders still have no SPIR-V route** (§3(a)'s "tier 1 is shared"
  holds for graphics stages only) — Phase 7 work on the `VulkanShader` pattern.
- **The Y-flip decision** could not be forced by an orientation-symmetric
  fullscreen pilot — decided by (59), with the first 3D pass.
- The frame arena is a fixed 16 MiB per slot with counted overflow.

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

The decision: port default-argument semantics — `DrawIndexed(va, 0)` means
"the whole index buffer" on GL, and the Vulkan arm passed 0 into
`vkCmdDrawIndexed`, a perfectly legal draw of nothing, masked by an
explicit-count unit test and a hand-recorded pilot. Enumerate an entry
point's sentinels (`0` = all, `NoAttachment`, a null handle meaning "unbind")
before declaring it ported — a signature match proves nothing about a
sentinel whose meaning lives in the other backend's implementation.
Narrative: docs/agent-rules/rhi-abstraction-boundary.md §9b "The facade's
*semantics* are wider than its signatures".

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

GL gives uniform blocks, storage blocks, samplers and image units independent
number spaces and the engine used that freedom (binding 57 was a UBO, a
sampler, and §5's vertex-pull SSBO at once). The decisions: renumber
colliding bindings **before** backend branches multiply; reserve 57/63 as the
documented vertex-pull pair (stream 0 / stream 1 — a two-buffer VAO has no
other way to reach its second stream once vertex-input state is gone); and
the collision test is **per-shader reflection, not global uniqueness** — a
collision matters only where two meanings meet inside one compiled shader.
Narrative: docs/agent-rules/rhi-abstraction-boundary.md §9c "GL's separate
binding namespaces are a boundary assumption".

### (61) Bare uniforms cannot enter SPIR-V, and the no-op setter makes the failure silent

The decision: bare `uniform float x;` declarations (legal only on GL's
raw-GLSL compute route) migrate into pass-owned `std140` blocks — legal on
both routes, since GL compute at 460 core takes UBO blocks. The trap is the
setter's silence: `VulkanComputeShader::Set*` is a deliberate no-op, so the
values read as zero with no error anywhere. Per-dispatch values migrate the
same way with no ring machinery — a `SetData` per dispatch is a
`glBufferSubData` on GL and a fresh arena-versioned address on Vulkan (§4).
Narrative: docs/agent-rules/rhi-abstraction-boundary.md §11 "Bare uniforms
cannot enter SPIR-V, and the no-op setter makes the failure silent".

### (62) The graph's own contracts bite first — three of them

One line, because these are pure gotchas: a standalone-graph fixture must
enable transient materialization, enable settings-driven pass nodes, and feed
every binding the shader declares — each omission reads exactly like a
backend bug.
Narrative: docs/agent-rules/rhi-abstraction-boundary.md §11 "The graph's own
contracts bite first — three of them".

### (63) The layout tracker needs transitions the render graph never emits

Three decisions: (1) attachment layout transitions are the draw front-end's
job — the lazy dynamic-rendering scope transitions each attachment through
the tracker and batches those barriers **before** `vkCmdBeginRendering`,
since a rendering scope cannot contain `vkCmdPipelineBarrier2`; (2) an
attachment WRITE access lowers to READ|WRITE — `loadOp LOAD` reads the
attachment, and as a source scope the extra read bit is ignored, so one
spelling serves both directions; (3) the backend derives each attachment's
aspect and layout from its own image — remap the access by aspect at the top
of the lowering, where `LowerAccess` and `LayoutFor` both inherit it. GL's
implicit mid-pass visibility is handled at bind time (ATTACHMENT/TRANSFER
runs transition to `SHADER_READ_ONLY` there); `GENERAL` is deliberately left
alone — compute store-then-sample chains keep it by design.
Narrative: docs/agent-rules/rhi-abstraction-boundary.md §9d "Implicit
synchronisation is part of the GL API surface".

### (64) Backend-blind singletons and factories are the port's quiet failures

The rule: a factory or a process-wide guard must live in a neutral TU with an
explicit backend switch, and a process-wide guard must be inert off its
backend — "constructs a GL object" and "queries GL state" are the same leak
class as a stray include, and the include scan cannot see either. And
amendment (34)'s restore-what-you-displaced discipline has a second half:
**re-initialise what you recreate** —
`RenderCommand::RecreateForSelectedBackend()` constructs but does not
`Init()`, so a fixture that swaps and restores the process-global backend
leaves a fresh GL object with caps unqueried, failing MRT-shaped tests far
from the cause.
Narrative: docs/agent-rules/rhi-abstraction-boundary.md §9a "A factory in a
`Platform/<Backend>/` TU is a leak the include scan cannot see".

### (65) Device features are discovered by validation, one shader at a time

Core feature bits are a growing list separate from ADR 0010's extension
floor, each surfacing only when a shader family reaches pipeline creation.
The rule: enable from `vkGetPhysicalDeviceFeatures2`'s answer **when
supported** — never demanded (the device-selection floor stays where ADR 0010
put it), never from a hand-maintained constant. The canonical bit list lives
in the boundary doc's §9e — note it includes independent blend, which this
amendment's original list omitted.
Narrative: docs/agent-rules/rhi-abstraction-boundary.md §9e "Device
capability is not one contract but a growing list".

### (66) A descriptor mapping must name its resource KIND

The decision: `VkDescriptorSetAndBindingMappingEXT` entries carry per-kind
`resourceMask`s (UBO → uniform buffer, SSBO → storage buffer, combined
sampler → sampled image, storage image → image), never ALL — two ALL-masked
mappings at one (set, binding) violate VUID-11244, pipeline creation FAILS,
and the dispatch silently drops. Per-kind masks express amendment (29)'s
disjoint-namespace model the way the extension intends.
Narrative: docs/agent-rules/rhi-abstraction-boundary.md §11 "A descriptor
mapping must name its resource KIND".

### (67) Only a window can prove orientation and extent — the offscreen tenant is blind to both

The rule: **orientation, extent and present-layout are window-only proofs** —
a tenant reads its output back in the same order it wrote it, so a uniform
vertical flip cancels exactly, and the swapchain is the first asymmetric
consumer in the entire system. The extent half: a swapchain image is
undefined outside what the frame writes, so **the acquired image's extent —
not the graph's spec — is the authority for a backbuffer draw**. Two
live-only latches: the engine presents during startup, and a nested present
resets the command buffer the outer call is recording into.
Narrative: docs/agent-rules/rhi-abstraction-boundary.md §11 "Only a window
can prove orientation and extent".

### (68) A diagnostic that cannot distinguish two states will misdiagnose one of them

The rule: a diagnostic that names a *cause* must be able to exclude the other
causes that produce the same symptom; when it cannot, name the **symptom**
and list the candidates. (The concrete case: "no heap descriptor — will
render BLACK" fires on Vulkan whenever the heap path merely is not live —
the deliberate design there — and confidently sent the reader to the wrong
layer during live bring-up.)
Narrative: docs/agent-rules/rhi-abstraction-boundary.md §11 "A diagnostic
that cannot distinguish two states will misdiagnose one of them".

## Amendments from Phase 8 (2026-08-14) — completeness & verification parity

### (70) The sampler heap retires the embedded sampler, and sampler state is not a PSO axis

§1.2a's "sampler deduplication that has no GL counterpart" landed as
`VulkanSamplerHeap`: the second `VK_EXT_descriptor_heap` heap, deduplicated
by the full `VkSamplerCreateInfo` (slot 0 = the old embedded linear/clamp
default, so an unstaged binding samples exactly as before). Every combined
image-sampler root field is now TWO u32s — image heap index at `Offset`,
sampler heap index at `Offset + kSamplerIndexOffset` — sourced by the
mapping's `samplerHeapOffset`/`samplerAddressOffset` half with
`pEmbeddedSampler = nullptr`. Consequences that generalise:

- **The inherit contract needs a home the API can see.** GL keeps filter/wrap
  on the texture object; the backend's equivalent is per-image sampler state
  in `VulkanImageInfo`, stamped with §4f's per-class GL defaults at creation
  and mutated by the `SetTextureFilter`/`SetTextureWrap` facade entries. The
  per-class table matters: framebuffer attachments are CLAMP_TO_EDGE+LINEAR
  (GL's `PrepareTexture`), NOT the `Texture2D` REPEAT default — the
  chromatic-aberration tenant caught the divergence analytically as an edge
  sample wrapping to the far side of the frame.
- **An explicit `RHI::SamplerDesc` must TRAVEL the heap-off fallback.** The
  seam dropped it before reaching the backend; on GL that was harmless (the
  slot path samples object state by design), on Vulkan it silently degraded
  the ShadowDepthSampler comparison state — `sampler2DArrayShadow` reads
  through a non-compare sampler are undefined. A third `BindTexture` overload
  carries the desc; GL's default body reduces to the two-arg bind.
- **Both heaps are one binding model, so their lifecycles CASCADE.**
  `VulkanResourceHeap::CmdBind` binds the sampler heap too (a hand-recording
  caller that binds one heap and draws trips VUID-11308 — the Phase 6 pilot
  test was the first), and `Release` releases it (the first fixture teardown
  without the cascade tripped VMA's allocations-not-freed assert).
- Integer formats force NEAREST at the sampler-derivation seam — outranking
  even an explicit desc, because GL answered LINEAR-on-integer with
  incompleteness, never with linear filtering.

### (71) A deferred clear must remember WHO asked

The decision: a deferred clear is bound to the target that requested it — the
pending record carries the SAME identity triple the scope-match predicate
uses (framebuffer pointer / backbuffer view / depth-array view) plus values
captured at request time, folds into the loadOp only when the opening scope
IS the requester, and otherwise MATERIALIZES as an eager transfer clear
(per-layer for the depth-array case) at supersession and at EndRecording.
Deferral is an optimisation over GL's eager semantics, so every path where
the deferral becomes observable must collapse back to eager.
Narrative: docs/agent-rules/rhi-abstraction-boundary.md §12 "A deferred clear
must remember WHO asked".

### (72) One-shot submits are ordered BEFORE the still-recording frame

Queue submissions execute in submit order, so a blocking one-shot issued
mid-frame runs before everything the frame has recorded but not submitted.
The rules: mid-frame WRITES record into the frame command buffer (staging
buffers go to deferred reclaim, never inline destroy); mid-frame READS
flush-and-continue (`VulkanContext::FlushFrameRecordingAndWait`), with two
named refusals — the backbuffer already written (the binary acquire semaphore
belongs to the final submit) and an open query (a query span cannot cross
command buffers). And "is a Vulkan recording live" probes the LIVE object via
`dynamic_cast`, never `RendererAPI::GetAPI()` — a static flag a fixture can
set without recreating the API object.
Narrative: docs/agent-rules/rhi-abstraction-boundary.md §12 "One-shot submits
are ordered BEFORE the still-recording frame".

### (73) The projection seam's reader classes need SEPARATE UBO members

Amendment (59) said enumerate readers; Phase 8 found the readers of ONE
member split across both flavours, which no call-site fix can express —
`CameraUBO` gained `ProjectionForReconstruction` (row-flip flavour,
identical on GL, appended so every truncated `CameraMatrices` declaration
stays valid). Three sharpenings: capture-flavour writers fill the RAW matrix
(their math sibling has neither flip nor remap); `[1][1]` is negative on
Vulkan in BOTH flavours, so a pure magnitude use (tessellation screen-space
scale, VG pixel error, SSAO radius) takes `abs()` rather than a member
switch — all three were silently negative on Vulkan; and cross-stage GLSL
block declarations must agree per program (glLinkProgram rejects a
truncation mismatch between stages of one shader — extend every declaring
stage, not just the consuming one).

### (74) A sweep keyed on "ran in the live log" has a shadow, and an inclusion-list ratchet cannot see it

Two rules: a coverage claim derived from a live session inherits that
session's pass coverage — a pass that never ran in the log is invisible to
the sweep; and an INCLUSION-list ratchet cannot see what never ran — fixing
an unlisted shader does nothing until it is registered in every list. The
migration is mechanical; the REGISTRATION is the part that rots.
Narrative: docs/agent-rules/rhi-abstraction-boundary.md §12 "A sweep keyed on
the live log has a shadow".

### (75) GL's cube-face z IS a Vulkan array layer — and a partially-uploaded cube must keep a uniform layout

`CopyImageSubDataFull`'s `(level, z)` maps directly onto `(mip, arrayLayer)`
on the CUBE_COMPATIBLE image — the whole IBL/sky bake write path. The
decision: the face-upload family transitions the WHOLE image, never the
uploaded face — per-(mip,layer) tracking answers a mixed-layout whole-image
query with UNDEFINED ("discard is the safe total answer"), so a face-scoped
transition could legally discard the faces already uploaded. Mid-frame face
uploads take (72)'s write rule and face readbacks its read rule; sampled heap
views honour the image's recorded dimensionality, not its layer count.
Narrative: docs/agent-rules/rhi-abstraction-boundary.md §12 "GL's cube-face z
IS a Vulkan array layer".

### (76) A pull branch is a per-DRAW-SITE stride contract

Converting the bake/sky shader family confirmed the §5f rule at scale: the
pull branch's stride and field offsets come from the DRAW SITE's vertex
buffer layout, not from the shader's attribute list — the cube/caster draws
pull the 8-float engine `Vertex`, the fullscreen-triangle draws a 5-float
layout, and a wrong guess renders garbage with no validation error. The
families deliberately NOT converted each break the pattern's preconditions
and need their own slice: Renderer2D's batch streams carry INT attributes (a
float[] pull would reinterpret the bits), the terrain family is tessellated
with a different field order, and the editor debug draws each need their VB
layout verified individually.

### (77) Phase 8 decisions recorded: inspector currency and CI cadence

- **`GPUResourceInspector` (and the MCP JSON it feeds) surfaces BOTH
  currencies** — `RHI::ResourceHandle` identity alongside the native handle
  (via `GetNativeHandleForDebug`'s backend tag), never an opaque blob. The
  precedent is `TransientPool::AcquiredInfo`'s documented both-currencies
  answer: the native id is what a RenderDoc capture shows, the identity is
  what answers alias/recycling questions; hiding either behind an opaque
  blob serves nobody. (The relocation work consuming this decision is
  tracked in §1.6.)
- **Dual-backend CI cadence: the Vulkan device-gated suite stays a
  GPU-runner concern, not a per-PR gate.** Headless CI runs the GL suite +
  the headless ratchets (which include the Vulkan text/compile gates that
  need no device); the device-gated Vulkan tenants run wherever a GPU runner
  runs the visual/golden set — same cadence, same machine class. Promoting
  Vulkan tenants to a per-PR blocking gate needs a dedicated GPU runner and
  is a Phase 9 rollout question, not a Phase 8 one.

### (78) The null root address is a device-loss, not a soft miss — and a UBO's lifetime must span its last draw

Two decisions from one live incident. (1) A root-data buffer address of 0 is
not "deterministic zeros" — with `PhysicalStorageBuffer` pointer loads it is
a GPU page fault that kills the device.
`VulkanFrameArena::GetNullBlockAddress()` keeps a persistent 64 KiB
zero-filled block and root assembly substitutes it for every zero address
(warn-once unchanged) — an in-bounds contract only, so shaders must keep
guarding their counts (the fluid splat's counters-gated early-out is the
model). (2) On Vulkan a `UniformBuffer`'s destructor un-publishes its
binding-state occupancy, so **a UBO created for a draw must outlive the call
that records/executes that draw** — any facade-level "create, fill, bind,
drop" helper is a latent Vulkan device loss (under GL the same shape read
stale-but-valid data by accident of object lifetime).
Narrative: docs/agent-rules/rhi-abstraction-boundary.md §12 "The null root
address: a device-loss diagnosis recipe".

### (79) Row order is per-TARGET under Vulkan, and readback consumers cannot assume either convention

> **Superseded by (85).** Recorded, from the first MCP captures against a live
> Vulkan editor, that off-screen row order was **per-TARGET** — the number and
> kind of hops that produced a target decided its orientation, with no global
> "Vulkan intermediates are X" invariant — and answered it with per-consumer
> readback predicates plus an `olo_render_capture_target` `flipY` argument. The
> evidence did not survive: it was taken against a broken-geometry frame with
> (80)'s bug live (its own in-place Phase 8 correction had already reclassified
> UIComposite once). Phase 9's live inventory measured every off-screen target
> uniformly top-down under Vulkan, and (85) retires the knob and the per-target
> predicates for one backend-wide rule. Kept for the record of how the seam was
> first observed, and because (85) and the boundary doc cite this number.
>
> One fact from the same session that (85) does not restate: the backend maps
> the graph's `Depth24Stencil8` label to `VK_FORMAT_D32_SFLOAT_S8_UINT` on this
> hardware, so **a readback format table must key on the VULKAN format from the
> image-info registry, never on the graph's format label.**

### (80) A CPU-writable buffer whose address is life-stable replays GL's command-ordered writes as last-write-wins

A CPU buffer write between two recorded draws is command-ordered on GL; a
life-stable Vulkan address makes every recorded draw read the frame's LAST
upload. The decision: inside a recording bracket, a graphics-visible
`VulkanStorageBuffer::SetData` snapshots the written range into the frame
arena and the draw's root-data writer embeds the snapshot's address
(`GetRootDataAddress`); COMPUTE keeps the persistent `GetDeviceAddress` (its
SSBOs are GPU-write participants whose writes must land where the indirect
resolve looks) — the split is the `commandOrderedBufferReads` flag on
`AssembleAndPushRootData`; outside a recording bracket the write-through IS
the ordered value and no snapshot is taken. Tenant:
`InterleavedInstanceBufferUploadsKeepCommandOrderAcrossDraws`. The same
archetype is a recorded, unfixed seam for a vertex-pull stream rewritten
mid-frame between draws.
Narrative: docs/agent-rules/vulkan-command-ordered-buffer-writes.md — the
whole-file twin of this amendment.

### (81) Unfed sampler bindings must resolve to a typed null texture, not heap slot 0

The decision: an unfed sampler binding resolves to a 1x1 zero-filled null
image of the MATCHING view dimensionality (2D / 2D-array / cube / cube-array
/ 3D — reflection carries `VulkanShaderBinding::ImageDim`; a 2D descriptor
under a `samplerCube` is undefined), never heap slot 0, which is whatever
texture registered first. GL's twin reads deterministic black from an unbound
unit; Vulkan now matches. The null images are owned by
`VulkanDescriptorHeapBackend` and reclaimed with the heap. Storage-image
units keep the slot-0 fallback: there is no safe neutral WRITE target, and no
production shader dispatches with an unfed image unit.
Narrative: docs/agent-rules/rhi-abstraction-boundary.md §12 "Unfed sampler
bindings sampled the loft HDRI".

### (82) The tessellation domain origin is part of the GL-parity contract, and its failure mode is a shading bug, not a geometry bug

The decision: every patch pipeline chains
`VkPipelineTessellationDomainOriginStateCreateInfo` with
`VK_TESSELLATION_DOMAIN_ORIGIN_LOWER_LEFT` (maintenance2, core 1.1) — GL
parity by construction for water and terrain alike. Vulkan's UPPER_LEFT
default mirrors the tessellator's v for a GL-authored TES: generated
positions stay ON the patch while every emitted triangle's WINDING flips — so
the failure mode is a shading bug (inverted `gl_FrontFacing`, collapsed
fresnel on a two-sided surface; a back-culled surface would vanish outright),
not a geometry bug, with zero validation errors and every binding fed. Pinned
by the water tenant's back-cull phase. When a tessellated surface shades
wrong under one backend with all inputs verified, check the domain origin
before re-auditing the inputs.
Narrative: docs/agent-rules/rhi-abstraction-boundary.md §12 "The water-murk
hunt ended at the tessellation domain origin".

### (83) A GL framebuffer has no intrinsic size; a Vulkan one does — and a native-id gate is always a Vulkan no-op

Two rules from one incident. (1) **A gate must key on the currency that
exists on every backend — the RHI identity — never the native id**, which is
diagnostics-only and 0 by contract on Vulkan: a native-id gate
(`GetCSMRendererID() != 0`) is always a Vulkan no-op. (2) A GL framebuffer
has no intrinsic size — its size IS its attachments' — while a Vulkan
framebuffer spec drives the rendering scope's render area, so **a pass must
resize its framebuffer whenever it disagrees with its attachments'
resolution**. Both are GL-tolerated latency: always-wrong states made
load-bearing by a backend where framebuffer size and resource identity are
real contracts, forced out by the parity gate rather than any unit test.
Narrative: docs/agent-rules/rhi-abstraction-boundary.md §12 "Two stacked
shadow bugs, split by a live MCP A/B".

### (84) BDA turns a GL-tolerated out-of-bounds read into device loss — and VK_EXT_device_fault names the address

A bounded GL SSBO bind returns zeros out of range; a buffer-device-address
read has no bounds (`robustBufferAccess` does not apply) and page-faults the
device once the read crosses into an unmapped page — amendment (83)'s
GL-tolerated latency applied to memory safety. The fixes:
`OLO_INSTANCE_SINGLE` (define before including `InstanceBlock_Vertex.glsl`)
is the shape for any shader whose instancing rides its own vertex stream —
it resolves the `u_Model` family to `instances[0]` with the varying
interface unchanged, and is backend-neutral because the bug was; and
**`VK_EXT_device_fault` is enabled whenever the driver offers it**
(`VulkanDevice::LogDeviceFaultInfo` on every DEVICE_LOST translation) —
Windows gives no dmesg/Xid currency, so without the fault report a Vulkan
device loss here is unattributable. Audit "the CPU uploads as many entries as
the shader indexes" contracts per DRAW SITE, not per shader family, when a
backend moves them onto raw pointers.
Narrative: docs/agent-rules/rhi-abstraction-boundary.md §12 "254k instances,
one uploaded matrix, and `VK_EXT_device_fault`".

---

## Amendments from Phase 9 (2026-08-15) — rollout

### (85) One row order per backend — every off-screen target is backend-natural, and the per-target knob retires

Supersedes (79)'s per-target regime, whose evidence went stale: the Phase 9
live inventory (ten target archetypes — geometry products, NDC-passthrough
post hops, compute `imageStore` outputs, depth, shadow — captured raw from a
live editor on both backends) measured **every** off-screen target already
uniform: bottom-up under GL, **top-down under Vulkan**. (79)'s "per-target"
observations were taken against a broken-geometry frame ((80)'s bug was live)
and did not survive #794's fixes. The decision:

- **Every off-screen target is backend-natural: bottom-up on GL, top-down on
  Vulkan.** No per-target exceptions, no flips inside the offscreen chain.
  Why top-down is the only reachable uniform convention: (59)'s clip-y
  negation lands a view's top row at memory row 0 (the same fact the capture
  flavour exists to avoid), and un-mirroring rasterization would need the
  per-pass viewport negation (59) already rejected.
- **The present blit PRESERVES row order.** The Phase 7 negative-height
  viewport mirror at the backbuffer is deleted: it was derived from a chain
  believed to be GL-ordered, and nothing displayed the graph through that
  path since (the editor composites via ImGui; the runtime was Vulkan-gated
  until this phase) — (67)'s "orientation is a window-only proof", twice
  over. Pinned by the FinalPassBlits... tenant, now asserting presented row r
  == chain row r.
- **CPU readbacks carry ONE backend predicate, in the shared helpers**: flip
  to top-down PNG order iff GL. `olo_render_capture_target`'s `flipY`
  argument is retired (schema and meta echo removed); `CaptureFramebufferPng`
  and the save-game thumbnail capture share the rule.
- **ImGui uv pairs collapse to one predicate** —
  `RHI::RenderTargetRowsAreBottomUp()` (ImGuiLayer's same-named helper is
  the uv-facing spelling and forwards to it) — for every panel drawing a
  RENDER TARGET. File-loaded textures (stbi pre-flipped) keep the fixed
  GL-convention pair; the content browser threads provenance
  (rendered-thumbnail vs loaded-icon) to choose. Entity picking keys its
  mouse-origin conversion on the same predicate (GL arms convert to
  bottom-up; `VulkanFramebuffer::ReadPixel` takes the top-down coordinate
  verbatim).
- **Direction-addressed captures are NOT screen targets** and keep (59)'s
  capture flavour unchanged — bake rows stay byte-identical to GL, so the
  on-disk IBL cache needs no version bump.
- Named seams, deliberately not unified here: `SetScissorBox` still takes
  each backend's native scissor space (GL bottom-left, Vulkan top-left) —
  `UIRenderer::PushClipRect` now converts iff GL, and the DDGI tile scissors
  remain self-consistent per backend; a facade-level top-left contract would
  need the GL arm to learn the bound target's height and is left as a
  recorded seam. `GPUResourceInspector`'s preview panel deliberately shows
  raw memory order (a debug view of mixed-provenance textures).

### (86) OloServer stays renderer-free — the backend decision for the headless target is "neither"

§2 decided backend selection for windowed apps and deliberately left
`OloServer` open; issue #691 framed the choice as "Vulkan compute, or stays
OpenGL-only". Both arms were wrong, because the server initializes **no**
backend today: `IsHeadless` skips selection entirely, and `Renderer::Init`
has exactly one call site, inside the `!IsHeadless` branch. Nothing
gameplay-affecting requires a GPU — the fluid solver's CPU reference path is
documented as the server path, navmesh/physics/terrain are CPU by design, and
the deployment contract (docs/ops/deployment.md) is an ubuntu+libstdc++6
container with no libGL and no libvulkan. The decision:

- **OloServer remains backend-less.** `IsHeadless` continues to skip backend
  selection; no Vulkan headless-compute path is added. There is no customer,
  and adding one would put a device dependency on the one target whose
  deployment image and WSL2 host guarantee none.
- **Authored content cannot override it.** The two per-entity flags that
  could drive GL resource creation on a context-less server —
  `ParticleSystem::UseGPU` and `FluidSolverMode::GPU` — fall back to their
  CPU paths (warn-once) when no device is available, and
  `MeshSource::Build` defers GPU buffer creation the same way. This makes the
  decision enforceable rather than an accident of the server currently
  having no asset manager.
- **The Consequences bullet on `OLO_WITH_VULKAN=OFF` is corrected in place:**
  OFF removes the volk/VMA/vulkan-headers ports and the backend object code,
  but `find_package(Vulkan REQUIRED)` at configure time is deliberately
  ungated — the *shader* toolchain (shaderc/glslang/SPIRV-Cross) needs the
  SDK on every build. "Build without a Vulkan SDK present" was never true and
  is not a goal; the valve is about link weight and port count, not the SDK.
- WSL2 consequence: unchanged — the server remains the only WSL2 target and
  needs no GPU stack there.

### (87) Dual-backend CI cadence: (77) confirmed wired — and the lane that carries it is now guarded

(77) decided the cadence (headless CI runs GL + the device-free ratchets;
device-gated Vulkan tenants ride the GPU runner's nightly visual/golden run;
no per-PR Vulkan gate without a dedicated runner). Phase 9 audited the
workflows against that decision. The wiring matches — with one real gap: the
nightly `gpu-conformance-amd` lane had been red for days on a provisioning
disconnect (the #773/#781 vcpkg migration's `setup-vcpkg` action ran an
unconditional `sudo apt-get`; the Rocky runner has neither apt nor
passwordless sudo), which meant the device-gated Vulkan tenants were running
**nowhere**. The action now verifies-first and only installs on hosted
images; the runner provisioning list (docs/ops/self-hosted-gpu-runner.md)
gained the autotools row. Standing consequences:

- The AMD runner's SDK (1.4.350.1) is below ADR 0010's 1.4.357 tooling
  floor: tenants run (the capability gate reads driver extensions, not the
  SDK), but GPU-assisted validation for descriptor-heap traffic is
  unavailable until the host SDK is upgraded.
- Promoting Vulkan tenants to a per-PR blocking gate remains rejected — it
  needs a dedicated GPU runner. The nightly cadence *is* the decision, and a
  red nightly is a paged failure, not background noise.

---


### (88) The inspector's Vulkan arm DISCOVERS rather than being registered into — and the enumeration lives in the neutral registry

Issue #810, the follow-up (77) and §1.6 left open. (77) decided that
`GPUResourceInspector` must show both currencies; it did not say where a
Vulkan session's resource list would come from, and the answer decided it.

**The decision: `RHI::ResourceRegistry` grows a `Snapshot()`, and that is the
enumeration for every backend.** The alternative — teaching ~15 Vulkan
resource constructors to call `OLO_GPU_REGISTER_*`, mirroring what the GL
constructors do — was rejected because those macros would duplicate
bookkeeping the engine *already* performs: every backend resource registers
with the RHI registry at creation, and that registry is the one place that
holds the identity, the native handle, the kind and the owning backend
together. A second, hand-maintained list of the same objects is the
two-mirrors-drift shape, and the mirror nobody looks at is the one that rots.

So the two arms have deliberately different discovery models, spelled
`IResourceInspectorBackend::DiscoversResources()`:

- **OpenGL is PUSHED.** The macros fire from resource constructors, the
  shell's map is authoritative, and `DiscoversResources()` is false. Nothing
  about the GL arm changed.
- **Vulkan is PULLED.** `DiscoverResources()` takes a registry snapshot, keeps
  the Vulkan-owned rows, and enriches each from the backend-internal side
  tables — `VulkanImageInfoRegistry` for extent/format/mips/layers,
  `VulkanRootObjectRegistry` for the buffer and framebuffer objects' sizes and
  extents, `VulkanRawBufferRegistry` for the object-less raw family, and VMA's
  own `vmaGetHeapBudgets` for the per-heap totals.

**Three consequences worth not re-deriving.**

1. **The shell keys its map on the IDENTITY, not the native handle.** A Vulkan
   framebuffer registers native 0 — there is no `VkFramebuffer` under dynamic
   rendering (amendment (83)) — and an arena-backed uniform buffer has no
   native object at all, so several live resources legitimately share native
   0. Keying on it collapses them into one row and under-reports without
   erroring. For the same reason `Snapshot()` decides liveness from the
   freelist rather than from `Native != 0`, which reads like a sane liveness
   test and is not.

2. **Native ids are u64 through the whole inspector now.** A GL name widens
   losslessly; a `VkImage` truncated into a u32 is a plausible-looking wrong
   answer, which is the failure class this tool exists to prevent.

3. **Previews and the async download engine stay GL-only, and say so.** They
   are a PBO + fence pipeline feeding the GL ImGui renderer backend; neither
   exists under Vulkan, and a Vulkan `ImTextureID` would need a per-image
   `VkDescriptorSet` the panel has no lifetime story for. Those entry points
   return "unsupported" and the panel prints why, rather than returning an
   empty image that reads as "the texture is black". Pixel inspection under
   Vulkan goes through the facade readback spine instead, which is the other
   half of #810.

**A second decision came out of the readback rewrite.** Porting
`olo_render_probe_pixel` / `olo_render_target_stats` off
`glGetTextureLevelParameteriv(GL_TEXTURE_INTERNAL_FORMAT)` needed a neutral
way to ask "what format is this texture?", so the facade grew
`QueryTextureFormat` returning `RHI::TextureFormatInfo` — the neutral
`RHI::Format` where one matches, the NATIVE format enum always, plus a
backend-neutral token, channel count and float/integer/depth flags. Keyed on
the Vulkan format from the image-info registry, never on the render graph's
label, per (79). Two things it pinned:

- A **depth** readback must name a DEPTH destination format on both backends.
  GL needs it because only depth destinations lower to `GL_DEPTH_COMPONENT`
  (reading depth as `GL_RED` is `GL_INVALID_OPERATION`); Vulkan needs the same
  name because its identity fast path only fires when the image really is
  `VK_FORMAT_D32_SFLOAT`, and this hardware backs the graph's
  `Depth24Stencil8` with `D32_SFLOAT_S8_UINT`. `EncodeReadbackTexel` gained
  the `D32Float` case so one destination serves both.
- `RHI::Format` is the vocabulary the engine CREATES textures with, and it is
  narrower than what the tools must READ. A packed 11/11/10 target or an sRGB
  swapchain flavour has no `RHI::Format`, and a tool that refused on that
  ground would be refusing formats it can decode perfectly well — hence
  `Neutral` is allowed to be `Unknown` while the token and channel count still
  describe the storage.

**The afterPass clone went neutral too, and that reversed a prior decision.**
`PassSnapshotBackend.h` pinned the mid-frame snapshot's contract native at
both ends, on the stated grounds that `native -> handle` is not recoverable.
That reasoning is sound for CONVERTING a name somebody else minted; it does
not apply to the scratch clone, which that code **creates itself** and can
therefore mint WITH an identity from birth. Nobody had drawn the distinction
because nothing yet wanted the identity. The seam and its GL clone engine are
deleted; the snapshot now allocates through
`RendererAPI::CreateMatchingTextureHandle` and copies through
`CopyImageSubDataFull`, so capture / probe / stats / validate-compare run one
code path on both backends, `afterPass` included.

`CreateMatchingTextureHandle` takes a SOURCE HANDLE rather than an
`RHI::TextureDesc`, and that shape is the decision. What the caller needs is
not "a texture roughly like that one" but a destination the backend's own
image copy will accept. Routing it through a neutral format vocabulary would
mean translating the source's native format out to `RHI::Format` and back —
and `RHI::Format` is deliberately narrower than what the render graph creates
(a packed 11/11/10 target, an sRGB swapchain flavour). A near-miss
translation does not fail loudly: both `glCopyImageSubData` and
`vkCmdCopyImage` require format compatibility, so it yields an empty or
garbage clone the diagnostic then reports as fact. Phrasing the contract as
"match this" lets each backend reproduce its OWN description and never
translate. `RHI::TextureFormatInfo` grew `MipLevels` / `ArrayLayers` /
`Shape` to go with it — the shape matters because a 64-slice volume and a
64-layer array report the same count and are not interchangeable.

Two Vulkan-side consequences. The copy must record into the CURRENT frame's
command buffer, which `CopyImageSubDataRegion` already does; a
`VulkanOneShot` would execute BEFORE the still-recording frame (amendment
(72)) and silently clone the PREVIOUS one — identical output on a static
scene, wrong for every reason you would reach for the tool. And the clone is
owned by a new `VulkanRawImageRegistry` rather than a `VulkanTexture2D`,
because that class builds its image from an engine `ImageFormat` and the
whole point is not to translate.

**Every MCP render tool now answers on Vulkan.** The remaining refusals
(`olo_cluster_grid_stats`' SSBO read, `olo_froxel_fog_probe`'s volume read)
went the same way — `ReadBufferSubData` and a 1x1x1 `ReadTextureSubImage`
were already implemented on both backends. `olo_render_validate`'s `compare`
was not refusing at all: it had **no** backend guard and resolved through
`NativeTextureIdForDiagnostics`, which truncates a `VkImage` pointer to a
nonzero garbage `u32` that then reached `glGetTextureLevelParameteriv` with
no GL context. A crash, not a gap — fixed by the same identity resolve.

The one honest remaining limitation is texture PREVIEWS in the inspector
panel, which are a GL PBO+fence pipeline feeding the GL ImGui backend.
