# The OpenGL abstraction boundary — where it actually leaks, and how to measure it

Working notes for issue [#691](https://github.com/drsnuggles8/OloEngineBase/issues/691)
(add a Vulkan backend alongside OpenGL 4.6). The *decisions* live in
[ADR 0010](../adr/0010-vulkan-rhi-heap-bindless-only.md) (scope) and
[ADR 0011](../adr/0011-rhi-neutral-resource-and-binding-model.md) (the neutral
model, backend selection, PSO cache; its amendment *bodies* are split into
[0011-amendments.md](../adr/0011-amendments.md) and indexed from the parent,
so a citation like "amendment (63)" resolves there). This file is the part a
future session would otherwise have to rediscover by reading the whole
renderer.

---

## 1. Do not trust a `glXxx(` grep — three published numbers, none of them right

Three estimates of "raw GL calls outside `Platform/OpenGL/`" existed before
anyone counted carefully:

| Source | Number | What was wrong |
| --- | ---: | --- |
| Issue #691 body | ~620 | rough estimate |
| Task handover | 724 | pattern too loose — counted `glfw*` window calls as GL |
| Measured (comments/strings stripped) | **549** | — |

**The dominant error is pattern looseness, not comments.** With the same
`\bgl[A-Z]\w*\s*\(` pattern, stripping comments and string literals only removes
16 of 565 hits (~3%) — real, worth doing for a number a test asserts on, but a
rounding error next to the 175-hit gap to 724. `\bgl[A-Z]` does not match
`glfwInit`; `gl[a-zA-Z]*\(` does, and that is apparently what produced the 724.

That confusion is expensive because it invents work that does not exist: it
attributed 33 GL calls to `Platform/Linux/LinuxWindow.cpp` and 32 to
`Platform/Windows/WindowsWindow.cpp`, prompting a whole discussion about whether
window/context creation deserves an exemption bucket. **Those files contain zero
raw GL.** They are pure GLFW, and there is no exemption to argue about.

**The rule now lives in code**, in `OloEngine/tests/Rendering/RHIBoundaryRatchetTest.cpp`:
identifiers matching `gl[A-Z][A-Za-z0-9_]*` immediately followed by `(`, counted
after C/C++ comments and string / char / **raw-string** literal bodies are
blanked. Raw strings matter — there are 34 in `OloEngine/src` (GLSL snippets in
`ShaderBindingLayout.h` among them) and a naive quote-scanner mis-parses the
rest of the file after hitting one.

If you need the number again, run the test; do not re-grep.

---

## 2. The include graph is the real boundary, and it is much worse than the call count

The finding that reframed the whole phase:

- **42** files outside `Platform/OpenGL/` call GL.
- **79** files outside `Platform/OpenGL/` `#include <glad/gl.h>`.
- So **40 files include the entire OpenGL API while making zero GL calls.**

They do it to name `GLenum` / `GL_*` constants that `RendererAPI`'s *own virtual
interface* demands: `SetBlendFunc(GLenum, GLenum)`, `SetDepthFunc(GLenum)`,
`SetStencilOp(GLenum, GLenum, GLenum)`, `SetCullFace(GLenum)`,
`SetPolygonMode(GLenum, GLenum)`, `CreateTexture2D(u32, u32, GLenum internalFormat)`,
`BindImageTexture(..., GLenum access, GLenum format)`. The facade is not leaky
at its edges — GL is part of its declared vocabulary. 521 `GLenum`/`GLuint`/
`GLint`/… mentions and `GL_*` constants in 105 files sit downstream of that.

**Why this is the metric that matters:** a call count is a progress measure that
a workaround can game (wrap the call in a helper that still lives in
`Renderer/`). A translation unit that cannot see `glad/gl.h` *provably cannot
name a GL symbol*. It is a compiler-enforced property, not a grep.

### The ordering trap

`RendererAPI.h` itself includes `<glad/gl.h>`, and essentially every pass
includes `RendererAPI.h`. That is why only **3** of the 42 GL callers need a
direct `glad` include — everyone else gets it transitively.

**Consequence: deleting `#include <glad/gl.h>` from a pass today accomplishes
nothing.** The file still compiles, the symbols still arrive, the boundary is
still open, and the include counter drops while the property it is supposed to
prove has not changed. Strip the `GLenum`/`GLuint` parameters out of
`RendererAPI`'s virtuals **first**; only then is each removed include a real
guarantee, and at that point most of the 40 zero-call includers fall out
mechanically because they only ever wanted `GL_SRC_ALPHA` and friends.

### There was a SECOND transitive source, and it was wider — check the PCH

Phase 2 found this the hard way, and it is the more important half of the trap.
`RendererAPI.h` is the *renderer's* transitive glad source. There was another one
that bypassed the renderer entirely:

> `OloEngine/Debug/Instrumentor.h` included `<tracy/TracyOpenGL.hpp>`, which
> needs the GL loader's symbols and therefore pulls in `<glad/gl.h>`. And
> `Instrumentor.h` is included by **`OloEnginePCH.h`** — so *every translation
> unit in the engine* could see the entire OpenGL API, whether or not it went
> anywhere near the renderer.

With that in place, `sweep_glad_includes` could have been driven to zero while
the property it claims to prove — "this TU cannot name a GL symbol" — stayed
false everywhere. The counter would have looked like a completed phase.

It surfaced only because removing the per-file includes made Tracy's header the
*first* thing to need GL in those TUs, and it failed to compile. That is luck,
not method: had `Instrumentor.h` been listed after the renderer includes in more
files, it would have kept silently supplying the symbols.

**Method, generalisable to any "this module cannot see X" boundary:**

1. Enumerate every *transitive* path to the forbidden header, not just the
   direct includes. Start from the PCH — a precompiled header is an include that
   does not appear in any file, so grep will never show it to you.
2. Third-party headers are a live source of these. `TracyOpenGL.hpp`,
   `imgui_impl_opengl3.h`, and anything else named after a backend will drag it
   in. Their include sites need the same audit as your own.
3. Prefer deleting the dependency to relocating it. Here the three
   `OLO_PROFILE_GPU*` macros the include existed to serve were **used nowhere in
   the repo** — engine, editor, runtime and server — so the whole thing was dead
   weight buying an engine-wide leak. GPU zones now belong in a
   `Platform/OpenGL/` TU, which may legitimately see GL.

The ratchet cannot catch this class of problem on its own, because a transitive
include is invisible to a per-file `#include` scan. Only a compile failure or a
deliberate include-graph walk finds it.

### "Makes no `glXxx()` calls" is NOT sufficient reason to delete its glad include

Two ways a file needs `<glad/gl.h>` while the call-count pattern reports zero,
both hit during Phase 2's de-glad pass:

- **`GLenum`/`GLint`/`GLsizei`-typed locals and `GL_*` constants** with no call of
  their own. Obvious in hindsight; check for the *types* as well as the calls.
- **glad's own loader symbols.** `SlugFontProcessor.cpp` contains
  `if (glad_glCreateTextures != nullptr)` — using the loader's function-pointer
  table as a *"do we have a GL context yet?"* probe. `glad_glCreateTextures` does
  not match `gl[A-Z]`, because the character after `gl` is `a`. The ratchet is
  right to not count it (it is a null test, not a call), but the file genuinely
  cannot compile without the header.

  Note this pattern also needs a neutral replacement before Step 2 can finish —
  a context-presence probe is a legitimate need, but reaching into the GL
  loader's symbol table is not a portable way to express it.

The safe predicate for removing an include is "zero GL calls **and** zero
`GL*`-prefixed identifiers of any kind", verified by a compile — not by the
ratchet's call pattern, which is deliberately narrower because it is measuring
something else.

Step 2 finished with exactly four files still including `<glad/gl.h>`, and they
split along this line. Three — `BloomRenderPass.cpp`, `OITResolveRenderPass.cpp`
and `ShaderPack.cpp` — named no `GL*` identifier at all, so the include just fell
out. The fourth hit the second case: `UIRenderer.cpp` made **zero** GL calls but
typed its clip-rect stack in `GLint`/`GLsizei`. Those values are scissor-rect
*coordinates* — `i32`/`u32` — and `RenderCommand::SetScissorBox` already took
engine types, so the GL spelling was pure inertia. It was nonetheless
load-bearing: delete the include without retyping the struct and the file does
not compile.

### A `Platform/<Backend>/` include leaks just as much, and this scan cannot see it either

The PCH is the transitive path everyone warns about. There is a second one that
is much easier to walk into, because it looks like ordinary engine code:

> Three passes — `FluidIntermediatesPass`, `WaterRenderPass`,
> `VirtualMeshRegistry` — included `Platform/OpenGL/OpenGLUtilities.h` to
> construct `Utils::GLClearProgramGuard` around their clears. That header
> includes `<glad/gl.h>`. Deleting each file's *direct* glad include would have
> driven `sweep_glad_includes` to zero while all three TUs could still name
> every symbol in OpenGL.

That is the counter-gaming the baseline's own `_comment` warns about, arrived at
honestly rather than deliberately — which is what makes it worth recording.

**The fix is a layering question, not an include question.** `GLClearProgramGuard`
exists because an NVIDIA driver revalidates the bound program at clear time
(`gl-clear-program-revalidation.md`). That is *backend knowledge*. It belongs
inside `OpenGLRendererAPI::Clear{Texture,Buffer,FramebufferColorAttachment,
FramebufferDepth}` — where `ClearDepthOnly()` had been carrying it correctly all
along. Once the guard moved down, the passes needed no backend header at all.

Generalisable: when a module outside a boundary constructs a helper from inside
it, the helper is usually on the wrong side. Ask what knowledge the helper
encodes; if the answer names a vendor, a driver or an API, it belongs to the
backend, and the call site should be getting the behaviour for free rather than
opting into it.

Audit `Platform/<Backend>/` includes from the sweep bucket **by hand**, on the
same schedule as the PCH. Note the *factory* files (`Texture.cpp`, `Shader.cpp`,
`VertexBuffer.cpp`, …) legitimately include their backend counterparts to
construct one in `Create()` — those are the pattern working as intended, not
leaks, and Phase 4 adds a Vulkan branch beside them.

### Expect to ADD a few includes, and do not read that as a regression

ADR 0011 §0 measured that 3 of the 42 GL-calling files had no direct
`#include <glad/gl.h>` and were relying on `RendererAPI.h`'s transitive one.
Closing that transitive path makes exactly those files fail to compile, and the
correct fix is to give them a direct include — they really do call GL, and they
stay on the step-2 sweep backlog. `sweep_glad_includes` therefore does not fall
monotonically during Phase 2: it drops by the number of zero-call includers
removed and rises by ~3. That is the counter becoming *honest*, not a
regression, and it is worth predicting so nobody "fixes" it by re-hiding those
files behind a transitive include.

---

## 2b. The facade was not just GL-typed, it was **incomplete** — and that is the bigger number

Step 1 rewrote the vocabulary of `RendererAPI`'s 74 existing virtuals. Step 2
then discovered that stripping `GLenum` was the smaller half of the job:

> **84 distinct GL entry points** appear across the 313 swept call sites, and
> **54 of them had no `RendererAPI` equivalent at all.** Closing that gap took
> **60 new virtuals** — nearly doubling a 74-virtual facade.

Keep those two numbers distinct rather than folding them into one percentage: an
entry point can expand into more than one virtual (`glClearTexImage` becomes a
float clear and a uint clear; the readbacks each gained a `bool` return). 54 is
the size of the gap, 60 is the size of the fix.

Whole categories had simply never been abstracted, so every pass reached past the
facade to perform them: buffer binding points (`glBindBufferBase`, 26 sites),
raw buffer lifecycle (the virtual-geometry arena + persistent-mapped upload
ring), named-framebuffer state (draw/read attachment selection, clears, blits,
attachment, completeness), occlusion and timer queries, fences, VAO lifecycle,
texture clear/readback, debug markers.

**The lesson, and it generalises past this phase:** an abstraction's
completeness is not measured by how many call sites it already serves, but by
how many distinct *operations* the layer above performs. 74 virtuals looked like
a thorough facade while 60 operations went around it — because each of those was
rare enough (1–3 sites) to read as a special case. Frequency was not the signal
either: `glBindBufferBase` had 26 sites and was still missing.

Practical consequence for a future phase: before starting a sweep, histogram the
**distinct entry points**, not the call count. The call count tells you how much
typing you face; the entry-point histogram tells you how much *designing* you
face, and that is the part that cannot be delegated or hurried.

### Read the previous phase's declaration-only header for VOCABULARY, not just for types

The sweep needed to tell `AllocateBufferStorage` how a buffer's memory is used,
and began inventing `RHI::BufferUsage { DynamicDraw, DynamicCopy, DynamicRead }`
— a straight transcription of GL's `glNamedBufferData` hints, which is exactly
the mistake this whole phase exists to stop.

Phase 1 had **already designed that concept**, and better: `RHI::MemoryResidency
{ DeviceLocal, HostToDevice, DeviceToHost }`, named by intent rather than by GL's
spelling, three members mapping one-to-one onto the need. It was invisible
because it sat beside `BufferDesc` in `RHIResources.h` — a header nothing
consumed yet — while `RendererAPI.h` includes only `RHITypes.h`.

**What caught it was a name collision, not review.** `RHIResources.h` also had a
`BufferUsage` (the *bind-flags* enum: `Vertex`/`Index`/`Storage`/…), so the two
could not coexist. And the collision only fired in the **test** build: the engine
library compiles without ever including `RHIResources.h`; only
`RHIBoundaryRatchetTest` includes it, precisely so the declaration-only
vocabulary keeps compiling. Rename either enum and the duplicate concept ships
silently.

Method, for any phase that inherits a declaration-only header:

1. Before adding a type to the shared vocabulary header, grep the whole
   `Renderer/RHI/` directory for the **concept**, not the name you picked.
2. Treat "this header is declaration-only, nothing consumes it" as a reason to
   read it *more* carefully, not less — unconsumed means uncorrected, so it holds
   the design intent at its cleanest and its most easily missed.
3. If the concept exists but lives in the wrong header for your consumer, **move
   it** rather than duplicating it. `MemoryResidency` moved to `RHITypes.h`; it
   was vocabulary all along, filed under resource description.

### Behaviour deltas a sweep introduces even when it changes no logic

Three showed up here. None is a bug, all three are visible, and a reviewer
should know to expect them:

- **The backend's own state cache becomes truthful.** `VirtualGeometryPass` set
  depth state with raw `glEnable(GL_DEPTH_TEST)`, which left
  `OpenGLRendererAPI::m_DepthTestEnabled` stale; `Clear()` derives its
  `GLbitfield` from that member. Routing the pass through `SetDepthTest(true)`
  fixes the divergence — which means a later `Clear()` can now clear depth where
  it previously did not. Verify visually rather than reasoning about it.
- **Profiler counters move.** The facade's state setters bump
  `RendererProfiler::StateChanges`; the raw calls they replaced did not. Draw
  counters deliberately did *not* move: the new `DrawBound*` family does not
  touch `RendererProfiler`, because the call sites it replaced never did and
  several keep their own `CommandDispatch::Statistics`.
- **The mock gets safer, and records more.** Call sites that used to issue raw
  `glXxx()` under a `MockRendererAPI` were calling a null glad function pointer
  in a headless test. They now land on the mock. Tests asserting *exact* recorded
  call counts will need updating; ones using `HasCall` / `GE` will not.

---

## 3. `Renderer/Debug/` is 43% of the problem and is *not* exempt

236 of the 549 calls are in 7 files under `Renderer/Debug/`: `GLStateGuard`,
`GPUResourceInspector`, `RenderGraphFrameCapture`, `RenderGraphPassSnapshot`,
`GPUPassTimerPool`, `GPUTimerQueryPool`, `RendererValidate`.

The tempting call is "a debug inspector is legitimately GL-specific, exempt it."
That is wrong, and the reason is `CLAUDE.md`'s own rule: *"Do not report a
rendering change as done on the strength of unit tests alone."* That rule is
**enforced through these exact tools** and the MCP endpoints they back
(`olo_render_capture_target`, `olo_render_transient_plan`, `olo_screenshot`). A
Vulkan backend they cannot see is a Vulkan backend Phase 7 cannot verify — which
is what #691 is already warning about.

So they are a third category: **relocate, don't exempt.** Neutral interface stays
in `Renderer/Debug/`, GL implementation moves under `Platform/OpenGL/`. They get
their own ratchet counter so the progress is visible without blocking Phase 2's
headline number from reaching zero.

One member deserves a different treatment: **`GLStateGuard` should be a no-op on
Vulkan, not a port.** Both
[gl-clear-program-revalidation.md](gl-clear-program-revalidation.md) and
[render-pass-published-state.md](render-pass-published-state.md) exist because
of OpenGL *global state* hazards — a driver revalidating the bound program at
`glClear`, and a blanket `GLStateGuard(Restore)` silently reverting
engine-global bindings. Vulkan has no equivalent global state to guard. The
`RendererAttachedTest` fixture's `GLStateGuard(Restore)` wrapper therefore needs
a backend conditional, not a translation.

---

## 4. One `u32` is doing three jobs — do not replace it with one new type

`u32 GetRendererID()` and its aliases (`using RendererID = u32` in
`Commands/RenderCommand.h`, `TransientPool::AcquiredInfo::RendererID`,
`RGCommandContext::ResolveTexture() -> u32`) simultaneously mean:

1. **identity** — which GPU object is this (pool aliasing, `operator==`, debug labels),
2. **binding address** — via the texture unit it gets bound to, and
3. **native handle** — the thing you pass to a driver entry point.

Vulkan separates all three, and `VK_EXT_descriptor_heap` separates them further:
what the shader indexes is a heap offset for one *view*, whose lifetime is not
the image's lifetime.

**The Phase 2 failure mode to avoid is a rename** — swapping `u32 RendererID`
for one opaque `RHIHandle` keeps the conflation and buys nothing, and Phase 5
would sweep again. ADR 0011 splits it into `RHI::ResourceHandle` (identity,
`{Index, Generation}`), `RHI::HeapOffset` (a shader-visible `u32`), and a
backend-private native handle reachable only through a deliberately conspicuous
`GetNativeHandleForDebug`.

Two details worth keeping:

- **The generation is load-bearing.** GL recycles object names, so two genuinely
  different objects *could* compare equal through `Texture::operator==` when one
  was deleted and another created. `TransientPool`'s alias reporting — the
  tooling built in #607 specifically to answer "did these two plan entries get
  the same object?" — depends on telling those apart. Step 3 closed this: the
  operator compares `GetRHIHandle()`, so the generation now makes the collision
  unrepresentable rather than merely unlikely. Note the fix had to be made in
  the *operator*; minting handles everywhere did not fix it on its own, because
  a comparison keeps reading whatever currency it names.
- **`HeapOffset` must stay layout-compatible with `u32`.** It gets written into
  a UBO and read by GLSL as an array index; it cannot be opaque.

### Step 3 part 1 built the mint; the sweep onto it is part 2 — read this before starting it

`RHI::ResourceRegistry` mints `RHI::ResourceHandle` and is proven by
`RHIResourceRegistryTest`. `ViewHandle` and `HeapOffset` are deliberately
deferred to Phase 3 as a matched pair (ADR 0011 amendment (11) — a `ViewHandle`
with no heap behind it has one view per resource and therefore detects nothing,
and deferring adds *two* call sites to Phase 3, not a second sweep, because the
~230 bind sites are already Phase 3's to delete).

**A full-sweep attempt was abandoned rather than finished; conversion is now
proceeding slice by slice instead.** The counters say how far there is to go —
read the live values from `rhi_boundary_baseline.json`, not from this paragraph
(at the time of writing `sweep_renderer_id` is 699, `facade_native_id_params`
68). Everything below is what that attempt learned — treat it as a checklist,
not history.

**Do not use `sweep_renderer_id` as a progress meter.** It matches the accessor
*name* `RendererID`, not the currency. The first fully-migrated resource (SSAO's
noise texture) was five native-id call sites and zero `RendererID` spellings, so
migrating it end-to-end moved the counter by 0. It is a regression ratchet;
`facade_native_id_params` reaching 0 is the completion criterion.

**Do `facade_native_id_params` first.** Same ordering logic §1.7 gives for
stripping the `GLenum`s before counting includes: while `RendererAPI` still
accepts a `u32 textureID`, every caller can keep producing one and nothing is
enforced. Once the facade takes handles, a TU holding a native name *cannot*
call it, and the rest of the sweep becomes compiler-driven instead of
grep-driven.

- **The three-way split does not mean three independently-schedulable sweeps.**
  §1.3 assigns the bind sites to Phase 3, which reads like they can be left
  alone. They cannot be left on `u32`: `ResolveTexture` (Phase 2's, by name) is
  the *producer* and `BindTexture` the *consumer* of one dataflow, so the
  facade's parameter has to move with it or every pass resolves a handle back to
  a native name. The ~230 call sites stay textually identical — only the type
  flowing through them changes, and Phase 3 still deletes the call from the same
  sites with the same operand. **Scope a currency change by dataflow, not by the
  phase table.**

- **A `.data()` boundary is where the type system stops checking.** Everywhere
  else, re-typing a facade virtual breaks every caller at once — the compiler is
  an exhaustive checker, the same "provable rather than measured" property that
  makes `sweep_glad_includes` worth more than a call count. But
  `CreateQueries(std::span<u32>)` passed `.data()` to `glCreateQueries`;
  re-typing the span to `std::span<RHI::ResourceHandle>` **still compiles**, and
  GL would write 4-byte names over an array of 8-byte handles. Silent, no test
  failure. Grep for `.data()` on any container whose element type you change.

- **`Delete*` needs two changes, and only one is obvious.** Resolving the handle
  to call `glDelete*` is the visible half. Unregistering it is the half that
  matters: skip it and the slot keeps its generation, so a handle to a destroyed
  object goes on resolving to a name the driver may reissue — the recycled-name
  bug reintroduced at the one place most likely to be treated as mechanical. A
  scripted conversion will do the first and not the second.

- **A bulk identifier rename needs a collision check against the *target* name
  before it runs.** `shaderRendererID -> shaderHandle` collided with a
  pre-existing `AssetHandle shaderHandle` in three POD command structs. That was
  loud (`C2371`) because they shared a struct — but in the ~120 files where the
  two names did *not* co-occur, the merge would have been silent and two
  genuinely different concepts (GPU program vs asset id) would have become one.
  Recovering the split needed a difflib alignment against `HEAD` plus a per-file
  count check; five files failed that check and needed a content-based rule.

### The compiler tells you two types disagree; it never tells you which side is wrong

This is the single most useful thing to know before automating a currency sweep.
`cannot convert argument 2 from 'RHI::ResourceHandle' to 'u32'` has **two** valid
repairs — re-type the callee, or convert at the caller — and tooling that always
picks one is wrong a predictable fraction of the time. Automating "re-type the
callee" was right for roughly 90% of sites and wrong for these, each of which
genuinely wants a small native integer:

- **`DrawKey::SetShaderID`** packs an identity into a 64-bit sort-key *bit
  field*. A handle cannot be shifted, so the caller passes `handle.Index`. This
  one failed loudly (`SHADER_MASK << SHADER_SHIFT` on a struct) — but only
  because the callee *computes* with the value. A callee that merely stored it
  would have accepted the change and looked fine.
- **`GPUResourceInspector`** makes raw GL calls on the id it stores
  (`glBindBuffer`, `glGetTextureSubImage`). It is the tools bucket (§1.6), all
  its callers are `Platform/OpenGL/` passing their own native names, and it
  legitimately holds one until Phase 8 relocates it.

**Rule of thumb:** before re-typing a callee, check what it *does* with the
value. Arithmetic, bit-packing, or a backend call means the callee is right and
the caller must convert (`handle.Index` for a bucketing key,
`Debug::NativeOf(handle)` inside `Renderer/Debug/`). Only a callee that purely
*carries* the value should be re-typed.

Two of those decisions turned out to be improvements rather than neutral:
`InstanceGroupKey` now keys on the handle (two live handles cannot collide,
where a delete/create pair can hand two objects the same GL name), and the
radix-sort sites use `handle.Index`, which is dense from zero where a
driver-assigned name is arbitrary.

### Three scripted-edit defects, and what caught each

Recorded because the pattern is consistent and it decides where review effort
should go. **Type errors were caught by the compiler; semantic choices were not
— every one of these was found by reading output.**

| Defect | Blast radius | Caught by |
| --- | --- | --- |
| Unanchored filename regex: `Sort.h` matched `IntroSort.h` | 15 legitimate includes deleted across 9 files with nothing to do with the task, including `Scene/Components.h` | reading the diff |
| `X.field == 0` → `X.!field.IsValid()` — negation inside the member access | 6 sites | syntax error, but the same rewrite on a form that *parses* would have inverted a render-pass guard silently |
| Always re-typing the callee (above) | 2 confirmed | one loud, one by reading the code |

The middle row is the one to take seriously: an inverted boolean guard in a
render pass is exactly the "tests green, screen wrong" class `CLAUDE.md`'s
rendering rule exists for. **When reviewing a scripted currency sweep, read the
conditionals first** — the type changes are compiler-checked, the guards are not.

### A GLOBAL rename is always wrong here, and slice 6 proved it five times

The defining property of a dual-currency migration is that **some call sites must
not move**. A regex cannot see which, so every broad tool overreaches. Slice 6
hit this five separate times; recording the shapes because four of the five were
caught only by luck of the destination type differing:

| The tool | What it did | How it surfaced |
| --- | --- | --- |
| `grep … \| head -30` to survey `RendererID` uses | Read a TRUNCATED list as complete, missed `Renderer3D.h`'s 38 | 4551-error parse cascade |
| `s/(=\s*)0(\s*[;,])/\1{}\2/` | Matched the `=` of `!=`, making `x != 0;` into `x != {};` | Failed to parse — but the same rewrite on `x != 0 &&` would have parsed |
| `s/->GetRendererID()/->GetRHIHandle()/` over `Scene.cpp` | Swept up the IBL trio that must stay native for the graph import | Error count went UP, 67 → 75 |
| the same, second pass | Swept up the cloudscape weather map and three fluid SSBO ids (later slices) | Compiler, because the destinations were still `u32` |
| `s/field = <literal>/field = TestHandle(<literal>)/` in tests | Wrapped a literal `0` that meant "absent", producing a VALID handle naming slot 0 | **The test suite** — `DrawWaterCommandZeroInitNoNaN`. Nothing else would have. |

The last row is the dangerous one and the reason to write this down: it is the
only one the compiler could not see, and had the default handle happened to be
`{0,1}` instead of `{0xFFFFFFFF,0}` it would have passed while silently
weakening a zero-init assertion. **In a test, a literal `0` on a migrated field
is the ABSENT sentinel (`RHI::NullResource` / `!IsValid()`), never a synthetic
id** — `TestHandle(0)` is a live handle naming registry slot 0.

The tool that *did* work: drive edits from the compiler's own `(file, line)`
output and only rewrite lines it rejected. That cannot touch a site the compiler
accepted, which is exactly the set that must not move.

### Identity is the C++ object, not the native name — and that fixes something

Anchoring the registry entry to the resource *object* (with `UpdateNative` for
recreated storage) means an in-place hot-reload keeps the handle valid.
`TextureInPlaceReloadTest`'s header used to warn that consumers "must read the
RendererID off the object each frame rather than caching it", because GL may
hand recreated storage a different name. Caching a handle is now safe. Two
*different* objects still never compare equal, which is the `Texture::operator==`
defect the generation exists for.

Also: a framebuffer's colour/depth **attachments** are separate GL objects that
the engine samples through `ResolveTexture`, so they need handles of their own.
"The framebuffer's handle" is the wrong answer to "which texture is this".

### The migration grain is a RESOURCE, not a layer — and migrating one is the only way to find the missing facade surface

The first real call-site migration (SSAO's 4×4 noise texture) had to move its
whole chain in one commit — create, configure, import, bind, delete — because
`native -> handle` is unrecoverable, so a half-migrated chain has a step that
cannot get back to the currency the next step wants.

Doing it that way immediately turned up **three facade entry points that a
breadth-first read of `RendererAPI` had missed**: `SetTextureFilter`,
`SetTextureWrap`, `UploadTextureSubImage2D`. They were missed because the survey
was organised around the *bind* family and the *create/delete* family, and
texture configuration is neither. There is no reading of the facade that
reliably enumerates what a migration needs; **migrate one real resource and let
the compiler enumerate it for you.** Budget for it: the first pass cost 4 files
per missing entry point (`RendererAPI`, the backend, `RenderCommand`, the mock).

Related: **a sibling added at the wrong layer is invisible until a caller needs
it.** `ImportTextureHandle` was added to `RenderGraph`, but passes reach the
graph through `RGBuilder`, which had no forwarder — the pass could not call the
function written for it. Same for `RGCommandContext::BindTexture`. Add a sibling
at every layer the intended caller actually traverses, and confirm by naming the
call site before writing it.

### Picking up step 3: the measured worklist, in dependency order

Do not re-derive this. It cost three wrong scoping guesses to produce, and the
distribution is the part that matters — the unit of work must intersect what you
are counting, which twice it did not.

Reproduce with `python OloEngine/tests/scripts/measure_rendererid.py`
(`\w*RendererID\w*` over `OloEngine/src`; the ratchet's `sweep_renderer_id` is
the same thing after stripping comments, strings and the exempt backend). The
script derives the repo root from its own location — the first version pinned an
absolute worktree path and reported a confident `TOTAL 0 across 0 files`
everywhere else, so **if it prints 0, check that before believing it.**

Counts are *after slice 6*. The raw total went 1196 across 118 files → 1150
(slice 5) → **848 across 104** (slice 6):

| Where | Then | Now | Note |
| --- | ---: | ---: | --- |
| `Platform/OpenGL/` | 412 | ~410 | **Exempt.** The backend may name GL ids. |
| `Renderer/Commands/` | 141 | **53** | Slice 6. What survives is `CommandDispatch.cpp`'s remaining native spellings. |
| `Renderer3DMeshSubmission.cpp` | 71 | **~0** | Slice 6, via the POD structs. |
| `Scene/Scene.cpp` | 66 | **~0** | Pulled in by slice 6 (it fills the PODs). |
| `Renderer3D.h` | 62 | **~0** | Pulled in by the alias deletion — item 3's header, landed early. |
| `Get{Color,Depth}AttachmentRendererID` call sites | 72 | **29** | Slice 5 + the two Renderer3D setters slice 6 unblocked. |

By spelling: `m_RendererID` 435 (backend-internal, exempt), `GetRendererID`
325 → **212** (**the real target**), `shaderRendererID` 110 → **111** (unchanged
— it is a FIELD NAME on the migrated structs, and renaming the field is
cosmetic churn better done with item 4's deletion pass), attachment getters
71 → **29**.

Suggested order, each a buildable commit:

1. ~~**Attachment consumers**~~ — **DONE (slice 5)** for every site whose sink
   was reachable; see "What slice 5 actually moved" below for the eight that
   were not, and why leaving them is the correct call rather than a shortfall.
   One prediction in this list was wrong and is worth keeping: *"no new facade
   surface needed"*. Six new virtuals were needed
   (`CopyImageSubData` / `CopyImageSubDataFull` / `ClearTextureFloat` /
   `ReadTextureImage` / `ReadTextureSubImage` / offset-`UploadTextureSubImage2D`),
   because the survey behind this table counted only *bind* sinks. The
   attachment getters also feed a **copy** family (the bakers stage an
   attachment into a persistent `Texture2D`/cubemap) and a **readback** family
   (thumbnail / light-probe / reflection-probe capture). Same lesson as §4's
   `SetTextureFilter`/`SetTextureWrap`/`UploadTextureSubImage2D` discovery, one
   slice later: **you cannot enumerate a migration's facade needs by reading;
   migrate one real consumer per SINK FAMILY and let the compiler tell you.**
2. ~~**The command-layer bind cache**~~ — **DONE (slice 6)**, and it was
   materially bigger than this line implies. Three corrections for whoever
   scopes a comparable unit:

   * **The blast radius was 253 errors across 16 files, not "~180 concentrated
     in `CommandDispatch.cpp`"** — that file was ~40% of it. The rest came from
     deleting `using RendererID = u32`, which is a TYPE in headers included
     everywhere: `Renderer3D.h` alone used it in 38 declarations and produced a
     **4551-error parse cascade** on the first build. Item 3's `Renderer3D.h`
     therefore lands with item 2 whether you planned it or not.
   * **Seven resource chains not named in this worklist came with it**, because
     each feeds the cache and `native -> handle` is unrecoverable:
     `CloudShadowMap`, `SnowAccumulationSystem`, `OceanFFTField`,
     `FoliageRenderer`, `DepthPrepassShaderIDs`, the global IBL maps, and
     `ShadowMap`'s compare-off views.
   * **Nine new facade virtuals were needed**, against a prediction of zero:
     `CreateDepthArrayCompareOffViewHandle`, `SetProgramUniformFloat`, handle
     forms of `DrawIndexedRaw` (×2), `DrawIndexedInstancedRaw`,
     `DrawIndexedPatchesRaw`, and `DrawBoundElementsIndirect`.
3. ~~`Scene.cpp` / `Renderer3D.h`, then the remaining passes~~ — **DONE
   (slice 7)**.
4. ~~**Last:** delete `GetRendererID()` and the u32 facade forms~~ — **DONE
   (item 4). `facade_native_id_params` is 0 and STEP 3 IS COMPLETE.** Three
   corrections to this line, all worth keeping:

   * **It did not fall "per entry point".** The plan assumed each `u32` form
     could go as its last caller did. In practice the LAST caller of almost
     every form was another `u32` form's caller, so the whole facade had to
     flip in one edit and the ~250 call sites were then repaired against the
     compiler. That is the same shape as item 2, one level larger, and the
     counter went 67 → 0 in a single step rather than draining gradually.
   * **`GetRendererID()` itself is NOT deleted, and should not have been on
     this list.** The accessor is what `Platform/` and `Renderer/Debug/` are
     *supposed* to use, and the graph's diagnostics path reads it deliberately.
     What item 4 actually removes is every way for the SWEEP BUCKET to reach
     one: the facade takes identities, so a translation unit holding a native
     name has nothing to pass it to. Deleting the accessor is Phase 8's job,
     with the tools relocation.
   * **The creators keep their `...Handle` suffix.** Slice 4 called it
     temporary and expected item 4 to take the plain name back. On arriving,
     renaming ~40 call sites bought nothing semantic and
     `CreateTexture2DHandle` reads correctly as "create a texture, get its
     identity". Revisit in Phase 3, where `ViewHandle` reshapes the create
     family anyway.

**BOTH "producer gaps" this list named were wrong**, and in opposite ways:

* `VertexBuffer` / `IndexBuffer` "expose no `GetRHIHandle()`" — true, and
  irrelevant. Their accessor is `GetBufferHandle()`, which is used *only*
  inside `Platform/OpenGL/`; the thing `Renderer3DMeshSubmission` and
  `VirtualMeshRegistry` actually hold is a `Ref<StorageBuffer>`, which has
  minted handles since slice 2. No producer needed adding. **Grep for the
  accessor the CALLER uses, not the one the type family suggests.**
* `ShadowMap`'s handle siblings "block item 2" — they had already landed *in*
  item 2, so by item 4 the native accessors were the leftovers, not the gap.

### `ImportTextureHandle` BLINDS `ResolveTexture`, and `ResolveTexture` is what the MCP capture endpoints read

**Read this before migrating any `builder.ImportTexture` call.** It is the one
finding from slice 5 that changes how later slices must be scoped, and it is
already live in `master`.

`ImportTextureCommon` treats the native id and the identity as **alternatives,
never both** — `ImportTexture(name, id, desc)` passes `identity = {}` and
`ImportTextureHandle(name, handle, desc)` passes `textureID = 0u`. That
invariant is deliberate and correct (it is what makes `AllocateTextureHandle`'s
change detection honest — see the section below on stamping identity on
afterwards). The consequence is not:

- `RenderGraph::ResolveTexture` ends at `m_PhysicalTextures[i].TextureID`, so it
  returns **0** for a handle-imported resource;
- `Renderer3D::ResolveFrameGraphTexture` forwards to it, and
- `McpToolsRender.cpp` resolves *every* reported id through those two
  (`olo_render_list_targets`' `GLTextureId`, `olo_render_validate`'s identity
  table, and `ResolveTargetTexture`, which backs `olo_render_capture_target`).

So **migrating a resource's import silently removes it from the diagnostics**.
It does not fail, warn, or look different from a resource that genuinely has no
backing — the capture just reports id 0. #732 already did this to SSAO's noise
texture, which is why `olo_render_capture_target SSAONoise` cannot work today.
That matters more than one broken probe: CLAUDE.md's rendering-verification rule
is *enforced through these endpoints*, so a slice that quietly blinds them
removes the check on itself.

The fix is a fallback, not a new resolver: try the native id, and when it is 0
ask the identity and go through `RHI::GetNativeHandleForDebug` — the hatch
documented in `RHIResources.h` for exactly "the introspection tools in
`Renderer/Debug/` and the MCP capture endpoints they back". It lives in
`Renderer/Debug/RenderGraphResourceIdentity.{h,cpp}` as
`Debug::NativeTextureIdForDiagnostics`.

**Where it lives is the load-bearing part, and it was wrong first.** The
obvious home is the caller — `OloEditor/src/MCP/`, which `RHIBoundaryRatchetTest`
does not scan, so `debug_escape_hatch` stays 0 for free. That is what this fix
did initially and it is a trap: `OloEngine-Tests` does not link `OloEditor`, so
the *composition* had no test — only its individual legs did. That is precisely
the configuration that let the original defect through, so "fixing" it there
re-arms the same trap one layer out. `Renderer/Debug/` satisfies both
constraints at once: it is a sanctioned home for the hatch **and** it is inside
the engine library, so `RenderGraph.Diagnostics*` can pin it.

Do **not** make it a `RenderGraph` member. That puts the hatch inside
`Renderer/`, where `backend_resolve_hatch` bans it at 0 — moving that boundary
is a decision on its own merits, not a side effect of a bug fix.

**Sequencing rule this gives you:** a resource's import may only move to
`ImportTextureHandle` *after* the diagnostics can read a handle-imported
resource. Slice 5 therefore migrated DDGI's atlas *consumers* while leaving
`importAtlas` on the native id, and left `m_ProbeDataTexture` native entirely —
two complete chains on two currencies, which is fine, rather than one chain that
compiles and blinds a tool.

### What slice 5 actually moved, and the eight sites it deliberately did not

Migrated: all seven bakers (`ThumbnailCapture`, `LightProbeBaker`,
`ReflectionProbeBaker`, `IBLPrecompute`, `ImpostorBaker`, `SkyCubemapBake`,
`AssetPreviewRenderer`), the straightforward bind passes (`Fog`, `Overdraw`,
`SelectionOutline`, `Cloudscape`'s composite, `Decal`, `Bloom`), all of
`DDGIProbeUpdatePass`'s attachment reads including the `SetAtlasTextureParams`
signature, and `RenderGraph`'s attachment clear + NaN-census readback.

Deferred at the time, each for a stated reason rather than for size — **and
five of the six were cleared by slices 6 and 7**, which is itself the lesson:

| Site | Why it was deferred | Outcome |
| --- | --- | --- |
| `SSAO` blur→AO copy, `SceneRenderPass`'s three exports, `GPUDrivenOcclusion`'s two | "the other operand is a **transient**, and a transient has only a native id" | **WRONG — cleared in slice 7.** See below. |
| `Cloudscape`'s raymarch source + history | native history id + a transient resolve | **Cleared in slice 7** |
| `Renderer3DFrameExecution`'s HZB depth, `PlanarReflection`, `Water` | feed `Renderer3D::` setters | **Water + PlanarReflection cleared in slice 6** when those setters migrated; **the HZB one cleared in item 4** (`HZBGenerator` and `HZBOcclusionInputs` are identity-typed) |
| `RenderGraph`'s three external-sink copies | the sink's `TextureID` is registered from outside the graph as a raw `u32` | **Cleared in item 4** — every registrant holds a `Ref<Texture2D>`, so the sink takes the identity and the copy compares objects |
| `RenderGraph`'s JSON topology dump | reports native ids on purpose, for external tooling | stays native |
| `Renderer/Debug/`'s two | Phase 8 relocation | stays |

### The "blocked on the transient pool" claim was wrong, and the shape of the error is worth keeping

It was recorded as fact in the worklist AND posted to #691 before anyone
measured it. `TransientPool::AcquireTexture` returns a **`Ref<Texture2D>`, which
has minted handles since slice 2**; `AcquiredInfo::RendererID` is a
diagnostics-report field with no role in resolution. The real constraint was one
line in the planner that simply never set `.Handle`, and behind it a **design
invariant, not a missing producer**: `PhysicalTexture` documented `TextureID`
and `Handle` as "ALTERNATIVES… exactly one is set per entry".

That rule is right for an **import** — an importer only ever *has* one currency,
and neither is derivable from the other. It was never true of a **transient**:
the planner holds the pooled `Ref` itself, so it has both in hand and reads them
off one pointer in one statement. Nothing is derived, so nothing can drift.
Setting both is what unblocked all eight sites.

**Generalisable:** when a migration says "blocked on X", check whether X is a
missing *capability* or an *invariant someone wrote down*. A missing capability
is work. An invariant is a decision, and decisions can be revisited once you
know which case they were written for. Recording the blocker without checking
which kind it was cost this issue a whole slice of imagined work — and put a
false statement on the tracker.

Bonus from doing it: every one of those sites guards its copy with
`if (src != dst)`. Those now compare OBJECTS, so a recycled driver name can no
longer make a source and its export look identical and skip a copy the frame
needed — the `InstanceGroupKey` defect class, in four more places.

**Counters after slice 5:** `sweep_renderer_id` 699 → **653**;
`facade_native_id_params` unchanged at 68, because that slice *adds* handle
overloads and deletes no `u32` form — item 4's job, and the documented order.
The 46 flatters it: the accessor name survives wherever a native sibling is kept
on purpose (DDGI's `GetIrradianceAtlasID` is still there so `importAtlas` can
call it), and 40 of the 46 are the attachment getters themselves.

**Counters after slice 6 (the bind cache):** `sweep_renderer_id` 653 → **355**,
`facade_native_id_params` 68 → **67**. The 298 is the honest measure of what
converting the CURRENCY (rather than one producer family) is worth — six times
slice 5's move.

The `facade_native_id_params` fall is small but worth reading, because the naïve
version of this slice *raised* it. `DrawElementsIndirectRaw(vaoID, bufferID)`
needed a handle form, and the obvious answer was a mixed
`(RHI::ResourceHandle, u32 indirectBufferID)` overload — which adds a
`u32 <name>ID` parameter and pushes a ratchet that may only fall to 69. The real
answer was that its single caller had *already* run `BindVAOIfNeeded`, so the
draw was re-binding the VAO behind the redundant-bind cache's back; replacing
both `u32` forms with `DrawBoundElementsIndirect(u32)` (matching the existing
`DrawBound*` family) removed a redundant bind AND netted −1. **When a migration
looks like it must raise a ratchet, that is usually the signal that the call
site's shape is wrong, not that the ratchet is.**

Read §4's "do not use it as a progress meter" note before reading either number
as a completion fraction.

### Hashing a driver name into a cache fingerprint cannot see a destroy-then-recreate

Found while migrating DDGI, and general: `RenderPipeline::ComputeBlackboardFingerprint`
hashed `GetIrradianceAtlasID(ping)` so that recreating the atlases would rebuild
the frame graph and re-import them. But `DDGIProbeUpdatePass::EnsureResources`
calls `DestroyResources()` **before** creating the replacements, so every atlas
texture is freed first and GL is then free to reissue the same names — under
which the fingerprint does not change at all. The rebuild never happens and the
graph keeps an import whose `Width`/`Height` still describe the *old* resolution,
which is exactly what `olo_render_list_targets` then reports.

Hashing `RHI::HashKey(handle)` fixes it, because a generation cannot be
reissued. **Any cache keyed on "did this GPU object change" has this bug if it
keys on the driver name and the owner frees before it allocates** — and note the
opposite ordering (allocate-then-release, as `m_IrradianceFB[i] = makeAtlasFB(…)`
would be on its own) hides it completely, so whether the bug is live depends on
a line of teardown code nowhere near the hash.

### The command layer's bind cache is ONE unit, and its GL-name keying has already shipped a bug

Scoping note for whoever migrates `Renderer/Commands/`. It looks like several
independent migrations (material textures, shadow maps, UBOs, VAO/shader
binding) and it is exactly one, because they share
`CommandDispatchData::BoundTextureIDs` — the redundant-bind cache. Migrating
`PODMaterialData`'s texture ids alone does not compile past its own file: those
textures bind through `BindTrackedTexture`, which keys the same array that
`BindTrackedTextureUnit` uses for CSM / shadow atlas / their raw-depth views /
snow depth / cloud shadow. So the unit is: that array, `BoundUBOIDs`,
`CurrentBoundShaderID`, `CurrentBoundVAO`, `DepthPrepassShaderIDs`, the six
per-frame shadow id fields, **and** handle-returning siblings on `ShadowMap`
(`GetCSMRendererID` / `GetAtlasRendererID` and the raw + placeholder variants).
Every producer involved can already mint (`Texture2DArray`, `UniformBuffer`,
`Shader`, `VertexArray` all expose `GetRHIHandle()`), so it is unblocked — it is
just indivisible. Expect ~180 compile errors from the field-type change alone,
concentrated in `CommandDispatch.cpp`, with `Renderer3DUtilityDraws.cpp`,
`Renderer3DSpecializedDraws.cpp`, `CommandPacketDebugger.cpp` and
`CommandBucket.cpp` following.

Do NOT start it as "migrate PODMaterialData" and discover the rest downstream;
that is the same mistake as picking a slice by what looks self-contained in one
file.

The reason it is worth doing rather than deferring: **this cache has already
caused a real, debugged visual bug of exactly the kind the identity currency
prevents.** The comments on `InvalidateTextureSlot` / `InvalidateTextureBinding`
record it — `VirtualGeometryPass` binds the Hi-Z pyramid to unit 0 for its cull
compute, unit 0 is also `u_AlbedoMap`, and "any material whose albedo ID matched
the stale cache entry silently sampled the HZB depth texture as its albedo."
Keyed on identities that particular collision becomes unrepresentable: a deleted
texture's handle is retired, so it can never compare equal to a live one.

### …but invalidation gets MORE load-bearing, not less — the one place handles are WEAKER

**An earlier version of this section said the invalidation calls "stop being
load-bearing correctness and become a pure optimisation". That was wrong, and
wrong in the direction that ships bugs.** Recorded here because the reasoning is
seductive and the failure is silent.

The recycled-name hazard does die. But there is a second hazard the identity
currency *creates*, because identity is deliberately stable where the driver
name is not: an **in-place reload** (`OpenGLTexture::InvalidateImpl`) deletes the
GL texture, creates new storage, and calls `ScopedResourceHandle::Sync`, which
**preserves** the handle — that is the whole point of §4's "identity is the C++
object", and it is what makes caching a handle safe. So:

- the cache holds handle `H` for slot N, GL name `old` is bound to unit N;
- reload: `old` is deleted, `new` created, `H` still names the object;
- `BindTrackedTexture(H, N)` sees `BoundTextures[N] == H`, concludes "already
  bound", and **skips a bind that must happen** — leaving the unit pointing at a
  deleted name.

Under the old native-id keying this self-corrected, because the name changed and
the cache missed. Under handle keying it does not. So the rule inverts:
**every site that recreates a texture's storage MUST call
`InvalidateTextureBinding`** (it takes a handle now). Deleting those calls as
"no longer needed" — which the old paragraph invited — would produce a
tests-green / screen-wrong bug visible only after a hot reload.

`InvalidateTextureSlot` is also still required, for an unrelated reason: a raw
binder bypasses the cache entirely, so the cache's claim about the slot is simply
untrue and no keying scheme can detect that from the inside.

### Delegating to the native path and stamping the identity on afterwards silently disables generation bumping

`RenderGraph::ImportTextureHandle` was first written to reuse the native
importer — `ImportTexture(name, 0u, desc)` — and then assign `phys.Handle`
afterwards, with a comment explaining that forking the import logic would be a
second place to fix. The reasoning was right and the implementation still broke
the core invariant, because `AllocateTextureHandle` decides whether to retire
prior handles with

```cpp
const bool resourceChanged = (phys.TextureID != textureID) || (phys.IsHistory != isHistory);
```

which for a handle import compares `0 != 0` on every call. Re-importing a name
with a *different* texture kept the slot generation, so every cached
`RGTextureHandle` stayed valid and resolved to the slot's **new** occupant —
the recycled-name hazard the phase exists to eliminate, recreated inside the fix
for it.

The rule: **when a resource's identity is passed to a routine as an
afterthought, it is not available to that routine's invalidation logic.** Thread
it in as a parameter (defaulted, so the native path still compiles) and let one
change test cover both currencies; the native path then also *clears* a stale
identity for free, keeping "alternatives, never both" true without a second
assignment site.

Why no test caught it: the first migrated pass creates its texture once at init
and never recreates it, so the bug is inert exactly where it was introduced. A
pass that recreates on resize would be the first to hit it, and it would look
like a stale frame rather than a crash. **A migration's first subject is
usually its least demanding one** — do not treat "the migrated pass works" as
evidence that the shared machinery it exercises is correct.

### Item 4 (the tail): what "delete the u32 forms" actually cost, and the two silent bugs it surfaced

Step 3 finishes here. `facade_native_id_params` 67 → **0**, `sweep_renderer_id`
345 → **182**. `RendererAPI.h` declares no `u32 <name>ID` parameter, so a
translation unit in the sweep bucket that holds a native name has nothing to
pass it to — the boundary is compiler-enforced rather than measured, which is
the property §1.7 credits `sweep_glad_includes` with, now one level up.

**The plan said the counter would drain per entry point. It did not.** The
reasoning was that each `u32` form could go as its last caller migrated. What
actually happens is that the last caller of nearly every form is *another* form's
caller — `ResolveTexture` feeds `BindTexture`, `GetColorAttachmentRendererID`
feeds `CopyImageSubData`, `CreateTexture2D` feeds `SetTextureFilter` — so the
facade flips in one edit and the ~250 call sites are then repaired against the
compiler's own `(file, line)` output. Budget item 4 as one indivisible change,
not a drain. (The same correction item 2 needed, one size up.)

#### Queries become identities, and the `.data()` trap was real

`RHI::ResourceKind::Query` already existed in the registry when the mint landed,
so leaving queries on `u32` would have left that enumerator dead. The concrete
hazard is not hypothetical: `OcclusionQueryPool` hands a query issued in frame N
to `BeginConditionalRender` in frame N+1. A `Shutdown()` + `Initialize()` in
between frees the names, GL may reissue one, and the draw is then gated on an
unrelated occlusion result — a *plausible* frame, not a broken one. A retired
handle resolves to 0 and the conditional render is simply skipped.

`CreateQueries` is where §4's `.data()` warning applies literally, because that
warning was written about this function. The out-span never reaches the driver:
the backend creates into its own `std::vector<GLuint>`, then registers each name
and writes the handles back. `DeleteQueries` resolves *and* unregisters, both
halves. `IsQueryResultAvailable` returns **false** for a stale handle rather than
reading query 0 — for occlusion, a zero result reads as "fully occluded", so the
honest-looking answer is the one that deletes geometry.

#### TWO silent defects, both the same shape, both found by reading rather than by a test

Neither is a type error; both compile, and both would have shipped a wrong frame.

1. **`DeclareTransientTexture(name, desc, backingTextureID)` set only the native
   id.** That is the call that publishes `ShadowMapCSM`, `ShadowMapAtlas` and
   their compare-off raw views into the frame graph. Every consumer of those
   resources reads them through `ResolveTextureHandle` — which answers **null**
   for an entry with no `Handle` — so `DeferredLightingPass` would have fallen
   through to `ShadowMap::GetCSMPlaceholderHandle()` on every frame. Shadows
   gone; no error, no warning, no failing assertion.
2. **The transient planner's alias fan-out set only the native id.** Pass 1
   (`WillAllocate`) sets both since slice 7; pass 2 — the entries that *inherit*
   a sibling's physical resource under aliasing — still set `TextureID` alone.
   So the aliased half of any plan resolved to a null identity. This one is
   worse than (1) because it is load-dependent: it appears only when the planner
   decides to alias, which depends on the frame's resource lifetimes.

**The generalisable rule, and it is a correction to `PhysicalTexture`'s own
comment:** "TextureID and Handle are ALTERNATIVES, exactly one is set" is right
for an **import** — an importer holds one currency and neither is derivable from
the other. It is wrong everywhere the graph itself is the one doing the setting,
because there the graph holds the resource *object* and reads both off one
pointer in one statement. Slice 7 learned this for the transient acquire and
fixed that one site. Item 4 found two more sites governed by the same rule, which
suggests the right move next time is to grep for **every** assignment to
`.TextureID` and check each for a matching `.Handle`, rather than fixing the one
the current slice happens to touch.

#### Two "blockers" in the worklist were phantoms, in opposite directions

* **`VertexBuffer` / `IndexBuffer` expose no `GetRHIHandle()`** — true, and
  irrelevant. Their accessor is spelled `GetBufferHandle()`, and it is used
  *only* inside `Platform/OpenGL/`. What `Renderer3DMeshSubmission` and
  `VirtualMeshRegistry` actually hold is a `Ref<StorageBuffer>`, which has minted
  handles since slice 2. The gap was recorded by reasoning about the type family
  rather than by grepping the accessor the caller uses.
* **"A resource's import may only move to `ImportTextureHandle` after the
  diagnostics can read one"** — still true, and by item 4 it was already
  satisfied. #736's `Debug::NativeTextureIdForDiagnostics` unblocked six imports
  at once (DDGI's atlases, the colour-grading LUT, the fluid intermediates, the
  fog volumes, the TAA/clouds history, the virtual-geometry debug target). A
  sequencing constraint that has been *met* reads exactly like one that is still
  blocking; re-check the blocker before scoping around it.

#### The IBL trio's second currency died with its only reason to exist

`SetGlobalIBL` took *seven* resource parameters: four identities and three native
ids. The natives existed for exactly three lines — `RenderPipeline`'s
`graph.ImportTexture(ResourceNames::IrradianceMap, data.GlobalIrradianceMapNativeID, …)`
— because at the time the graph could not import an identity. Switching those to
`ImportTextureHandle` removed the parameters, the three `Renderer3DData` fields,
the three accessors, and the "both currencies clear together" comment in
`ClearGlobalIBL` that existed to keep them consistent. **When a value carries two
currencies, find the single consumer that forced it; the rest is usually
bookkeeping to keep the two in step.**

#### What is deliberately NOT done

* **`GetRendererID()` still exists** on `Texture` / `Shader` / `Framebuffer` /
  the buffer types. It is what `Platform/` and `Renderer/Debug/` are supposed to
  use, and the graph's diagnostics path reads it on purpose. Item 4 removes every
  way for the sweep bucket to *use* one, not the accessor. Deleting it is Phase 8,
  with the tools relocation.
* **The creators keep their `...Handle` suffix.** Slice 4 called it temporary;
  taking the plain name back would rename ~40 call sites for no semantic gain,
  and Phase 3 reshapes the create family for `ViewHandle` anyway.
* **`shaderRendererID` keeps its field name** on the POD command structs. It has
  been an `RHI::ResourceHandle` since slice 6; the name is the last cosmetic
  residue and accounts for 114 of the remaining `sweep_renderer_id` count. Renaming
  it is churn with a collision risk (§4's `shaderHandle` story) and no behaviour
  change — leave it for a commit that has a reason to touch those structs.

---

## 4b. Phase 3 — what the bindless rehearsal actually proved, and what it disproved

Phase 3's stated *purpose* is validating the engine-side binding contract before
Vulkan-specific effort starts. So the useful output is not "it works" — it is a
sorted list of which predictions held and which did not, because a prediction
that failed is worth more to Phases 4–7 than one that held. Sorted by how much
it should change a later phase's plan.

### The measured surface, and the three published numbers (again)

| Source | "texture-bind sites" | What was wrong |
| --- | ---: | --- |
| Issue #691 body | ~125 | rough estimate, and it says "~35 passes" |
| ADR 0011 §1.3 | ~173 | measured, but only `BindTexture(` and only in `.cpp` |
| Phase 3 handover | 197 | `BindTexture(` outside `Platform/`, all extensions |
| Measured, `BindTexture(` **and** `BindImageTexture(`, comments/strings blanked | **232** | — |

The same lesson as §1, one phase later: **the number moves with the definition,
so publish the definition.** The ratchet's `sweep_bind_texture_sites` counts both
families because both disappear under the heap — an image binding is a heap slot
too — and it counts after blanking so the facade's own prose about the family
name does not inflate it. Correct the issue and the ADR rather than adding a
fifth number.

### DISPROVED: the shader side of the rehearsal does not transfer, and could not

This is the finding that reshapes Phase 6, and it is a *toolchain* fact rather
than a design one, which is why no amount of ADR review would have caught it.

`GL_ARB_bindless_texture` is a GLSL-only extension that predates SPIR-V and has
no representation in the Vulkan target environment. Every production shader
enters the pipeline through `shaderc(target = vulkan 1.2)` (§6), so **bindless
GLSL cannot travel the production shader path at all** — not "compiles to
something suboptimal", but rejected at the first hop. The only route is handing
the original GLSL to `glShaderSource`, bypassing SPIR-V and SPIRV-Cross
entirely.

Consequences, in order of how much they matter later:

1. **The GLSL half of Phase 3 validates nothing about Vulkan.** Vulkan reaches
   the same shape through descriptor indexing / `VK_EXT_descriptor_heap`, which
   *is* expressible in SPIR-V and needs no second compile route. So the
   rehearsal's shader work is genuinely throwaway, where the ADR implied it was
   a dry run.
2. **A dual-path renderer needs dual-path shader ARTEFACTS — but not dual
   source files.** Measured: one `.glsl` behind `#ifdef OLO_BINDLESS` serves
   both, because with the define absent the `#extension` line is preprocessed
   away before glslang sees it, so the slot-based variant still travels the
   normal Vulkan-SPIR-V path from the same file. What cannot be shared is the
   compiled artefact: two programs, two caches, two compilers. A bonus of that
   shape is that the five SPIR-V-reading shader tests still validate the shared
   UBO layouts and stage interfaces via the default variant, leaving only the
   bindless branch unchecked. **The conversion's real cost is therefore in the
   PASSES, not the shaders** — each of the 232 sites needs a "write an offset or
   bind a texture" fork, and that is where the silent-failure risk lives.
3. It is pinned by `BindlessShaderPipelineTest`, which **fails if the answer
   changes in either direction**. A `EXPECT_FALSE` on "shaderc accepted it" reads
   backwards until you notice that good news here deletes a whole compile route,
   and silently keeping a dead route is the failure worth catching.

### DISPROVED: most of the backend does not transfer either

ADR 0011 §1.2 frames `ARB_bindless_texture` and `VK_EXT_descriptor_heap` as two
realisations of one model. Above the backend interface that is true. Below it,
three things exist only on GL and one of them is load-bearing:

- **Residency.** A handle must be made resident before use and non-resident on
  destruction, and making an already-resident handle resident *again* is an
  `INVALID_OPERATION` — so the backend has to refcount. Vulkan writes a
  descriptor and is done.
- **Immutability.** Taking a handle freezes the texture's parameters, and a
  handle names the *object*, not a view of it.
- **Sampler folding.** GL bakes sampler state into the handle, so the sampler
  heap is bookkeeping on this backend. Under a split heap it is real, and an
  exhausted sampler heap changes from a warning into a correctness failure.
  The engine's sampler *deduplication* (§1.2a's "nothing to port") is therefore
  the one piece of new machinery here that Phase 4 inherits directly.

**What does transfer is everything above `IDescriptorHeapBackend`**: views
owning slots, the two lifetime classes, offsets as data, fetch-don't-store,
generation validation, poison-on-free. That is roughly the whole of
`RHIDescriptorHeap.{h,cpp}` and all of the contract tests. Keeping the backend
seam that narrow was worth more than any other structural decision in the phase.

**One prerequisite the engine already satisfies, and it is worth knowing before
someone budgets for it.** `ARB_bindless_texture` refuses a handle for a texture
whose storage can still change, so every sampled texture must have been created
with immutable storage. `OpenGLTexture*` already uses `glTextureStorage2D`
throughout and contains **zero** `glTexImage2D` calls — a consequence of the
DSA-only house style rather than of any bindless planning. Had it not, this
would have been a texture-creation sweep on top of the binding sweep. Vulkan has
the same property by construction (a `VkImage`'s format and extent are fixed at
creation), so the constraint is not GL-specific and the engine is already
compatible with both.

### HELD, and it caught a real hazard: the resource registry's stability is a liability here

`ResourceRegistry` deliberately **preserves** a handle across an in-place reload
(`ScopedResourceHandle::Sync` repoints, never retires) — §4's "identity is the
C++ object", and the property that makes caching a `ResourceHandle` safe.

A descriptor does **not** inherit that safety. An `ARB_bindless_texture` handle
names the underlying object, so recreating the storage leaves every descriptor
naming a deleted object *while the view's own generation is unchanged* — which
means `OffsetOf` cannot detect it. The cheap generation check that catches every
other staleness in this layer is structurally blind to exactly this one.

**This is the exact mirror image of the slice-6 finding** where stable identity
made the redundant-bind cache skip a bind that had to happen. Same root cause
(identity outliving storage), opposite symptom (stale descriptor vs skipped
rebind), same remedy: `RHI::DescriptorHeap::InvalidateResource` is the sibling of
`InvalidateTextureBinding`, and **every site that recreates a resource's storage
must call both**. Generalisable well past this issue: *when a layer deliberately
makes an identity outlive its storage, every cache keyed on that identity needs
an explicit invalidation hook, because the identity itself can no longer report
the change.*

### HELD: two lifetime classes, and the ring is what makes aliasing expressible

ADR 0011 §1.2 predicted that persistent transient slots would force a mid-frame
offset rewrite under `WriteNewVersion` aliasing. That held exactly, and building
it made the reason concrete: with a per-frame ring, one physical object holding
two offsets in one frame is just two ring entries, and the alias becomes
*visible in the heap* rather than being something the heap has to reconcile.
`AliasedResourceHoldsTwoOffsetsInOneFrame` is the test; it is short, and it is
the one to read first if the model ever needs re-deriving.

The ring also turns §1.2's "never store a transient offset across a use" from a
rule nobody can enforce by review into a detectable failure: retiring the ring
bumps every generation, so a held offset does not merely go stale, it *reports*
stale.

### The counter's target is NOT zero, and that is a design decision

`sweep_bind_texture_sites` is the first ratchet counter in this issue whose
target is not zero, and the difference is worth stating because the previous
three trained the opposite instinct. `ARB_bindless_texture` is not universally
available, so the slot-based path must survive as the fallback; a zero would
mean the engine had stopped working on those devices. There is also no
type-system equivalent of `facade_native_id_params` available here — **both
paths must compile, so nothing can make the old one unrepresentable.** It is a
monotone-down progress measure with a floor guard, not a completion criterion.

### The neutral `ViewDesc` is under-specified, and it will bite Vulkan too

`RHI::ViewDesc` carries a `SubresourceRange` and a `FormatOverride` but nothing
that lets a backend actually *build* the view: GL needs the source target and
internal format for `glTextureView`, and Vulkan needs the same information for
`VkImageViewCreateInfo`. The GL backend therefore declines any view that is not
the whole resource, and counts it.

Recorded as a Phase 4 input rather than fixed here, because the fix is a real
design question — widen the neutral desc, or have the heap ask the registry for
the resource's metadata — and choosing it against one backend is how the
original `GLenum`-in-the-facade problem started. **The useful part is that the
gap is a property of the neutral model, not of GL**, so it was going to surface
during Vulkan bring-up regardless; finding it on the debuggable backend is the
rehearsal working as intended.

### The raw-GLSL route, and what converting a real pass actually cost

The constraint above says bindless GLSL cannot enter the SPIR-V pipeline. The
route around it is small and confined: `OpenGLShader::CreateProgramFromRawGLSL`
feeds the already-include-resolved source straight to `glShaderSource`. Four
things about it are worth knowing before building the equivalent anywhere else.

**Variant selection is one token and one toggle.** A shader opts in by mentioning
`OLO_BINDLESS`; the route is taken only when `DescriptorHeap::IsEnabled()`. On
any failure it falls through to the ordinary path, so a broken bindless branch
costs the frame its optimisation and never its shader. `Reload()` re-decides,
which is what makes an A/B possible without a restart.

**The engine must inject `#extension`, not the include.** GLSL requires every
`#extension` directive to precede all non-preprocessor tokens. Put it in
`include/BindlessHeap.glsl` and every converted shader inherits an invisible
rule — *the include must sit above your first declaration* — which is exactly
where you do not want it, since the natural place for it is next to the samplers
it replaces. Injecting `#extension` + `#define OLO_BINDLESS 1` immediately after
`#version` makes include placement a non-issue across all ~35 shaders. Found by
writing the SSAO conversion the obvious way first.

**The program-binary cache must be keyed on the variant.** A driver stamps a
binary with its own version, not with which GLSL branch produced it, so
`.cached_opengl.pgr` and `.cached_opengl.bindless.pgr` have to be separate files
— otherwise a bindless binary loads into a slot-based run, links cleanly, and
samples nothing. This is ADR 0011 §3's "encode the target env in the cache
filename" one variant axis over, and it was caught immediately by
`AssetContentValidity.AllCacheFilesMatchKnownPattern`, which is that whitelist
earning its keep.

**What a converted pass looks like.** SSAO, the first one, in full:

- three `context.BindTexture(SLOT, tex)` became
  `context.BindTextureOrHeapOffset(SLOT, tex, lifetime[, sampler])`, plus one
  `FlushHeapOffsets()` before the draw;
- the shader's sampler declarations went inside `#ifdef OLO_BINDLESS`, replaced
  by `#define u_DepthTexture OLO_HEAP_TEX_2D(19)`;
- **the shader body did not change by a single character.**

That last point is the ergonomic result worth carrying to the other passes. It
works because the offset table is indexed by the *same `TEX_*` constant* the
bindful branch writes in `layout(binding = N)` — ADR 0011 §1.1's "the number
survives, promoted from a compile-time constant to runtime data", made literal.
The two variants are then structurally unable to disagree about which texture is
which, and a reviewer diffs a declaration block rather than a shader.

**The one genuinely dangerous line is the std140 layout.** The offset table is
`uvec4 g_OloHeapOffsets[16]`, not `uint g_Offsets[64]`, because std140 pads a
`uint` array to a 16-byte stride. Get it wrong and the shader reads every fourth
entry — three textures in four sample a *different real texture*, in a frame that
still looks plausible. `OffsetsReachTheShaderThroughTheRealSeamAtTheirSlotIndices`
drives the real `RGCommandContext` seam with slots 19 and 22 specifically because
they land in different uvec4 groups **and** different components, so no layout
bug can satisfy both.

### Publishing the heap once per frame is WRONG, and the test was hiding it

The first batch of converted passes rendered a **black viewport**. The cause is
worth recording in full, because both halves of why it survived are reusable
mistakes.

`RenderGraph::Execute` published the heap once, just before `ExecutePlan`, with a
comment asserting *"every transient view this frame was minted during planning,
so the table is complete by now"*. That assertion is false. A pass mints its
views inside its own `Execute` — `BindTextureOrHeapOffset` is a pass-time call —
so at the moment of that flush **this frame's transient descriptors do not exist
yet**. They land in the CPU mirror, get marked dirty, and are never uploaded. The
offsets then index heap slots holding the previous frame's descriptor or nothing
at all.

The fix is that `FlushHeapOffsets()` publishes *both* — descriptors first, then
the offsets that index them — at each converted pass. The frame-level flush stays,
but only to establish the heap's binding for passes that read persistent views.

**Why it survived the first conversion.** Two independent reasons, and each is a
verification anti-pattern on its own:

1. **The first converted pass was one the active render path did not execute.**
   SSAO compiles and logs `[Bindless] built through the raw-GLSL route`, which
   reads exactly like success — but the scene ran GTAO, so the converted pass
   never drew. *A shader compiling through a new path is not evidence that the
   path renders.* Confirm the pass executes before reading a clean frame as a
   pass.
2. **THE GPU TEST WAS SUPPLYING THE MISSING CALL.** It did
   `context.FlushHeapOffsets(); RHI::DescriptorHeap::Get().Flush();` — the second
   line being precisely the publish the engine was failing to do. The test passed
   because it sequenced the mechanism *for* the engine.

That second one is the generalisable lesson, and it is sharper than "test the
real thing": **a test that performs a sequencing step on the subject's behalf
cannot detect that the subject omits it.** The test now calls only what a
converted pass calls, so the same bug would fail it. Whenever a test sets up a
mechanism, ask which of those setup lines production code is also supposed to
execute — those lines are exactly where a missing call hides.

### "Unbind" has no translation, so the heap needs a reserved null descriptor

A slot-based pass clears an input by binding a null texture — `ToneMapRenderPass`
does exactly this with `RHI::NullResource`, to say "I am not using the water-depth
input this frame". Under the heap there is **nothing to clear**: the shader reads
an offset, and an offset left untouched is a perfectly valid index that goes on
sampling the previous texture.

That is the worst failure this model can produce — a real, plausible, wrong image
— and it is invisible to every check that only asks "did the frame render?". So
heap slot 0 is reserved, never allocated, permanently poisoned
(`RHI::kNullHeapOffset`), and every call site that fails or clears points there.

**Generalisable: when converting a binding model to an indexed one, enumerate the
operations that have no index equivalent.** Binding a resource maps cleanly;
*un*binding does not, because the old model's "absence" was a state and the new
model's is a value that must be written. The same applies to the fallback path —
`BindTextureOrHeapOffset` writing the null offset when it declines is not
belt-and-braces, it is the only thing stopping a stale index surviving a
heap-exhaustion or dead-resource case.

### Conversion is incremental WITHIN a shader, which is what makes shared includes tractable

`PostProcess_Fog.glsl` reads three textures: full-res depth (`sampler2D`), the
froxel volume (`sampler3D`) and the CSM shadow array — and the shadow array's
declaration lives in a **shared include**. Converting a shared declaration would
flip every shader that includes it in one indivisible step, which is the shape of
change that cannot be verified pass by pass.

It turns out not to be forced: a bindless-variant program can carry ordinary
`layout(binding = N)` samplers alongside heap ones, and the pass simply keeps
binding those the old way. Fog is converted for its depth and froxel inputs and
still binds `TEX_SHADOW` conventionally.

Two consequences for planning the remaining sites:

- **Shared includes are not a blocker**, they are just the last thing to convert.
  A shader can be moved input by input.
- **`sampler3D` through the heap is proven**, not assumed — fog's froxel volume
  is a real 3D texture sampled through `OLO_HEAP_TEX_3D`. Combined with the
  `sampler2D` passes and the GPU test's array/shadow macros, the macro layer is
  exercised beyond the 2D case that everything else uses.

### How far "full bindless" actually reaches — measured, not estimated
<!-- The storage-image bucket called out below is now BUILT; see §4c. -->


Eleven shaders and 24 sites later the counter stands at **208**, down from the
232 measured before any conversion. The remainder is not homogeneous, and the
split decides whether the phrase means anything:

| Bucket | Sites | Shape |
| --- | ---: | --- |
| Ordinary pass binds (`Renderer/Passes/`) | ~128 | Mechanical, exactly like the converted ones. |
| `Renderer/Commands/` — the material path | ~21 | The redundant-bind cache. One indivisible unit, same shape as Phase 2 slice 6. |
| **`BindImageTexture` — storage images** | **38** | **Not modelled at all.** |
| Facade declarations, the seam's own fallback, misc | ~21 | Mostly not deletable — the fallback bind is a real site and correctly counted. |

**The storage-image bucket is the real answer to "can we go full bindless".** The
heap produces *sampler* descriptors only: `ViewDesc` + `SamplerDesc` →
`glGetTextureSamplerHandleARB`. An `imageLoad`/`imageStore` binding is a
different descriptor — `glGetImageHandleARB`, with its own residency and a
format+layered+level key that `ViewDesc` does not carry. `ARB_bindless_texture`
supports it and `VK_EXT_descriptor_heap` treats it as just another descriptor
type, so this is a gap in *our* model rather than in either API — but it is ~18%
of the surface, it is concentrated in the compute passes, and nothing about the
sampler path generalises to it for free. **Anyone claiming "full bindless" has to
price this in as a second heap-side feature, not as more call sites.**

### Verify a mechanism against pixels it can produce, not against a frame it changes

CLAUDE.md's rendering rule says a renderer change must be checked against
pixels. The obvious reading — convert a production pass and compare screenshots
— is the *weaker* evidence here, and it is worth saying why, because the
instinct is strong.

A converted SSAO pass proves "SSAO still looks like SSAO", which a dozen
unrelated bugs also produce; a wrong heap offset renders a different *real*
texture, and against a noisy screen-space effect that is exactly the kind of
plausible-looking wrong the noise floor swallows
(`live-verification-noise-floor.md`). It also puts the entire shader pipeline on
a new compile route for a mechanism that is off by default.

`BindlessHeapGpuTest` instead renders two deliberately-different solid colours
through the real heap and reads the exact texels back, so a swapped offset, a
dead descriptor and an unbound heap each fail differently and specifically. The
generalisable rule: **when adding a mechanism rather than changing an image,
build the smallest scene whose correct output is known exactly, and check that
— then keep the golden suite as proof that nothing else moved.**

---

## 4c. Phase 3 bucket 3 — building the second descriptor kind

`sweep_bind_texture_sites` 208 → 178. **Every one of the 30 is a
`BindImageTexture`**, and that family goes 36 → 6. §4b's table predicted this
bucket would cost "a second heap-side feature, not more call sites"; that held,
and the ratio is worth having: the heap and backend changes were roughly two
thirds of the work, the 30 call sites and 15 shaders the other third.

### The floor for the image family is 6, and every one of them is a real thing

| Site | Count | Why it stays |
| --- | ---: | --- |
| `RendererAPI.h`, `RenderCommand.h` | 3 | The pure-virtual **declaration** and the facade's declaration + forward. The counter matches a declaration exactly as it matches a call. |
| `HeapBindingSeam.cpp` | 1 | `BindImageOrOffset`'s **own fallback call**. `ARB_bindless_texture` is not universally available, so this is permanent — deleting it deletes the degradation path, not the coupling. |
| `VirtualGeometryPass.cpp` | 2 | Deliberate. See below. |

**The two deferred ones are the interesting entry.** They feed
`VirtualMeshGBuffer.glsl`, and the bindless compile route produces no SPIR-V and
so never runs `Reflect()` — which leaves `m_IsDeferredCapable` false and
misroutes a G-Buffer shader out of the deferred producer bucket into the
forward-overlay fallback. `CreateProgramFromRawGLSL` already detects and errors
on exactly that.

Converting the **call sites alone** would have been perfectly safe: the seam
forks on `Shader::IsBoundProgramBindless()`, so an unconverted program still gets
a real bind. It would also have bought two counter points and no behaviour
change. That is precisely the shape of edit the counter exists to discourage, so
they stay and say so in place. *A ratchet you can satisfy without changing
behaviour is a ratchet you have to be willing to leave unsatisfied.*

### The four things a second descriptor kind actually needed

1. **`ViewUsage` on `ViewDesc`** (not a second desc type — ADR amendment (27)),
   plus `StorageAccess`. `glGetImageHandleARB` in the backend, with its own
   residency namespace.
2. **A second `usage` parameter on `ReleaseDescriptor` and `NullDescriptor`.**
   GL has two residency namespaces with two entry-point pairs, and the spec does
   not promise a texture handle and an image handle are numerically
   distinguishable — so a `u64` alone cannot say which call to make. Passing the
   kind is cheaper and safer than a lookup that could answer wrongly.
3. **A second reserved null slot.** A null is per *kind*: `image2D(samplerHandle)`
   is undefined in exactly the way `image2D(0)` is.
4. **A disjoint index region in the offset table.** GL image units and texture
   units both start at zero; without `HEAP_IMAGE_SLOT_BASE` image unit 0 and
   `TEX_DIFFUSE` overwrite each other's offsets and each renders the other's
   resource.

### Residency has an ACCESS, and it has to be widened rather than re-transitioned

`glGetImageHandleARB` takes no access — `glMakeImageHandleResidentARB` does. So a
read-only and a read-write view of the same (texture, level, layer, format) are
**literally the same driver handle**, while reading a `WRITE_ONLY`-resident
handle (or writing a `READ_ONLY` one) is undefined, and making an already-resident
handle resident again is an `INVALID_OPERATION`.

GTAO does this inside one frame: its edge texture is written by the main pass and
read by the denoise pass. The backend therefore tracks the residency access and,
when a second acquire needs one the current residency does not cover, drops
residency and re-establishes it as `READ_WRITE` — counted in
`Stats::ImageResidencyWidenings` so the behaviour is observable rather than
inferred.

This is the same shape as the sampler path's folding (GL bakes sampler state into
a texture handle), and it belongs in the same place: the neutral key stays honest,
the backend does the folding.

### Three defects, none of them a type error

**1. The persistent memo cache could not tell two views apart.** Its key was
`(resource, samplerSlot, depthCompare)` — sound *only* because the GL backend
declined every view that was not the whole resource, so no two distinguishable
views could reach it. `HZBGenerator` asks for **four mips of one texture in one
dispatch**; under that key the second, third and fourth would each have been
served the first's view, and the whole pyramid would have been written at level
0. A plausible frame with a broken occlusion pyramid.

*Generalisable: a cache key that is "sufficient" only because a downstream layer
refuses the distinguishing cases is a latent bug with a scheduled trigger date —
the date the downstream layer stops refusing.*

**2. `InvalidateResource` published a zero descriptor on a failed re-acquire.**
The comment beside it said "poisoning renders black, which is the honest answer";
the code stored the returned `0`. A zero bindless handle is not black — sampling
one is undefined behaviour, which is the entire reason `NullDescriptor` is a real
resident texture rather than a constant. The path is reached precisely when
something has already gone wrong, which is the worst moment for the instrument to
become non-deterministic. Pre-existing; fixed here.

**3. `SetBoundProgramBindless` was published by graphics shaders only.**
`OpenGLComputeShader::Bind()` never touched it, so a bindless graphics program
followed by a compute program left the flag `true` — and the first converted
compute pass would have recorded offsets into a table its program never declares
while binding nothing at all. Latent rather than live, and it would have
presented as garbage in a compute output with no diagnostic anywhere.

### Compute needed no second compile route — check the constraint's blast radius

§4b's headline finding is that bindless GLSL cannot enter the
`shaderc(target = vulkan)` pipeline, and the graphics side needed
`CreateProgramFromRawGLSL` to get round it. The natural inference is that compute
needs the same thing.

It does not. `OpenGLComputeShader::Compile` has **always** fed include-resolved
GLSL straight to `glShaderSource` — it never travelled that pipeline — so the
entire conversion was the same `#extension` + `#define` prologue injection, ~20
lines. Budgeting this bucket from the graphics route's cost would have
over-estimated it substantially.

*Generalisable: a constraint that forced a workaround in one subsystem may simply
not apply to the next. Check which subsystems are actually subject to it before
pricing the workaround again.*

### An image macro DECLARES; a sampler macro is an expression

The one ergonomic difference, and it changes where a conversion's diff lands. A
format layout qualifier belongs to a declaration, and an image initialised from a
buffer read is not a constant expression — so the bindless declaration cannot sit
at file scope and moves **into the function that uses the image**. In `HZB.comp`
that is `WriteMip()`, not `main()`.

The body is still byte-identical between the two variants, so the reviewable
property §4b credits the sampler path with survives; the declaration simply moves
to a different scope rather than staying at the top of the file. The full recipe
is in [glsl-shaders.md §5b](glsl-shaders.md).

### The editor gate found what 5296 tests could not — and the suite structurally cannot

Running the editor with `OLO_RHI_BINDLESS=1` found **four** defects after a fully
green suite. The reason is structural rather than bad luck, and it is worth
internalising before trusting a green run again:

**the suite never builds a bindless variant at all.** `WantsBindlessVariant()`
requires `DescriptorHeap::IsEnabled()`, the toggle defaults off from the
environment, and no test sets it — so every shader in every test compiles its
DEFAULT branch. §5a of glsl-shaders.md already says the SPIR-V-reading tests
"only leave the bindless branch unvalidated"; the operational consequence is
stronger than it sounds: *a green suite says nothing whatsoever about the
bindless path.*

What that let through:

1. **`readonly` on a bindless image local is rejected** — initialising a
   `readonly` variable is a write (`error C7504`). Four compute shaders
   (Terrain_Erosion, VirtualDebugColorize, FluidSmooth, GTAO_Denoise) silently
   fell back to slot-based. The spelling probe existed and had enumerated where
   the qualifier GOES without enumerating which qualifiers EXIST — it now covers
   `coherent` and asserts `readonly` must fail.
2. **A bindless image must be declared in every function that names it.** Caught
   in HZB, missed in Terrain_Erosion (five functions). Found by writing a
   mechanical scope audit rather than re-reading — the audit immediately turned up
   a fifth site the manual pass had missed.
3. **`Shader::IsBoundProgramBindless` was stale for every command-dispatched
   draw.** `RendererAPI::BindShaderProgram` reaches `glUseProgram` without any
   `Shader` object, so the flag carried whatever the last post-process shader set:
   stale TRUE skips a bind an unconverted program needed, stale FALSE leaves a
   converted program reading an offset nobody wrote. Black sky, missing terrain,
   clean log. **This was the second instance of a bug already fixed once** (in
   `OpenGLComputeShader::Bind`), and the first fix's note claimed "both Bind()s
   now publish" — having fixed a class once is not evidence of having found its
   siblings. The question is "what are ALL the paths into this state", not "where
   else does this class occur".
4. **A converted shader can be individually wrong.** `Skybox.glsl` rendered a flat
   sky; reverting that one file (shaders need no rebuild, which makes this a
   two-minute bisect) restored it. Left slot-based.

### The #740 noise floor does not reproduce, and the claim it supports is suspect

§4b records that the bindless A/B measured **14.36% differing pixels against a
14.39% floor** and concluded the heap changes nothing.

Re-measured on the VehiclesTest water sandbox, the same-config floor is
**2.70–3.10%** and the OFF-vs-ON measurement is **13.65%** (RMSE 19.1) — five
times the floor. That delta is **pre-existing**: reverting every shader converted
after #740 leaves it at 13.65% versus 13.66% with them, so the later conversions
contribute nothing measurable.

The methodological point is the reusable one. A control landing within **0.03%**
of the signal it is meant to calibrate is not a reassuring result, it is the
signature `live-verification-noise-floor.md` describes — a floor that is
accidentally measuring the same instability as the measurement, which then
"passes" for a correct AND a broken implementation alike. A floor should be
*small*; when it comes out the same size as the effect, the first hypothesis is
that the harness is unstable, not that the effect is absent.

So: **the bindless path currently differs visibly from the slot path, and #691
does not yet have the evidence its notes claim.** The heap is off by default so
nothing ships broken, but this is open work, not a closed question.

> **SUPERSEDED (2026-08-04) — see §4e.** Both figures above are measurements of an
> unstable harness, not of the heap. Re-measured on the same sandbox, two captures
> of the *same build* at the *same pose* differ by 23.5% (RMSE 9.74) — larger than
> any ON-vs-OFF number ever recorded here, including these. The scene animates
> water, vehicles and a TAA history, so the live A/B cannot resolve the question in
> either direction and no amount of retaking will change that. The evidence that
> does exist: all **80** golden and visual-evidence tests pass in both
> configurations, un-rebased. Do not treat "the bindless path differs visibly" as a
> standing finding — it was never measured by an instrument that could show it.

### Two ordering rules the slot path did not care about

- **Bind the shader before the image.** The seam consults
  `Shader::IsBoundProgramBindless()`, which describes the program *in flight*, so
  an image bound first silently takes the fallback path even with the heap on.
  Two sites needed reordering (`TerrainErosion`, `VirtualGeometryPass`'s
  colorize) — both had bound resources first purely as a matter of style.
- **A ping-pong needs a flush per iteration.** `FluidSmooth` and `GTAO_Denoise`
  swap source and destination every pass, so a flush hoisted out of the loop
  publishes only the final pair. Same rule as §4b's "a pass with several draws
  needs a flush per draw", one loop level down.

---

## 4d. Converting a shader silently changes what its TESTS have to do

Converting a pass is not only an engine change. Any test that drives that
shader directly must bind the same way the engine now does, and the compiler
cannot tell you it doesn't.

`FullscreenPass::Draw` (`tests/.../RenderPropertyTest.cpp`) bound its input with
a raw `glBindTextureUnit(0, tex)`. That was correct until `PostProcess_FXAA`,
`_DOF` and `_MotionBlur` were converted — after which those shaders compile to
the **bindless variant whenever the heap is on** and read
`g_OloHeapOffsets[TEX_*]` instead of the sampler at unit 0. The bind then
addresses a unit nothing samples, the shader reads the reserved null descriptor,
and the test reads back **black**: `Expected rgba >= 251, actual 0`.

Three things worth carrying forward:

- **The failure is total, not subtle.** A bindless shader that cannot see its
  input samples the null and returns zero. `actual: 0` on an *identity* test
  ("zero velocity is identity", "depth at focus distance is identity") is the
  signature — read it as "the binding never arrived", not "the math drifted".
- **Publish the offset AND keep the raw bind.** Which variant is in flight is
  decided by heap state the draw helper does not control, so it must satisfy
  both. Same reasoning as `PublishTextureOffsetAndBind`.
- **A test harness has no frame loop.** Nothing else will call
  `HeapBinding::FlushOffsets()`, so the helper has to — the descriptors *and*
  the offsets indexing them are both unpublished until it does (§4b).

**THE FIX IS TO OPT THE HARNESS OUT, NOT TO TEACH IT THE HEAP.** The obvious
move — make the harness bind through `HeapBinding` like the engine does — was
tried and abandoned after three rounds. It is recorded here because it looks
right and costs a day:

- **The heap's lifetimes assume a frame; these harnesses have none.**
  `FrameTransient` slots are reclaimed only at a frame boundary, so the ring
  cursor marched forward for the whole process until the 1024 slots were gone,
  after which every acquire failed, the seam fell back, and offsets were staged
  as null. Black frames in whichever tests ran last.
- **Retired transient slots are released LATER, somewhere else.** The release
  happened at the next `ResetFrameTransients` — inside an unrelated test's frame,
  by which time the harness's texture was long deleted:
  `GL_INVALID_OPERATION ... Not a valid texture`, attributed by the synchronous
  debug context to whoever checked error state next. It failed
  `VirtualGeometryPerf`, which had nothing to do with any of it.
- **Retiring eagerly instead puts descriptor teardown next to caller-owned
  textures.** The harness holds `FullscreenPass` as a member, so its destructor
  body deletes the texture and only THEN destroys members — same GL error, new
  disguise. Switching to `Persistent` fixed the ring and the errors and still
  left an order-dependent crash that only reproduced in the full suite.

So `PostProcessHarness` and its golden-image twin wrap shader creation in
`ScopedSlotBasedShaders`, and their binds stay plain `glBindTextureUnit` (behind
`BindSlotBasedInput`, a named function whose only job is to make that dependency visible
at the call site). Coverage does not move: `BindlessHeapGpuTest` proves the seam
with real texel readback, and the visual-evidence suites drive these same shaders
through the real passes with the heap on. **A math test should measure math.**

The transferable rule from the wreckage: **any cleanup that touches GL must run
while the thing it is cleaning up still exists.** That single mistake appeared
three times in one issue wearing three costumes — `~OpenGLRendererAPI` at atexit
after the context died, `~FullscreenPass` via member-destruction order, and a
transient heap slot released in a later test's frame. Check it before writing the
destructor, not after the errors appear.

**The failure mode to fear is a test that starts passing for the wrong reason.**
`MotionBlurStaticTest.ZeroVelocityIsIdentity` sat green throughout: it asserts
that zero velocity produces an identity image, and a velocity texture the shader
cannot see reads as zero velocity. It would have passed whether the binding
arrived or not. Its sibling `VelocityDirectionDrivesBlur` — which asserts a
*positive* effect — is the one that caught the bug. When converting, audit the
identity/no-op tests in the same file: those are precisely the ones a missing
binding satisfies.

The general rule: after converting a pass, grep for tests that load that shader
by filename. They bind through their own helper, not through the pass you just
converted, and they will keep compiling and start lying.

---

## 4e. Bucket 1 resumed: the revert was aimed at the wrong layer

The revert recorded in `rhi_boundary_baseline.json` backed out all 30 shader
conversions and concluded *"expect ONE systematic defect in the conversion
recipe, not sixty."* Right about the count, wrong about the layer. The defects
were in the **environment**, and #747's later commits fixed them without touching
a shader: a non-GPU unit test that shut down the process-wide `DescriptorHeap`
and never restored it, an offset table never re-based across a heap epoch bump, a
static-destructor teardown running at atexit with no GL context, and a
leak-detector constant that had drifted behind the binding registry.

Restoring all 30 by reverse-applying the revert, in six batches, held parity at
every step — **5417 passed / 1 failed in both configurations**, the failure being
the pre-existing `AtmosphereVisualEvidenceTest` (#735) that also fails with the
heap off. `ToneMap` and `FullscreenBlit`, which the revert's own bisect blamed for
roughly half of the 74 failures, now pass untouched.

**The transferable part is the bisect, not the outcome.** "Reverting ToneMap +
FullscreenBlit alone took 74 → 40" describes a per-frame *ordering* effect — those
two run every frame, so they were the biggest consumers of a broken shared
fixture, not the biggest producers of bugs. A failure set that moves when test
order moves is evidence about shared state (§4d already says this); a bisect over
*subjects* will still find a subject, and it will be the wrong answer that fits.
Bisect the fixture before reverting N changes to find one shared cause.

### Verify that a conversion RAN — and know all three ways it says so

A failed bindless build degrades to the slot path silently, so a green suite is
satisfied just as well by a conversion that never executed. The evidence is the
engine log's set of shaders that took the route, plus zero `.bindless.failed.glsl`
dumps and zero fallback warnings.

It announces itself **three different ways**, and a detector missing any one
reports healthy conversions as silent fallbacks:

| case | log line |
|---|---|
| graphics, fresh build | `'<Name>' built through the raw-GLSL route (no SPIR-V).` |
| graphics, `.pgr` cache hit | `Loaded bindless program from binary cache: <path>` |
| **compute** | *no `[Bindless]` line at all* — only a `(bindless)` suffix on `Compiled compute shader '<Name>'` |

The compute case is structural rather than an oversight:
`OpenGLComputeShader::Compile` has always fed raw GLSL to `glShaderSource`, so it
never travelled the shaderc→SPIR-V hop that forced `CreateProgramFromRawGLSL` on
the graphics side, and never grew that logging. Matching only the cache form made
six freshly-converted shaders — i.e. exactly the ones under test — look like they
had fallen back; matching only the graphics forms did the same to seven `.comp`
files. Both false alarms are shape-identical to the real failure mode, so neither
can be waved off, and both cost a diagnosis.

### A suite-wide route list under-reports for a second reason

Some shaders are created only inside `ScopedSlotBasedShaders` scopes (the
post-process math harnesses), so in a full-suite run they never build a bindless
variant at all. That is by design (§4d), but it means the route list from a full
run is a *lower bound*. Confirm a specific conversion by running the tests that
drive that pass in isolation, where nothing has pre-empted it.

And some passes are unreachable from the suite entirely: `Terrain_Erosion.comp` is
instantiated only by the editor's terrain brush, so no test can exercise its
bindless variant however the suite is run. Converting it is fine; claiming the
suite verified it is not.

### The live ON-vs-OFF A/B is not an instrument on an animated scene — stop retaking it

§4c records the #740 A/B (14.36% differing against a 14.39% floor) and re-measures
it at 13.65% against a 2.70–3.10% floor, leaving "the bindless path differs
visibly from the slot path" as open work. Re-measured again here on
`VehiclesTest.olo`, through the MCP camera at four fixed orbit poses,
`settleFrames = 12`:

| measurement | pixels differing (>8) | RMSE |
|---|---|---|
| **noise floor** — same config, same pose, two consecutive captures | **23.50%** | **9.74** |
| heap ON vs OFF, front | 15.54% | 7.44 |
| heap ON vs OFF, side | 31.60% | 9.19 |
| heap ON vs OFF, high | 8.43% | 4.68 |
| heap ON vs OFF, low | 22.25% | 7.62 |

**Every ON-vs-OFF RMSE (4.68–9.19) is below the same-config RMSE floor of 9.74.**
The claim is about RMSE specifically, and deliberately not about the pixel counts:
the side pose differs in 31.60% of pixels against a 23.50% floor, so "two captures
of the identical build differ by more" is true of the RMSE and false of the pixel
percentage. The scene animates water, three vehicles and a TAA history, which is
enough for two captures of the *identical* build at the *identical* pose to land
in the same RMSE band as the two builds do against each other. This measurement
cannot distinguish a correct implementation from a broken one — which
is precisely what `live-verification-noise-floor.md` says a floor the size of its
signal means, and precisely the error #740 made in the other direction by reading
"signal ≈ floor" as parity.

So the earlier figures were not wrong so much as **meaningless on this scene**, and
retaking them on it will keep being meaningless. Do not quote a live A/B here.

**The deterministic instrument already exists and was ignored.** The suite's 80
golden and visual-evidence tests use fixed cameras and committed goldens, and they
run under `OLO_RHI_BINDLESS=1` exactly as they run without it. On this branch all
80 **ran (0 skipped) and passed in both configurations without
`--olo-golden-rebase`** — that is the ON-vs-OFF pixel comparison, done
against a stable reference, and it is the evidence to cite. A live capture is
still worth taking for what it *can* show (the frame renders, the log is clean,
the expected shaders took the route); it is not worth differencing.

If a live A/B is ever genuinely needed, pause the scene or pick a static one
first, and publish the same-config floor next to the number — a floor is only
reassuring when it is *small*.

### A BINDLESS DESCRIPTOR BAKES SAMPLER STATE — the slot path does not

> **SUPERSEDED (2026-08-04) — see the Phase 3 narrative in
> `OloEngine/tests/Rendering/rhi_boundary_baseline.json`, the section beginning
> "LOCALISED, AND THE VERTEX-PATH DIAGNOSIS BELOW WAS WRONG".**
>
> What survives below is sound and worth reading: the sampler state IS derivable
> at the call site, and the measurement that the *correct* `Repeat` sampler made
> the number WORSE (5.861 → 10.447) is real — it is what killed the sampler
> hypothesis, and the reasoning about why a same-configuration test cancels a
> systematic difference still holds.
>
> Three claims in it are RETRACTED:
>
> - **"A residual ~5.861 … Still unexplained."** It is explained. The
>   depth-invariance contract group had split across compile routes: converting
>   `PBR_MultiLight` moved it to the raw-GLSL route while `DepthPrepass.glsl` —
>   which declares no samplers and so never mentions `OLO_BINDLESS` — stayed on
>   the SPIR-V one. `invariant gl_Position` cannot bridge two front-ends, so the
>   colour pass failed `GL_LEQUAL` against depth its own prepass wrote. See
>   glsl-shaders §7a-bis.
> - **"`PBR_MultiLight` stays slot-based."** It is converted and shipped, along
>   with `PBR_MultiLight_Skinned`, `PBR_GBuffer{,_Skinned}` and
>   `VirtualMeshGBuffer` — the whole material bucket.
> - **"the plumbing sits inert."** It is live and exercised by the suite in both
>   configurations.
>
> **AND ONE CLAIM IS REINSTATED (2026-08-07).** The heading is not just sound in
> principle — the mismatch it warns about was REAL and shipping, in the
> `SamplerDesc` DEFAULT rather than at any call site. Converting `Water.glsl`
> collapsed its tiled FFT displacement field into flat terraces: 22.670 RMSE
> ON-vs-OFF against a **0.000** same-config floor, down to 2.783 once fixed.
>
> **DO NOT READ THE FIX OUT OF THAT SENTENCE.** The first attempt was to make
> `Repeat` the struct default, matching `OpenGLTexture2D`; it produced the 2.783
> and BROKE the terrain arrays and cubemaps, which are `CLAMP_TO_EDGE`. What
> shipped is `SamplerDesc{}` meaning *inherit the texture object's state*, minted
> with `glGetTextureHandleARB`. Restoring a per-target default here would
> reintroduce the array regression. See §4f and ADR 0011 amendment (38).
>
> So the retraction below applies to the DIAGNOSIS of the rebase pop, not to the
> hazard. The experiment that "killed the sampler hypothesis" — a `Repeat`
> sampler making the number worse — was measuring a test that cannot see a
> systematic difference, which is exactly what the paragraph after it says.

This is the one thing that stops the per-material conversion today, and it is a
property of the model rather than a bug in the plumbing.

`glGetTextureSamplerHandleARB` folds sampler state **into the handle**. A
slot-based bind does not: `glBindTextureUnit` uses whatever wrap/filter/anisotropy
the *texture object* carries. So a converted shader samples with the state its
DESCRIPTOR was minted with, and an unconverted one with the state the OBJECT
carries — and if those disagree, the two variants render differently while both
look entirely plausible.

`RHI::SamplerDesc{}` defaults to `AddressU/V/W = ClampToEdge` and
`MaxAnisotropy = 1.0`. That is right for a render target — which is why every
pass conversion so far has passed `{}` and seen no difference — and wrong for a
**material** texture: every `OpenGLTexture2D` path sets `GL_REPEAT`, while
`OpenGLTextureCubemap` sets `GL_CLAMP_TO_EDGE`. Anisotropy is never set on a
texture object at all, so the default already matches.

So the state IS derivable at the call site (it follows the texture TYPE, not the
individual material) and does **not** need to travel in `PODMaterialData` — an
earlier version of this section claimed it did.

**BUT FIXING IT DID NOT FIX THE DIVERGENCE, and that is the point worth keeping.**
Measured on `WorldOriginRebaseVisualEvidenceTest`, which compares a frame before
and after a floating-origin shift:

| configuration | RMSE |
|---|---|
| heap OFF | **1.413** |
| heap ON, C++ plumbing live, shader NOT converted | **1.413** |
| heap ON, converted, default (`ClampToEdge`) sampler | **5.861** |
| heap ON, converted, correct (`Repeat`) sampler | **10.447** |
| heap ON, converted, correct sampler, material UBO cache **disabled** | **5.861** |

Two independent defects, and a hypothesis killed:

1. **The material UBO cache is stale-prone.** Disabling it takes 10.447 back to
   5.861, so roughly 4.5 of the divergence is a cached upload outliving what it
   describes. The slot path cannot have this bug: `BindTrackedTexture` re-reads
   `mat.albedoMapID` every draw, whereas a cached offset is computed once and the
   bind-skip means nothing re-checks it.
2. **A residual ~5.861 that sampler state does not touch.** Still unexplained.

**The sampler hypothesis was wrong, and the shape of the error is instructive.**
This test compares two frames in the SAME configuration, so any *systematic*
difference — including a wrong sampler — affects both frames equally and cancels.
It can only detect something that changes ACROSS the rebase. Attributing a
temporal-consistency failure to a systematic cause was a category error, and the
measurement that exposed it (correct sampler made it *worse*, not better) is the
kind only an A/B can give you.

So the per-material path is built, proven to compile and render, and **not
landed**: `PBR_MultiLight` stays slot-based and the plumbing sits inert behind
`WritesOffsetsForBoundProgram()`. What remains is to find what changes across a
rebase — start with descriptor lifetime against the rebase's own resource
churn, since `Persistent` views plus a cached offset is precisely the
combination the slot path never has.

### Every texture type that mints an identity owes a retire

`~OpenGLTexture2DArray` and `~OpenGLTexture3D` called
`m_RHIHandle.Sync(ResourceKind::Texture, …)` in their constructors and retired
nothing in their destructors, through two PRs and a full suite run. Both are bound
as **storage-image** descriptors in production — the wind field, the froxel fog
scatter/integrated volumes, the cloud noise volumes, the ocean FFT ping-pong — so
each destruction left an `ARB_bindless_texture` image handle resident on a texture
the next statement queued for deletion.

Nothing caught it because the symptom is one `GL_INVALID_OPERATION: Not a valid
texture` at shutdown, under `OLO_RHI_BINDLESS=1` only, which is not what CI runs.
The two calls now live in one place — `Utils::RetireTextureViews`
(`Platform/OpenGL/OpenGLUtilities.h`) — and it is `noexcept` deliberately: a
destructor is implicitly `noexcept`, `RetireResource` takes the heap's mutex and
touches containers, and a throw there turns a recoverable descriptor leak into a
process kill during teardown. `~OpenGLTexture2D` and `~OpenGLTextureCubemap` had
that hazard too and now share the guard.

Guarded from both ends: `BindlessHeapGpuTest` destroys a real `Texture3D` and
`Texture2DArray` and asserts residency drops, and
`RHIBoundaryRatchet.EveryTextureTypeThatMintsAnIdentityRetiresItsViews` scans
`Platform/OpenGL/` so the *next* sibling type cannot repeat it.

Two things that test taught, both worth keeping:

- **A storage view has its own residency namespace.** `glGetImageHandleARB`
  counts into `ResidentImageHandles`, not `ResidentHandles`. Asserting the sampler
  counter reads 0 both before *and* after the destructor — the test passes whether
  or not the retire happens. Assert the **precondition** (`== 1`), not just the
  postcondition; that is what caught it.
- **The invariant is "you retire what you minted", not "you call this helper".**
  `OpenGLFramebuffer` mints texture identities too and retires them correctly in a
  loop over N attachment handles — a different shape, already correct. A guard
  demanding the helper would have been demanding a refactor rather than enforcing
  a property.

## 4f. Closing bucket 1: the counter measured one spelling, and the blocker was four lines

Three things came out of converting the remaining 36 shaders, and each is the
kind that only shows up when you try to finish rather than to sample.

### The bind-site counter is blind to `Texture::Bind(slot)`

`sweep_bind_texture_sites` counts `BindTexture(` / `BindImageTexture(` — the
`RendererAPI` facade's spelling. `Texture::Bind(slot)` is the same act through the
resource object, and no text rule can catch it without also catching
`SoundGraph`'s `ref.Bind(...)` and `Templates/Function.h`'s `Storage.Bind(...)`,
which are unrelated methods that happen to share a name.

That blind spot is not cosmetic: **seven shaders looked unconvertible purely
because of it.** `IBLPrecompute`, `AssetPreviewRenderer`, `ImpostorBaker` and
`FoliageRenderer` bound their inputs that way, so the seam never saw them and a
converted shader would have read an offset nobody staged — a black frame with no
diagnostic. Seventeen such sites moved onto the seam here.

The eight that remain are deliberate, and three are **load-bearing**: `ShadowMap`
(×2), `WindSystem` and `SnowAccumulationSystem` are the "a direct `Texture::Bind()`
that never consults the seam" mechanism the §5c allowlist depends on, because a
shared `include/` header's declaration cannot be converted per-includer.

**No counter was added.** A ratchet whose rule is "the identifiers I could think
of" is precisely the false confidence §1.3's survey exists to prevent. The
artefact that replaces it measures the property instead of a proxy for it — see
below.

### Two orderings that decide whether a conversion works at all

Both are per-call-site and neither is visible in the shader:

- **The seam forks on the program in flight.** `BindTextureOrOffset` asks
  `Shader::IsBoundProgramBindless()`, so a bind issued *before* `shader->Bind()`
  always takes the fallback. `IBLPrecompute` binds its environment cube before the
  shader every time, which is why those five sites became
  `PublishTextureOffsetAndBind` (stage **and** bind) rather than the forking form.
  Reach for the publish whenever the consuming program is not bound yet.
- **A staged offset is CPU-side until the flush.** Every draw in
  `CommandDispatch` now publishes unconditionally, because *which* handler owes a
  flush is a property of the shader it dispatches — and that changes as shaders
  convert. Pairing them per site is a rule that has to be re-derived on every
  `.glsl` edit, and it fails quietly: the pass keeps rendering and reads the
  previous flush's offsets.

### The cost argument that blocked everything was measured on a different case

ADR 0011 amendment (32) declined the per-draw paths because a converted shader
"needs a flush per draw, which gives back exactly the cost bindless exists to
remove." It was written about *materials*, where per-material offsets in the
material UBO were and remain the right answer — then reused as a general reason,
and nothing re-checked it.

The cost it names is the table **upload**, and `HeapBinding::StageOffset` was
marking the table dirty even when writing the value already there. Consecutive
draws in a bucket overwhelmingly restage the same offsets (the same terrain
textures across every patch), so one `if` turns those flushes into a bool test.
The redundant-bind cache cannot suppress the writes — amendment (32)'s own second
finding is that it must not — so the stage is the only place the distinction can
be made.

Generalisable: **when a cost argument starts blocking more than it was measured
on, re-derive it.** This one cost roughly two thirds of the shader tree.

### `SamplerDesc{}` means INHERIT, and getting there took two goes

A bindless descriptor bakes the sampler in; the slot path samples with the
TEXTURE OBJECT's parameters. `RHI::SamplerDesc{}` therefore decides what a
converted shader sees while an unconverted reader of the same texture keeps
seeing the object's state.

The first fix changed the struct default from `ClampToEdge` to `Repeat`, matching
`OpenGLTexture2D` and GL's own default. It fixed `Water.glsl` — whose TILED FFT
displacement field had collapsed into flat terraces — and **broke the terrain
arrays**, because `OpenGLTexture2DArray` is `CLAMP_TO_EDGE`. There is no majority
answer to default to:

| target | what the object carries |
|---|---|
| `Texture2D`, framebuffer attachments | `REPEAT` |
| `Texture2DArray` (colour) | `CLAMP_TO_EDGE` |
| `Texture2DArray` (depth) | `CLAMP_TO_BORDER`, opaque-white |
| `TextureCubemap` | `CLAMP_TO_EDGE` |
| `Texture3D` | caller-supplied |
| **any integer format** | **`NEAREST`, mandatory** |

The last row is the sharp one. GL makes an integer texture with a `LINEAR` filter
INCOMPLETE, and an incomplete texture samples as **zero** — `texelFetch`
included, on Mesa but not NVIDIA. `Texture.h::IsIntegerFormat` records what that
cost once already (every Slug glyph vanished on AMD, logs clean); the heap had
reintroduced it for the `RG16UI` font band texture, the `R16UI` GTAO Hilbert LUT
and the `R32I` entity buffer. A sixth face: `LinearMipFilter` defaults true, which
makes a SINGLE-LEVEL texture incomplete — the hazard `ShadowDepthSampler` works
around by hand.

**So the answer is not a better table, it is not having one.** No stated intent ->
`glGetTextureHandleARB`, which bakes the object's own state and is parity by
construction. Stated intent -> `glGetTextureSamplerHandleARB`, which models a
split heap and is what the two sites that genuinely differ from their texture use.
`HeapBinding::CubeSampler()`, invented during the first attempt, was deleted.

**`SamplerDesc::Source` carries which it is**, so "inherit" and "these exact
values" stay distinct when the values coincide. And a desc that sets fields but
forgets `Source = Explicit` still has its fields honoured — making `Source` the
sole test would mean one forgotten line silently swaps a caller's sampler for the
texture's state, which deleting it from `ShadowDepthSampler` proved reachable
(136 tests passed with the shadow comparison sampler inheriting).

**The debt this creates is counted, not hidden.** Vulkan has no inherit — a
`VkSampler` must be described — so every default-desc site is Phase 4 work:
**199 of 209** seam call sites, with `Stats::DefaultSamplerInherits` counting them
at runtime.

**§4e retired the LIVE A/B; this is the instrument that replaced it.** The suite's
fixed-camera evidence PNGs are bit-identical across consecutive same-config runs —
a noise floor of exactly 0.000 — so the cross-config number means something:

| | RMSE | pixels >8 |
|---|---|---|
| noise floor (ON, two consecutive runs) | **0.000** | **0.000%** |
| OFF vs ON, before | 22.670 | 39.93% |
| OFF vs ON, after | 2.783 | 1.76% |

The residual was characterised, not waved off: amplified 6x it is hairline
contours around the FOAM boundaries, max 58 with nothing over 64 — a `smoothstep`
crossing differently because the two compile routes round the last bit
differently. §7a-bis's phenomenon on a threshold instead of on depth.

Four things generalise:

- **When a neutral struct's default has to agree with a backend, ask the backend
  rather than guess well.** Six rows, no majority.
- **A fix that turns one bug into another is telling you about the SHAPE**, not
  the value. `ClampToEdge` -> `Repeat` moved the failure from 2D to arrays.
- **A green suite is not evidence about pixels.** Every image above came from a
  passing test, in both configurations, before and after.
- **A zero floor is what makes a small number readable.** 2.783 would be invisible
  against the live scene's floor of 9.74.

### "Done" needs an end condition, and it has to be a test

A shader left slot-based renders correctly, so it is indistinguishable from one
nobody reached — which is why "39 remaining" survived three review rounds without
ever meaning anything. `BindlessShaderPipeline.EveryShaderIsOnTheRouteOrExplicitlyExcluded`
now fails on any sampler-declaring shader that is neither converted nor carries a
recorded reason, and checks its exception table in **both** directions so the
table cannot rot into a silencer. The four surviving reasons are tabulated in
glsl-shaders.md §5e; they are genuinely different from each other, which is why a
single "not yet" bucket would have been worse than none.

---

## 5. `ResourceTransition` looked backend-neutral by accident — FIXED in Phase 5

**Status: the defect below was fixed in Phase 5 (2026-08-08); kept because the
lesson generalises.** `ResourceTransition` now carries the unified
`RHI::Access FromAccess/ToAccess` pair, `PlannedBarrier` captures the
consumer's access AT EMISSION (a read for RAW, a WRITE for WAW — the lossy
read-declaration rescan is deleted, not patched), `"external"` producers carry
`Access::Undefined`, and `ReadWhileAttached` marks same-pass feedback reads.
The two-currency contract is literal in the facade:
`RendererAPI::IssueBarrierBatch(MemoryBarrierFlags, span<RHI::Barrier>)` — GL
executes the flags (byte-identical to the old `glMemoryBarrier` path), Vulkan
lowers the barrier span (`Platform/Vulkan/VulkanBarrierLowering` +
`VulkanImageLayoutTracker`), and neither backend may consult both. See ADR
0011 amendments (43)–(48).

The original finding, for the pattern: the transition was typed
`RGWriteUsage FromUsage` → `RGReadUsage ToUsage`, so it **structurally could
not express a write → write transition.** The planner *did* emit WAW barriers;
`BuildResourceTransitions` then looked for a matching **read** declaration on
the consumer, found none, and silently defaulted to `ShaderSample`. On GL
invisible (only `Flags` was executed); on Vulkan it would have lowered a
storage-image WAW into `SHADER_READ_ONLY_OPTIMAL` with a read-only access mask
— wrong layout, wrong access, silent.

**Generalisable lesson: a record is not backend-neutral just because its field
types are.** It was held up by a GL-specific field sitting next to it. When
auditing an abstraction for a second backend, ask what each field is *actually
read for today* — a field nothing reads is a field nothing keeps correct. And
when the derivation of a field is lossy, fix the PRODUCER to carry the fact
rather than the consumer to guess better — the guess was the bug.

Layout stays out of the neutral model, as designed — but it is a function of
**(Access, aspect, read-while-attached)**, not of access alone (ADR 0011
§1.5's three exception cases), and the barrier's `oldLayout` comes from the
backend's layout TRACKER, never from `FromAccess`: a pooled transient is in
whatever layout its previous tenant left, and first use is UNDEFINED.

---

## 6. The shader pipeline is a genuine head start — and has exactly one trap

Every shader already compiles GLSL → SPIR-V via shaderc targeting Vulkan
(`Platform/OpenGL/OpenGLShader.cpp`, `CompileOrGetVulkanBinaries`), then
cross-compiles *back* to GLSL via SPIRV-Cross purely to run on GL. Vulkan
consumes the SPIR-V directly and **deletes** the second hop. Three on-disk cache
tiers exist today, all under `Utils::GetCacheDirectory()`:
`.cached_vulkan.<stage>` (SPIR-V), `.cached_opengl.<stage>` (cross-compiled
GLSL), `.cached_opengl.pgr` (linked program binary, driver-stamped).

**The trap: tier 1 is a shared artefact.** `shaderc_env_version_vulkan_1_2` is
hard-coded, and a `VK_EXT_descriptor_heap` backend needs a newer target env.
Bumping it in place silently changes the SPIR-V the *OpenGL* path cross-compiles
— putting the GL backend's entire shader pipeline, and the SPIRV-Cross round
trip that `tests/Fuzzing/FuzzSpirvCross.cpp` pins as production behaviour, at
risk for a Vulkan-only reason. **Encode the target env in the SPIR-V cache
filename** so the two consumers are independent. Cheap now, very awkward later.

### Hot reload has a different invalidation granularity per backend

This is the constraint that shapes Phase 6, and it is not obvious from the GL
code:

- **GL:** blend / depth / raster / stencil / vertex layout are all dynamic
  state. `OpenGLShader::Reload()` recompiles and `FinalizeProgram` assigns a
  fresh `glCreateProgram()` handle to `m_RendererID` on the same `Shader`
  object. One shader → **one** new program object. Consumers hold `Ref<Shader>`
  and re-read the id on `Bind()`, so nothing needs re-pointing.
- **Vulkan:** all of that state is **baked into the `VkPipeline`** alongside the
  SPIR-V, including attachment formats and sample count. One shader is the
  source of **N** pipelines, one per state permutation it has been used with.

So `Shader::Reload()` cannot stay a per-object operation. The Vulkan backend
needs a `shader → {pipeline}` reverse index maintained at pipeline-creation
time. Recreate **lazily** (on next bind), not eagerly: eager recreation of every
permutation on every file save turns the iteration loop hot-reload exists to
serve into a multi-second stall, and the `VkPipelineCache` makes the lazy
recreation cheap anyway. Destruction must go through the deferred-reclaim
machinery (`VulkanDeferredReclaim` — ADR 0011 amendment (52), which also
corrected this section's original stale name for it), since a replaced
pipeline may still be referenced by in-flight command buffers.

### One GL workaround that does *not* transfer

`SyncProgramBinaryCacheDriverStamp` + `program_binary_driver_stamp.txt` exist
because `glProgramBinary`'s rejection behaviour is vendor-dependent and, on
Mesa/radeonsi, crash-prone (there are two explicit workarounds in
`OpenGLShader.cpp` for it). Vulkan does not need the equivalent: a
`VkPipelineCache` blob carries `VkPipelineCacheHeaderVersionOne` with vendor ID,
device ID and `pipelineCacheUUID`, and the driver is *required* to ignore a blob
it does not recognise. Keep the soft-fail posture (a bad cache costs compile
time, never correctness); drop the mechanism.

---

## 7. Backend selection: startup-time, not live — and the ordering contract is already load-bearing

"Most engines swap backends interactively" is *almost* right and worth stating
precisely, because the difference decides the design. Unreal takes `-vulkan` /
`-d3d12` at launch and its project setting needs a restart; Unity restarts the
editor when you reorder the graphics-API list; Godot takes
`--rendering-driver vulkan|opengl3`. None hot-swap a live device. What reads as
"interactive" is **choosable without recompiling, applied on restart** — a
runtime switch, just not a live one.

**Phase 4 built this** (2026-08-07): `RendererAPI::SetAPI` exists and is called
from `Application`'s constructor via `SelectRendererBackend`
(`Renderer/BackendSelection.{h,cpp}`: `--rhi=` flag → `config/renderer.yaml` →
OpenGL). One correction to the paragraph below — "never written by anything"
was true, but "nothing reads `GetAPI()` during static init" was NOT:
`RenderCommand::s_RendererAPI = RendererAPI::Create()` runs at static init and
switches on `s_API`, so the constructed backend is always the default OpenGL
one regardless of the flag. Harmless while Phase 4 never routes `RenderCommand`
under Vulkan; Phase 5 must re-create `s_RendererAPI` after selection (ADR 0011
amendment (39)).

`RendererAPI::s_API` was, before Phase 4, a static initialised to `OpenGL` and
never written by anything. Giving it a real setter comes with a hard ordering
requirement:

> `RendererAPI::s_API` must be set **before `Window::Create`**.

This is not hypothetical — the seam already exists and is already used:
`WindowsWindow::Init` calls `Renderer::GetAPI()` before `glfwCreateWindow`
(client-API hint + `GLFW_OPENGL_DEBUG_CONTEXT`), and `Application`'s
constructor calls `Window::Create` well before `Renderer::Init`. That is where
the setter lives: `SelectRendererBackend` resolves the chain and
`RendererAPI::SetAPI` applies it in `Application`'s constructor, immediately
after the working-directory switch (so the config fallback resolves against the
real cwd) and before `Window::Create`. Anything reading `Renderer::GetAPI()`
during static initialisation sees the constant-initialised `OpenGL` default
regardless of the flag — and one thing DOES read it there:
`RenderCommand::s_RendererAPI = RendererAPI::Create()` (see the Phase 4
correction above). Nothing else should join it.

Why a live swap is genuinely out of reach here, not just unimplemented:
`GLFW_CLIENT_API` must be set before `glfwCreateWindow` (so a swap destroys the
window, the ImGui context and the docking layout); every GPU resource is
backend-owned and many — transient targets, HZB pyramids, DDGI atlases,
virtual-geometry pages, FFT ocean cascades — have no retained CPU-side source to
recreate them from; and the editor's ImGui backend is `imgui_impl_opengl3`
including its font atlas.

Also note the separate axis: `OLO_WITH_VULKAN` (build-time, "is the backend
compiled in") is **not** the selection mechanism. Both backends ship in one
binary so a machine below Vulkan's hardware floor (ADR 0010: Intel ANV is still
experimental) can fall back at runtime.

`OLO_WITH_VULKAN=OFF` does **not** mean "no Vulkan SDK". That claim was wrong
from the start and #811 removed it here and from ADR 0011: OFF drops the volk /
vulkan-headers / VulkanMemoryAllocator ports and compiles every
`Platform/Vulkan` TU to an empty object, but `find_package(Vulkan REQUIRED ...)`
in `OloEngine/CMakeLists.txt` is deliberately **ungated** — the SDK is also
where the *shader* toolchain lives (shaderc, glslang, SPIRV-Tools,
SPIRV-Cross), which every build needs regardless of backend. The valve is about
link weight and port count.

It is also **whole-tree**, not per-target: `OloServer` is configured from the
same tree as the editor and the runtime, so "OFF for the server" is not a thing
a single configure can express. #811 decided explicitly that a lean, SDK-free
server build is **not** a goal — the server is backend-less anyway (ADR 0011
amendment (86)) and its deployment image (ubuntu + libstdc++6, no libGL, no
libvulkan) needs no GPU stack — so the capability was deleted from the docs
rather than built.

**If you touch a `#if OLO_WITH_VULKAN` guard, the OFF path has a CI job now:**
`.github/workflows/vulkan-off.yml` configures and builds the GL-only
configuration on Linux and asserts the seven Vulkan suites SKIP while
`BackendSelection`'s `#else` arms pass. It runs on every PR that touches the
paths where OFF-only breakage can live — the option plumbing (the root /
`OloEngine/` / `OloEngine/vendor/` `CMakeLists.txt`, `cmake/`,
`CMakePresets.json`, `vcpkg.json`), `Platform/Vulkan/`, `Renderer/RHI/`, the
Vulkan tests, and the two ON/OFF *contract* files (`ShaderBindingLayout.h` and
`VirtualShadowMapLocalTest.cpp`) — and unfiltered on master and weekly. Before
#811 nothing set the option anywhere in CI, and the guards had gone
uncompiled — and the SKIP branches unexecuted — for the whole epic.

**The filter is a heuristic, and here is its known hole:** it names two contract
files because those are the ones that broke, but *any* GL-only TU that acquires
an include of a `Platform/Vulkan` header inherits the same failure and would
slip past on the PR that introduced it (master/weekly still catch it, one merge
later). If that recurs, the right answer is not a longer path list but a cheap
grep job in the shape of `steam-stub.yml`'s `single-valve-tu` — assert that no
TU outside `Platform/Vulkan/` and the Vulkan tests includes a
`Platform/Vulkan/` header — which runs on every PR in seconds.

---

## 8. Writing a ratchet test that cannot silently pass

Every assertion in a "count must not rise" test is an **upper bound**, so a
wrong repo root, a broken literal-stripper, or a moved directory reports zero
for everything and the test passes forever while measuring nothing. That is a
false-green that could survive an entire phase.

`RHIBoundaryRatchetTest` guards against it with **floor** assertions on values
that must be large: the OpenGL backend itself must contain > 100 GL calls
(actual: 497) and the walk must see > 500 files (actual: 1417). Both thresholds
sit far below the real numbers so ordinary progress never trips them — they
detect a broken harness, not a changed codebase. It also pins the counting rule
directly with a table of synthetic inputs (comment, string, raw string, `glfw`,
`glm`, identifier-suffix), so the rule the baseline numbers are relative to
cannot drift unnoticed.

The test also **fails when a counter improves**, printing the exact JSON line to
paste. That is deliberate: a baseline that does not follow progress down lets a
later change re-add everything a sweep removed. Generalisable to any ratchet —
if the baseline is not updated in the same commit as the improvement, the
ratchet degrades into a very slow-moving upper bound.

### A guard that bans a token cannot also ban documenting the ban

`RHIBoundaryRatchetTest` asserts that nothing under `Renderer/RHI/` names a
backend type. The first version checked the **raw** file text, on the reasoning
that "a backend type mentioned even in a comment is a smell here." It failed
immediately — on the RHI headers themselves, whose comments say `GLenum` and
`VkImage` precisely in order to explain that they are forbidden.

The property actually worth enforcing is that no backend type is *used*, so the
check runs on the comment/string-blanked text. Includes are a separate case and
go through the `#include` scan rather than a plain substring search, so that
naming `glad/gl.h` in prose is fine while including it is not (and a quoted
`#include "glad/gl.h"` is still caught, despite its path being blanked as a
string literal).

Generalisable: whenever a test forbids a *spelling*, check the text after
stripping the places where documentation legitimately lives — otherwise the
guard's own rationale becomes unwritable, and the usual "fix" is to delete the
explanation, which is exactly backwards.

### The digit-separator trap in any C++ literal-stripper

A `'` is not always a char literal. C++14 digit separators (`1'000'000.0`) look
identical to a scanner, and the failure is asymmetric: `1'000` contains an **odd**
number of quotes, so a naive scanner opens a literal it never closes and blanks
forward to the end of the line. `if (x == 1'000) glFinish();` then counts as
**zero** GL calls — a silent *undercount*, which for a ratchet is the worst
possible direction to be wrong in, because it looks like progress.

There are 77 digit separators under `OloEngine/src`, four of them in
`Renderer/Debug/` files that do call GL. The current numbers happened not to be
affected (all four sit on lines with no GL call, and each has an even count), so
this was latent rather than active — but the next edit could have activated it
silently.

The fix is one predicate: **`'` opens a char literal only when the preceding
character is not alphanumeric.** A digit separator is always preceded by a digit;
a real char literal is preceded by `(`, `=`, `,`, whitespace, `return`, … The
rule also declines to treat the quote in `u8'x'` / `L'\n'` as an opener, which is
harmless — a char literal cannot contain a GL call either way.

---

## 9. Phase 7 (the pass suite): where the boundary actually held, and the five ways it leaked

Phase 7 ported every render pass to Vulkan behind the facade. The abstraction
mostly worked — pass bodies moved **unmodified**, which is the whole claim §1
made. What follows are the five categories that did leak, because each one is a
place the boundary looked clean and was not.

### 9a. A factory in a `Platform/<Backend>/` TU is a leak the include scan cannot see

`Texture3D::Create` and `Texture2DArray::Create` lived in
`Platform/OpenGL/OpenGLTexture3D.cpp` / `OpenGLTexture2DArray.cpp` and
unconditionally constructed the GL class. With one backend that is invisible;
with two it is a **null `glad` function pointer** in any process that never
created a GL context, i.e. an access violation with no diagnostic. The engine
header declared a neutral interface, every call site was neutral, and the
`glXxx(` counters were zero — the leak was the *definition site of the
factory*, which no counter in §8 measures.

The same shape caught `GLStateGuard`: its constructor queried GL state
unconditionally, so the first guard-carrying pass body to execute on Vulkan
faulted. Add both to the mental scan: **"constructs a backend object" and
"queries backend state" are leaks even when the file names no `GLenum`.** A
factory belongs in a neutral TU with an explicit `switch (Renderer::GetAPI())`;
a process-wide guard must be inert off its backend.

### 9b. The facade's *semantics* are wider than its signatures

`DrawIndexed(va, indexCount)` has an unwritten contract — `0` means "the whole
index buffer" — that lives only in the GL implementation, and **every pass body
relies on it**. The Vulkan arm passed `0` through to `vkCmdDrawIndexed`: a
legal draw of nothing, no error, no warning, nothing rendered.

Before declaring an entry point ported, enumerate its **sentinels**: `0` = all,
`RHI::NoAttachment` = "this slot is unused", a null handle = "unbind",
`SamplerDesc{}` = inherit (§4f already learned that one the hard way). A
signature match proves nothing about them.

### 9c. GL's separate binding namespaces are a boundary assumption

The neutral model gives a binding a *number*. GL gives uniform blocks, storage
blocks, samplers and image units four independent number spaces, and the engine
used that freedom: binding 57 was simultaneously a UBO, a sampler, and (after
§5's vertex pulling) the vertex-pull SSBO. One Vulkan descriptor set collapses
all four spaces.

The correct test is **per shader**, not global: a collision matters only where
two of the meanings meet inside one compiled shader. But the renumber is
cheapest before per-backend branches multiply, and reserved engine-wide
bindings should be documented as a set (this port reserved 57/63 as the
vertex-pull *pair* — stream 0 and stream 1 — because a VAO with two vertex
buffers has no other way to reach its second stream once vertex-input state is
gone).

### 9d. Implicit synchronisation is part of the GL API surface

Three things GL does silently that the neutral layer never described, each of
which the render graph does **not** emit because it reasonably assumed the
backend owned them:

- **Attachment layout transitions.** The graph's barrier planner never lowers a
  framebuffer-kind write to an image barrier; the draw front-end must
  transition attachments at scope open.
- **Mid-pass visibility.** Sampling an attachment the same `Execute` just
  rendered (or an image it just copied) needs a barrier and a layout change
  that no GL-shaped pass body contains. Bind time is the seam.
- **Per-attachment aspect.** The graph fans one framebuffer transition into
  per-attachment barriers reusing a single access prototype — depth included —
  with the documented contract that *the backend derives each attachment's
  aspect and layout from its own image*. A lowering that honours the enum but
  not the aspect emits colour layouts on depth images.

The general rule: when a neutral structure documents "the backend derives X",
that sentence is a **requirement on the backend**, and the first backend that
did not need to derive it will have left the code path untested.

### 9e. Device capability is not one contract but a growing list

ADR 0010's capability gate covers the extension floor. The *core feature bits*
are separate, and each one surfaced only when a particular shader family
reached pipeline creation: demote-to-helper-invocation (any `discard`),
vertex-stage stores (debug-draw channels), fragment-stage stores (virtual
geometry's debug images), independent blend (**the entire per-attachment blend
facade is undefined without it** — enabled late, retroactively fixing already
"green" WB-OIT work), multi-draw-indirect, draw-indirect-count, draw
parameters, tessellation.

Enable from `vkGetPhysicalDeviceFeatures2`'s answer *when supported*, never
from a hand-maintained constant, and expect the list to grow with every shader
family a port reaches.

### 9f. What the port surfaced in the GL path itself

A backend port is an audit. Three defects found were **pre-existing GL bugs**,
not Vulkan problems:

- Normal-mode decals write nothing on either backend — the shader emits at
  `location = 0` while the mode's draw-attachment map targets attachment 1.
  (Vulkan's validation layer states it verbatim; GL says nothing.)
- No `R32F` colour `FramebufferTextureFormat` exists, so the graph's format
  mapping answers `None` and the upscaled depth/velocity target silently loses
  its first attachment on the FSR1 path.
- A GTAO history/AO target survives a resolution-band resize without
  invalidation and can latch a zero attractor.

Expect this. The second backend is the first reader of contracts the first
backend never had to state.

---

## 10. Phase 4–6 bring-up traps (from ADR 0011 amendments (41), (41a), (51), (53))

Four postmortems from device bring-up. The decisions live in the ADR at the
amendment numbers above; what follows is the part a future bring-up would
otherwise rediscover with a debugger.

### Vendored headers must out-rank the installed SDK's — three traps, all hit

The engine already carries an SDK include dir on every TU's path — the shaderc
toolchain's `find_package(Vulkan)` — so vendoring Vulkan-Headers 1.4.357 pins
the backend's compile only if the vendored dir reliably *wins the include
search*. Three defaults vote against it, and Phase 4 hit all three:

- **volk's `VOLK_PULL_IN_VULKAN` (default ON) prefers `find_package(Vulkan)` —
  the installed SDK — over an existing vendored `Vulkan-Headers` target.** On a
  machine with an older SDK, volk and everything including `volk.h` silently
  compiles against that SDK's headers. It is OFF in `vendor/CMakeLists.txt`,
  with the vendored `Vulkan::Headers` linked explicitly instead.
- **Link-propagated INTERFACE include dirs come AFTER the target's own.** The
  vendored dir arriving "via the volk link" loses the `/I` order to
  `${Vulkan_INCLUDE_DIRS}`, which sits in `OloEngine`'s own PUBLIC include
  list — so the SDK's `vulkan_core.h` still won. Both are plain `/I` paths;
  only order decides. The fix is explicit: the vendored include dir is listed
  in that same list, **before** `${Vulkan_INCLUDE_DIRS}`.
- **`Vulkan::Headers` is an ALIAS resolved per directory scope.** The vendor
  scope resolves it to the vendored target; a later `find_package(Vulkan)` in
  another directory can mint its own, SDK-pointing `Vulkan::Headers` in *its*
  scope. Engine code links `volk`/`vma` and never `Vulkan::Headers` by name.

The backstop for all three is `static_assert(VK_HEADER_VERSION >= 357)` in
`VulkanContext.cpp` — if the SDK's headers ever win the search again, the
build fails naming the mechanism instead of failing to declare an extension
struct.

### `vulkan-1.lib` and the `/FORCE:MULTIPLE` thunk replacement — the diagnosis recipe

The engine had linked `Vulkan::Vulkan` (vulkan-1.lib) since the shaderc
toolchain arrived, harmlessly: no engine code called a Vulkan function, so no
import-library member was ever pulled. Phase 4's first run crashed with an AV
one millisecond into instance creation, and every ingredient was pre-existing
and individually reasonable:

- volk declares the core entry points as **data** (`PFN_vkCreateInstance
  vkCreateInstance`) and populates them at `volkInitialize`; an import library
  declares the same names as **function thunks** (`jmp [__imp_…]`).
- The repo links with **`/FORCE:MULTIPLE`** (the assimp/MaterialX pugixml
  collision workaround), so the duplicate did not fail the link — the linker
  silently picked the import thunk for the backend's data references.
- A load through that "pointer" reads the thunk's *instruction bytes* and
  calls the result: instant `0xc0000005`, with a useless stack.

The diagnosis that worked, in order: a standalone volk repro compiled clean
and ran clean (→ not the loader/layers/headers); `dumpbin /imports` on the
test exe showed **exactly the backend's own call list imported from
vulkan-1.dll** (→ the references resolved to the import lib); `dumpbin
/symbols` on `VulkanContext.obj` showed the references correctly data-typed
(→ the substitution happened at link, not compile). Fix: `Vulkan::Vulkan`
removed from the link entirely — volk's own contract, the loader is dlopen'd —
while `find_package(Vulkan)` stays for the SDK include dir and the
shaderc/glslang libs.

The rule: **under `/FORCE:MULTIPLE`, a name collision between a data symbol
and an import thunk is not a link error — it is a runtime AV.** Every new
dependency that self-loads an API (volk-style) must audit the link line for
that API's import library.

### Fold promoted feature structs into the version aggregate

Enabling `timelineSemaphore` and `bufferDeviceAddress` added
`VkPhysicalDeviceVulkan12Features` to the device chain — and validation
immediately flagged Phase 5's standalone
`VkPhysicalDeviceShaderAtomicInt64Features`
(VUID-VkDeviceCreateInfo-pNext-02830: a feature promoted into an aggregate may
not be chained alongside it). `shaderBufferInt64Atomics` moved into the
aggregate. The rule: the FIRST time a `VkPhysicalDeviceVulkanXYFeatures`
struct joins the chain, sweep the chain for standalone structs of features
that version promoted, and fold them in. The same slice enabled
`dynamicRendering` (same defaults-OFF class as `synchronization2`) and gave
VMA `VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT` — without it,
`vkGetBufferDeviceAddress` on a VMA buffer is undefined.

### A reclaim queue nobody drains leaks with green tests

`VulkanDeferredReclaim::NotifyFrameCompleted()` had exactly one caller: the
test fixture. A live `--rhi=vulkan` session enqueued forever and leaked until
exit — with every reclaim test green, because the tests supplied the tick the
frame loop never did. It is now called at the ONE point that proves the GPU
finished a frame slot — immediately after `vkWaitForFences` in
`VulkanContext::SwapBuffers` — alongside `VulkanFrameArena::BeginFrame`,
whose cursor rewind is gated on the same proof; `FlushAll()` is wired at
context teardown, which nothing called either. The rule: a generation-counted
queue is only as real as its production tick. Same genre as §4b's "the GPU
test was supplying the missing call": when a test sets up a mechanism, ask
which of those setup lines production is also supposed to execute.

---

## 11. Phase 7 pass-suite lessons beyond the five leaks (from ADR 0011 amendments (61), (62), (66), (67), (68))

§9's five categories are where the boundary leaked. These five are the rest of
what the pass-suite port taught — decision kernels in the ADR, the debugging
context here.

### Bare uniforms cannot enter SPIR-V, and the no-op setter makes the failure silent

GL compute never travelled the SPIR-V route (`glShaderSource` takes raw GLSL),
so compute shaders accumulated bare `uniform float x;` declarations fed by
`ComputeShader::SetFloat`. On Vulkan those declarations are illegal and
`VulkanComputeShader::Set*` is a deliberate no-op — the values read as zero,
silently. The migration is mechanical (pass-owned `std140` block, legal on
both routes since GL compute at 460 core takes UBO blocks); the *diagnosis* is
not, because nothing errors anywhere.

The amplifier to fear: ToneMap's auto-exposure writes into an SSBO that
**survives frames**, so a single NaN latches forever — exactly what the no-op
setters would have produced. And per-dispatch values (HZB's per-mip-batch
parameters, the denoiser's ping-pong axis) migrate the same way with no ring
machinery: a `SetData` per dispatch is a `glBufferSubData` on GL and a fresh
arena-versioned address on Vulkan, so both backends see per-dispatch values
without a race.

### The graph's own contracts bite first — three of them

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

### A descriptor mapping must name its resource KIND

`VkDescriptorSetAndBindingMappingEXT` entries carry a `resourceMask`. Emitting
`ALL` for every mapping works right up until one shader declares a sampler and
a storage image at the **same numeric binding** — two ALL-masked mappings at
one (set, binding) violate VUID-11244, `vkCreateComputePipelines` *fails*, and
the dispatch silently drops. Every earlier shader had disjoint numbers by
luck; the froxel-fog pair was the first to collide. Per-kind masks (UBO →
uniform buffer, SSBO → storage buffer, combined sampler → sampled image,
storage image → image) defuse the landmine for every later shader.

### Only a window can prove orientation and extent

Forty-five device-gated tenants rendered the pass suite correctly and could
not have caught either defect the first live frame showed, for one structural
reason: **a tenant reads its output back in the same order it wrote it**, so a
uniform vertical flip cancels exactly. The swapchain is the first asymmetric
consumer in the entire system.

The extent half is the subtler one. `FinalRenderPass` sized its viewport from
the *graph's* spec, which the editor shrinks to its viewport panel. On GL the
rest of the default framebuffer keeps its previous content and the editor's UI
covers it; a swapchain image is **undefined outside what the frame writes**,
so the same code presented the frame in a corner surrounded by garbage. The
acquired image's extent — not the graph's — is the authority for a backbuffer
draw.

Two more live-only findings, each needing an explicit latch and neither
reachable from a test: the engine **presents during startup** (shader-warmup
progress frames swap from inside renderer init, so a frame recorder driven off
the layer stack runs `OnUpdate` on layers that are still attaching); and a
nested present resets the command buffer the outer call is recording into.

### A diagnostic that cannot distinguish two states will misdiagnose one of them

`CommandDispatch` warned "material texture has no heap descriptor — it will
sample the reserved null and render BLACK". On Vulkan that message fires
whenever the heap path merely **is not live** — the deliberate design there,
since `VulkanShader::Bind` reports non-bindless and materials travel the slot
path. The warning cannot tell "the heap is off" from "acquisition failed", so
it confidently reports the wrong cause, and a reader follows it to the wrong
layer. The rule: a diagnostic that names a *cause* must be able to exclude the
other causes that produce the same symptom. When it cannot, name the
**symptom** and list the candidates.

---

## 12. Phase 8 editor-parity lessons (from ADR 0011 amendments (71), (72), (74), (75), (78), (81), (82), (83), (84))

The Phase 8 screenshot-parity gate — live editor scenes captured on both
backends — is what forced most of these out; no unit test did. Each ADR
amendment keeps the decision; this section keeps the hunt.

### A deferred clear must remember WHO asked

Phase 7's lazy-scope pending clear was a target-blind flag pair, which
diverged from GL three ways: `Bind(A); Clear(); Bind(B); Draw()` cleared B and
left A untouched; an unconsumed clear (a shadow-atlas entry culled to zero
draws) leaked into the next pass's loadOp — or onto the backbuffer through the
present path's forced rebind; and clear values were read at consume time, not
request time. The fixed shape: the pending record carries the SAME identity
triple the scope-match predicate uses (framebuffer pointer / backbuffer view /
depth-array view) plus captured values, folds into the loadOp only when the
opening scope IS the requester, and otherwise MATERIALIZES as an eager
transfer clear (per-layer for the depth-array case) — at supersession and at
EndRecording. Deferral is an optimisation over GL's eager semantics; every
path where it becomes observable must collapse back to eager.

### One-shot submits are ordered BEFORE the still-recording frame

The symptoms, worth recognising: `StorageBuffer::GetData` returned the
*previous* frame's contents (the blocking one-shot ran before everything the
frame had recorded but not submitted), `SubImage` diverged from the layout
tracker, and `ClearData`'s documented routing had never landed. The rules that
fixed it are in ADR amendment (72). One more from the same slice: the guard
for "is a Vulkan recording live" must probe the LIVE object via
`dynamic_cast` — never `RendererAPI::GetAPI()`, a static flag a fixture can
set without recreating the API object. Found as an access violation: amendment
(39)'s construction-order gap resurfacing in test clothing.

### A sweep keyed on the live log has a shadow

The Phase 7 bare-uniform sweep converted what the live session exercised and
NAMED six leftovers; the first Phase 8 live launch found a SEVENTH
(`ReflectionProbeCull.comp`) whose pass simply never ran in that log. Both
halves generalise: coverage claims derived from a live session inherit that
session's pass coverage; and a ratchet that is an INCLUSION list cannot see an
unlisted offender — fixing a shader does nothing until it is added to
`kPhase7MigratedComputeShaders`, `kKnownBlocks`, `IsKnownUBOBinding`, and the
binding-limit assert. The migration is mechanical; the REGISTRATION is the
part that rots.

### GL's cube-face z IS a Vulkan array layer

`CopyImageSubDataFull`'s `(level, z)` maps directly onto `(mip, arrayLayer)`
on the CUBE_COMPATIBLE image — which is the whole IBL/sky bake write path. The
subtle half is the layout contract: per-(mip,layer) tracking answers a
whole-image layout query with UNDEFINED when subresources disagree ("discard
is the safe total answer"), so a face-at-a-time upload that transitioned only
its own face would let the driver legally discard the faces already uploaded.
The face-upload family therefore runs each transition over the WHOLE image.
Mid-frame face uploads (the IBL cache load runs INSIDE the frame on this
backend) take amendment (72)'s write rule; face readbacks take its read rule.
And the heap descriptor path honours the image's recorded dimensionality for
sampled views — deriving from layer count alone handed a cube a `2D_ARRAY`
view against `samplerCube`.

### The null root address: a device-loss diagnosis recipe

Every VehiclesTest launch died at `vkQueueSubmit2` with
`VK_ERROR_DEVICE_LOST`, with nvlddmkm event-153 storms in the system log. The
notes for the next device loss: the fault storm **predates the reporting
submit** — loss is sticky and surfaces at the NEXT `vkQueueSubmit2`, so do not
read the reporting frame as the faulting one; GPU-AV hid the race entirely
(serialized submission); and the discriminator that localized it was flipping
`StartScene` — the fault followed the scene that re-runs the IBL bake, not the
build. The two root causes and their fixes are ADR amendment (78): a null
root-data address is a GPU page fault, not "reads as zeros" (the promise is
now real via the arena's persistent zero block); and both IBL bake sites held
their params UBO in a block scope that closed before the draw executed —
under GL the dangling bind read stale-but-valid data by accident of GL object
lifetime, on Vulkan the destructor un-publishes the binding and the draw reads
an unfed block.

### Unfed sampler bindings sampled the loft HDRI

Heap slot 0 is whatever texture registered first — in the editor, the
loft-HDRI equirect — so every unfed optional sampler
(`PostProcess_ToneMap`'s water-depth, `PBR_MultiLight`'s material maps behind
`u_Use*Map` flags) silently sampled a real image where the GL twin reads
deterministic black from an unbound unit. The fix threads the reflected
sampled-image dimensionality (spirv-cross `image.dim`/`arrayed` →
`VulkanShaderBinding::ImageDim`) to typed 1×1 zero-filled null images of the
matching view type — a 2D descriptor under a `samplerCube` is undefined. The
null images are owned by `VulkanDescriptorHeapBackend` beside its token nulls
and reclaimed with the heap: a process-leaked VMA image trips the
allocations-not-freed assert at device teardown (the sampler-heap lesson,
relearned). Storage-image units keep the slot-0 fallback — there is no safe
neutral WRITE target.

### The water-murk hunt ended at the tessellation domain origin

VehiclesTest's sea rendered as a flat grey gradient under Vulkan while every
input checked out one by one: the six "unfed" texture warnings were benign
(unassigned assets — GL skips them too), the water UBO and its per-draw time
arrived (live MCP A/B: WaveAmplitude 5 erupted identically on both backends),
tessellation levels matched (the GL control faceted the same), the underwater
fog contributed nothing (fog-off was pixel-identical), and the
planar-reflection mirror pass ran and produced real content (in-pass
centre-texel readback). The killer was one absent pipeline field:
`VkPipelineTessellationDomainOriginStateCreateInfo`.

Vulkan's default domain origin (UPPER_LEFT; GL's convention is LOWER_LEFT)
mirrors the tessellator's v coordinate for a GL-authored TES: the barycentric
weights become a corner permutation, so **generated positions stay on the
patch** while every emitted triangle's **winding flips**. On two-sided/uncull
water that meant right geometry, BACK face visible: `gl_FrontFacing`
inverted, the two-sided normal flip pointed the shading normal down, NdotV
collapsed the fresnel term — killing SSR, environment AND planar reflection
in one multiply — and pinned the view-depth blend to the deep colour. A murky
sea with zero validation errors and every binding fed. (A back-culled
tessellated surface would instead disappear outright.) The diagnostic lesson:
a winding-level bug on a two-sided surface presents as a *lighting/material*
bug, and every per-binding audit will come back clean — check the domain
origin before re-auditing the inputs.

### Two stacked shadow bugs, split by a live MCP A/B

The MaterialSpheres "exposure delta" (GL bright with specular hotspots, Vulkan
uniformly dim) was the directional light dying to TWO stacked shadow bugs. The
split that found them was a live MCP A/B: intensity ×10 changed nothing;
`CastShadows=false` restored the full light — so the light data was fine and
the CSM *sampling* returned fully-shadowed.

- **The identity gate.** `PopulateBlackboard` declared the CSM/atlas into the
  graph only when `GetCSMRendererID() != 0` — the native-GL-name currency, 0
  by contract on Vulkan. No Vulkan graph ever contained a shadow target (six
  on GL, zero in `olo_render_list_targets`), so the shadow pass declared no
  writes.
- **The stale framebuffer spec.** The scene bumps the CSM from the 1024
  startup default to 4096, but nothing resized the shadow pass's framebuffer.
  GL forgave it forever — a GL FBO's size IS its attachments', and `glClear`
  covers the whole attached 4096 layer. On Vulkan the framebuffer spec drives
  the rendering scope's render area, so every cascade cleared and rendered
  only its top-left 1024² quarter while sampling spanned the full layer —
  depth 0 outside it, every fragment fully shadowed. Found by capturing
  `ShadowMapCSMCascade0` on both backends: GL all-white, Vulkan black with one
  white quarter.

The genre note: both are **GL-tolerated latency** — states that were always
wrong (or wrong-shaped) under GL but harmless there, made load-bearing by a
backend where framebuffer size and resource identity are real contracts. The
parity gate, not any unit test, is what forces them out.

### 254k instances, one uploaded matrix, and `VK_EXT_device_fault`

The FoliageGenerationTest `VK_ERROR_DEVICE_LOST` was count-scaled — 254k grass
instances died ~2 s after the first foliage frame, 12k survived — and it was a
**pre-existing cross-backend OOB read**: `InstanceBlock_Vertex.glsl` resolves
`u_Model` to `instances[gl_InstanceIndex]` (the auto-batching contract, N
entries for N instances), but `FoliageRenderer::Render` uploads exactly ONE
shared render-origin matrix and draws 254k instances whose real per-instance
data rides their own 48-byte vertex stream. Every instance > 0 read past a
224-byte upload.

Under GL the SSBO bind carries a size and NVIDIA's out-of-range read returned
zeros — `u_Model` was a zero matrix and every blade collapsed invisibly to the
render origin, so the parity gate's GL control frame showed bare terrain and
*looked plausible*. Under Vulkan the same block is a buffer-device-address
pointer in the root struct: a BDA read has no bounds, so once N crossed into
an unmapped page (~32 MB past a 12 MB buffer at 254k) the GPU page-faulted and
the device was lost. Small N read garbage from neighbouring allocations and
*survived* — which is why the crash scaled with instance count while every
suite tenant (correctly-sized uploads) stayed green.

`VK_EXT_device_fault` is the instrument that closed it: the driver reported
`READ of invalid address` at base+32 MB, and a trace log of every vertex
buffer's BDA range turned that address into "224-byte-stride indexing of the
one-entry instance block". Windows gives no dmesg/Xid currency — without the
fault report a Vulkan device loss here is unattributable. The fix
(`OLO_INSTANCE_SINGLE`) *restored* grass under GL as well; the bug was
backend-neutral, only the punishment differed. Audit "the CPU uploads as many
entries as the shader indexes" contracts per DRAW SITE, not per shader
family, when a backend moves them onto raw pointers.

## 13. Phase 9 (rollout): a contract measured during a broken frame is not a contract

The single-orientation fix (ADR 0011 amendment (85)) closed the epic's last
open seam, and the way it closed carries three transferable rules.

### 13a. Re-measure any property whose evidence predates a bug fix

Amendment (79) recorded Vulkan off-screen row order as PER-TARGET — some
intermediates GL-ordered, some top-down, "depends on how many and which hops
produced it" — and grew the `flipY` per-call knob to cope. The Phase 9 live
inventory (ten target archetypes captured raw: geometry products, passthrough
post hops, compute `imageStore` outputs, depth, shadow) measured every one of
them **uniformly top-down**. The per-target chaos had been observed against a
frame whose geometry was missing entirely (amendment (80)'s command-ordering
bug was live at the time), and it silently stopped being true when #794's
fixes landed. Nothing re-measured, so the knob and its per-consumer
predicates outlived their premise by a full phase. The rule: an observation
made while a known bug corrupted the observable is provisional, and the fix
that lands later owes it a re-measurement.

### 13b. A green orientation test proves the relation it asserts, not the picture

The present-blit tenant asserted "presented rows mirror chain rows" and
passed — because its chain was seeded from a CPU UPLOAD, whose memory rows
ride the passthrough hops unchanged, so mirror-of-upload happened to display
GL-identically. Production's blit input is the top-down rasterized chain, for
which the same mirror displays upside-down. Nobody saw it because *nothing
displayed that path*: the editor composites through ImGui, and the runtime —
the one FinalRenderPass-to-swapchain customer — was still Vulkan-gated from
Phase 4. Amendment (67)'s "orientation is a window-only proof" has a
corollary: a present path no window has watched is unverified regardless of
its tenant, and un-gating a target (the Phase 9 runtime un-gate) re-opens
every window-only property along its path.

### 13c. One predicate, one owner

The endgame shape that retired `flipY`: row order is a per-BACKEND constant
(bottom-up GL, top-down Vulkan), so exactly one predicate expresses it —
`RHI::RenderTargetRowsAreBottomUp()`, living with the projection seam that
owns the convention; `ImGuiLayer::RenderTargetRowsAreBottomUp()` is the
uv-facing spelling and forwards to it, and the readback helpers call it
directly. (The first cut of this change inlined the `GetAPI() == OpenGL` test
in three consumers and *documented* it as one predicate — review caught the
gap between the claim and the code, which is its own small instance of the
rule.) The four consumers
found silently wrong under Vulkan (content-browser thumbnails, render-graph
debugger previews, save-game thumbnails, UI clip rects) were exactly the
sites that had hand-rolled the convention instead of sharing a predicate —
the writer-enumeration failure of amendment (59), replayed at the reader
level. When a convention is global, give it one spelling and make every
consumer call it.

### 13d. Closing a named gap is not the same as fixing the symptom that led you there

Phase 9 chased one validation error per window resize (a sampled image in
`UNDEFINED` where its descriptor promised `SHADER_READ_ONLY`). Reading the
bind paths found a real, *documented* hole on the way: the heap descriptor
route baked layouts into descriptors and issued no transition at all — the
debt the Phase 8 issue text carried as "the heap path's own mid-pass
visibility seam (amendment (63) covers the slot path only)". Closing it is
correct and it was closed.

It was not the bug. The same reproduction still fired afterwards, because the
failing sample reaches the image by a third path entirely. The near-miss is
the lesson: a plausible mechanism that *also* explains the symptom is not
evidence that it *is* the symptom's cause, and a fix motivated by a
reproduction owes that reproduction a re-run before the commit message says
"fixed". Here the re-run is what caught it — the error count was unchanged,
which is a far more honest signal than a green test suite, because no test
covered the case in the first place.

The corollary for reporting: when a change closes a gap without fixing the
motivating symptom, say both things. The gap-closing still ships (it is
correct on its own terms); the symptom gets filed with what was *ruled out*,
which is the expensive half of the next person's work — the ruled-out path is
worth as much as the suspected one.

---

## 14. Mesh shaders (#813): a new stage family's actual cost, and what the first full frame flushed out

`VK_EXT_mesh_shader` for virtual geometry landed as a Tier-2 optional
capability with a task+mesh+fragment pipeline replaying the same
visible-cluster segments the MDI path draws. What follows is what the port
actually cost and found, because most of it was NOT mesh-shader work.

### 14a. The thin-PSO design absorbed the new pipeline kind almost for free

§5's "no vertex input at all" decision (vertex pulling) meant a mesh pipeline
differs from a classic one by exactly: the stage array, `pVertexInputState` /
`pInputAssemblyState` both null, and the two input-assembly dynamic states
(topology, primitive restart) **absent from the declaration** — declaring
them against a mesh pipeline is itself a validation error, not just setting
them. No new PSO key axis: the shader's stage set IS the pipeline kind, and a
reload that changes it runs `InvalidateShader` first. The capability is ONE
predicate (`RenderCommand::SupportsMeshShaders()`, the §13c rule), enabled
Tier-2-style (never an ADR 0010 gate row), and `VirtualGeometryPass` logs the
raster-path decision at Init — a silent fallback here would make every later
measurement a measurement of the wrong path.

### 14b. glslang at SPIR-V 1.6 emits `LocalSizeId`, and the first new stage family finds the missing feature bit

The first task/mesh modules failed `VUID-RuntimeSpirv-LocalSizeId-06434`:
glslang lowers any workgroup-sized stage to `OpExecutionMode LocalSizeId` at
SPIR-V 1.6 (the vulkan_1_4 tier), which requires `maintenance4` — core in
1.3, MANDATORY there, and **still default-OFF at device creation** (the
synchronization2/dynamicRendering class, one more member). §9e said the
feature-bit list grows with every shader family a port reaches; this is the
worked example — the sweep tests compile SPIR-V and cannot see a device
enable-bit, so it surfaced only at the first `vkCreateShaderModule`.

### 14c. The first full frame of a subsystem is an audit of that subsystem, not of your change

Virtual geometry had never rendered a full frame on Vulkan (its primitives
were tenant-pinned, its shaders compiled — no full-pass evidence). The first
live frame found, in order: `VirtualVisibilityResolve.glsl` still consuming
vertex ATTRIBUTES (no `OLO_VULKAN` pull branch — pipeline creation fails
VUID-07904 and the SW-raster resolve silently never draws; the §5f sweep was
keyed on shaders a Vulkan session had actually reached, the §11 shadow rule
replayed); a device fault (`VK_EXT_device_fault`: READ of invalid address)
somewhere in the software-raster path; and a stale baked-layout warning
(VUID-09600) on a sampled image. None of these involve mesh shaders — the
fault reproduces bit-for-bit with the mesh path forced off.

**The runtime mode levers are what made that attributable in minutes.** The
`hwRasterMode` / `swRasterMode` pair let the whole fault matrix be walked
live over MCP without a rebuild: {mesh, MDI} × {sw-auto, sw-off} — both
sw-auto cells fault, both sw-off cells run. Give every alternative GPU path a
runtime lever; it is the difference between a bisect and an afternoon of
rebuilds.

### 14d. Measure before believing the new path is faster

On the one resolvable VG scene (~4.1k drawn clusters of ≤128 tris, Debug,
RTX 4090), `GPUPassTimerPool` medians over 12 samples: mesh 0.061 ms, MDI
0.053 ms — the mesh path is marginally SLOWER at this scale, and both are
noise at frame level. The issue's crossover question stays open; what ships
is the lever and the honest number, not a claimed win. The parity evidence
that matters: mesh-vs-MDI live frames differ by ZERO pixels at three camera
poses (noise floor also zero), over a frame the VG-on/off toggle proves is
full of virtual-geometry content — and the device-gated tenant pins the same
property headlessly (identical albedo + entity-id images, and the task stage
honouring a GPU-written count cut).

### 14e. A hand-encoded target env is accepted, not validated — pin the dialect too

The Vulkan tier hand-encodes `shaderc_env_version_vulkan_1_4` as
`(1<<22)|(4<<12)` because the *enumerator* is absent from any shaderc that
predates the 1.4 SDK, and the comment reasoned that runtime behaviour on such
a box is moot — no contract device exists there, so those builds only need to
compile. That reasoning was sound and the conclusion was still wrong, because
"only needs to compile" is precisely what stopped working.

`SetTargetEnvironment` takes the integer. An older shaderc — Ubuntu 24.04's
`libshaderc 2023.8`, which is what the sanitizer runners install — **accepts
the value, fails to recognise it, and silently falls back to a much older
SPIR-V**. There is no diagnostic: the API has no "unsupported version" return.
The first visible symptom was `GL_EXT_mesh_shader : not supported for current
targeted SPIR-V version` on all three Linux sanitizer jobs, while the same
commit compiled clean on the SDK-current Windows toolchain — the platform
split is the tell.

The narrow bug was the mesh stages. The real one is that **every** Vulkan-tier
shader on that toolchain was being compiled in a different SPIR-V dialect than
production ships, which is exactly what the sweep test's own comment says must
not happen. A dialect is not a detail: it decides which extensions are
permitted and which execution modes glslang emits (§14b is the same axis from
the other side).

The fix is one line and generalises: **when you hand-encode a version because
the name is too new, pin the thing that version was standing in for as well.**
`options.SetTargetSpirv(...)` is a no-op wherever the env is understood
(Vulkan 1.4 lowers to SPIR-V 1.6 anyway) and repairs the silent downgrade
everywhere else. Verify the mechanism in seconds rather than through a build —
`glslc --target-env=vulkan1.0 --target-spv=spv1.6` compiles a mesh shader that
`--target-env=vulkan1.0` alone rejects, which is the whole claim.

Two smaller things this turned up, both worth copying:

* `static_assert(kShadercEnvVulkan14 == ((1u << 22) | (4u << 12)))` asserts
  **nothing** — it compares the constant to its own definition, so it is true
  by construction and would survive any typo made in both places at once. An
  encoding assert has to name the expected *value* (`== 0x00404000u`).
* A red "Sanitizer" job is not evidence of a sanitizer finding. Those jobs run
  the whole suite under instrumentation, so an ordinary failing test turns them
  red with no ASan/UBSan/TSan report anywhere in the log. Grep the log for
  `ERROR: AddressSanitizer` / `runtime error:` / `WARNING: ThreadSanitizer`
  before you start hunting for a memory bug that was never reported.

---

## 15. Diagnostic parity (#810): a tool that refuses is honest; a tool that answers from the wrong table is not

Phase 9 left the debug tree structurally backend-neutral (`tools_gl_calls` is 0)
without giving Vulkan an equivalent **answer** for everything the GL tools
report. Closing that gap produced four rules that generalise past this issue.

### 15a. When two subsystems already record the same fact, enumerate the one that is not optional

`GPUResourceInspector` is fed by `OLO_GPU_REGISTER_*` macros that only
`Platform/OpenGL` TUs call, so the obvious fix was "call them from Vulkan too".
That is the two-mirrors-drift shape: every backend resource **already**
registers with `RHI::ResourceRegistry` at creation, and a second hand-maintained
list of the same objects rots the moment somebody adds a resource class and
forgets one of the two. The fix was a `Snapshot()` on the registry that already
holds identity + native + kind + owner, and a per-backend `DiscoversResources()`
flag deciding whether the shell is pushed into or pulls.

The general form: before adding a registration call site, ask what *already*
observes the thing you want to list. Registration you can forget is worse than
enumeration you cannot.

### 15b. `native != 0` is not a liveness test, and the exceptions are the interesting resources

A Vulkan framebuffer registers native 0 — there is no `VkFramebuffer` under
dynamic rendering (amendment (83)) — and an arena-backed uniform buffer has no
native object at all. Filtering an enumeration on `Native != 0`, or keying a map
on the native handle, silently drops or merges exactly those. Both read as
obviously-correct code. Liveness comes from the registry's freelist; identity
comes from the handle. The rule generalises to any "opaque id, 0 means none"
convention where the backend is allowed to have no id.

### 15c. Widen the id before you port the consumer

The inspector's native ids were `u32` throughout. A `VkImage` truncated into one
is not a failed lookup, it is a *plausible* number that resolves to nothing — and
on a diagnostic, plausible-and-wrong is the failure mode that costs the most,
because the next investigation trusts it. Widening to `u64` first (GL names widen
losslessly) made every later step mechanical.

### 15d. A readback destination format is a per-backend contract, not a preference

Porting the pixel probe and target stats onto `RenderCommand::ReadTextureSubImage`
surfaced a divergence the facade's signature hides: a DEPTH source must name a
DEPTH destination. GL needs it because only depth destinations lower to
`GL_DEPTH_COMPONENT` (asking for `GL_RED` is `GL_INVALID_OPERATION` — a silently
zero-filled buffer, not an exception), and Vulkan needs the same name because its
identity fast path only fires when the image really is `VK_FORMAT_D32_SFLOAT`,
while this hardware backs the graph's `Depth24Stencil8` with
`D32_SFLOAT_S8_UINT` (amendment (79): key on the Vulkan format, never on the
graph's label). The single-backend contract test that pins this —
`FacadeReadbackParityTest`, which reads the same texels through the facade and
through raw GL in ONE process and requires bit-equality — is the thing a
cross-backend A/B cannot do, because that compares two binaries against two
frames.

### 15e. Refuse the sub-feature, not the tool — then check whether the corner is real

`olo_render_probe_pixel` and `olo_render_target_stats` refused wholesale on
Vulkan because ONE of their arguments (`afterPass`) rode a GL-only mid-frame
clone. The first cut narrowed the refusal to that argument. When a tool has a
backend-specific corner, gate the corner: a whole-tool refusal reads to the
next session as "this question is unanswerable here", which is a much more
expensive wrong belief than "this option is unavailable".

Then the corner turned out not to be real, which is the second half of the
lesson. See 15f.

### 15f. "This cannot be neutral" can be an artefact of who MINTED the object

`PassSnapshotBackend.h` stated its native currency was deliberate and that "no
`RHI::ResourceHandle` can exist on this path", because `native -> handle` is
not recoverable. True — for a name somebody else minted. The snapshot's scratch
clone is not that: the snapshot **creates** it, and anything you create can be
created WITH an identity. Nobody had drawn the distinction because nothing yet
wanted the identity, so a correct local observation hardened into a wrong
global rule and a whole tool family inherited a refusal from it.

The generalisable check, before accepting "this seam cannot be neutral":
separate the objects the seam **adopts** from the ones it **allocates**. Only
the adopted ones are constrained.

Retiring it deleted more than it added — the GL clone engine, its seam header,
and three native-currency readback helpers in the MCP layer all went.

### 15g. Match the SOURCE, do not describe it, when the description is lossy

The scratch allocator is spelled `CreateMatchingTextureHandle(source)`, not
`CreateTexture(RHI::TextureDesc)`. A neutral desc would force the source's
native format out to `RHI::Format` and back, and `RHI::Format` is deliberately
narrower than what the render graph creates. The failure would not be loud:
`glCopyImageSubData` and `vkCmdCopyImage` both require format compatibility, so
a near-miss yields an empty or garbage clone that the diagnostic reports as
fact. "Match this" lets each backend reproduce its own description and never
translate.

The corollary is a shape query, not just a format one. A 64-slice volume and a
64-layer array report the same layer count and are NOT interchangeable — the
copy names a target type on both operands and the driver rejects a mismatch. An
early draft inferred dimensionality from the layer count with a hardcoded
`isVolume = false`; that is the shape of a bug that only ever fires on the
froxel-fog volumes. `RHI::TextureFormatInfo::Shape` exists so the question has
an answer instead of a guess.

### 15h. A tool with no backend guard is not the same as a tool that refuses

Auditing the refusals turned up the one tool that did NOT refuse:
`olo_render_validate`'s `compare` had no backend check at all, and resolved
through `Debug::NativeTextureIdForDiagnostics`, which does
`static_cast<u32>(nativeHandle)`. Under Vulkan that truncates a `VkImage`
pointer to a **nonzero garbage** `u32` — so the zero-check passed — which then
reached `glGetTextureLevelParameteriv` with no GL context. A crash that had
been live since the tool shipped.

When sweeping for "which tools are gated on backend X", the dangerous entries
are the ones the sweep does not match. A truncating cast to a smaller native
type is the specific mechanism to grep for: it turns "no answer" into "an
answer that passes a validity check".

**Issue #890 is the sequel, and it inverts the lesson.** With the crash fixed,
`olo_render_validate` could finally run on Vulkan — and returned a false
`ok: false` naming 13 resources as unbacked while `olo_render_capture_target`
read real HDR pixels out of one of them in the same session. The mechanism was
not truncation at all. Eleven of the 13 were framebuffer-backed, and
`VulkanFramebuffer::GetColorAttachmentRendererID` returns **0** by design; the
predicate read that 0 as "no object". So the grep above catches half the
family. The other half is the opposite shape: **a legitimate zero read as an
absence.**

The check that covers both: for any predicate a diagnostic *branches* on, ask
what a native `0` means on every backend before treating it as "nothing here".
Under Vulkan a framebuffer attachment, an arena-backed uniform buffer and every
texture class all report 0 legitimately — so a native handle can CONFIRM that
an object exists and can never DENY it. The decision belongs to
`RHI::ResourceHandle`, and the honest predicate is a storage query
(`Debug::HasLiveTextureStorage`), not a null test: two of those 13 were true
positives, and a fix built on "the identity is non-null" would have silenced
them. ADR 0011 amendment (90) carries the full rule.


## 16. A CPU-side layout tracker has TWO timelines (#800), and only one of them is the queue's

The Vulkan backend keeps one CPU state machine that GL has no twin for:
`VulkanImageLayoutTracker`, the authority for every barrier's `oldLayout`. It
was written with a single layout per subresource, and that single value quietly
answered two different questions:

- **What will this image be in once the command buffer I am recording is
  submitted?** — the right question for a barrier recorded *into that same
  buffer*.
- **What is this image in right now, on the queue?** — the right question for
  anything that reaches the queue *ahead* of that buffer.

`VulkanOneShot::Submit` is the second kind. It allocates its own command
buffer, records, submits and waits — so it executes *before* the frame command
buffer that is still being recorded (amendment (72) already established the
ordering; what it did not establish is that the tracker cannot describe it).
`ReadTextureSubImage`'s borrow mode is the one place that read a layout out of
the tracker and handed it straight to a one-shot as `oldLayout`:

```cpp
borrowedLayout = m_LayoutTracker.CurrentLayout(image, range);   // WRONG timeline
```

**Why this survived a year of frames.** On a steady frame the two answers are
identical, and not by luck: the render graph ends every frame in the same
layouts it started in, so "what the recording will leave" and "what the queue
has" converge to `SHADER_READ_ONLY` for exactly the attachments a mid-frame
readback names. The two diverge on precisely one frame — the frame an
attachment is **created**, i.e. a window resize. There the recording has
already walked the brand-new image `UNDEFINED → COLOR_ATTACHMENT → …  →
SHADER_READ_ONLY`, none of it submitted, while the image on the queue has never
been touched and is still `UNDEFINED`. The borrow barriers from
`SHADER_READ_ONLY`, and the layer says so once, then the frame's own barriers
execute and everything realigns. One error per resize, self-healing next frame:
issue #800, exactly as reported.

**The fix is to stop overloading the value.** The tracker now carries a second
layout per subresource — the layout after all *submitted* work:

- `CommitRecordedToExecuted()` at the end of `EndRecording` (and after a
  successful mid-frame flush) advances it: the bracket closing is what puts the
  recording on the queue.
- `VulkanImageLayoutTracker::ImmediateExecutionScope`, opened by
  `VulkanOneShot::Submit` around its record callback, makes writes inside it
  advance **both** — that work reaches the queue as it is recorded. This is what
  keeps the non-borrow readback arm and the load-time clear fallback correct,
  with no call-site edits.
- Borrow mode reads `CurrentExecutedLayout`, and its existing
  "`UNDEFINED` → refuse the read" arm then does the right thing on a resize
  frame: a brand-new image has no content worth borrowing, and transitioning it
  from `UNDEFINED` would discard the very pixels the read wants.

**Three things this cost, that are worth not repeating.**

*The repro recipe was not the one in the issue.* "Move the window" does not
reproduce it: a dense drag storm of 80 `SetWindowPos` resizes gave **0 errors in
6 rounds**. It needs a restore↔maximize cycle **and** the mouse parked over the
viewport — the second ingredient is not cosmetic, it is what drives
`VulkanFramebuffer::ReadPixel` into the borrow path that carries the bug.
Measured on one binary: 0/10 without the mouse, **10 hits / 8 rounds** with it.

*The bug hides from its own instrumentation.* Anything that adds work to the
image-**creation** path closes the race. A per-frame `OLO_CORE_WARN` in the
barrier loop: 8 rounds, 0 errors. `vkSetDebugUtilsObjectNameEXT` on every image
registration: 14 rounds, 0 errors — which is why `VulkanDebugNames` is opt-in
behind `OLO_VK_OBJECT_NAMES` rather than always on. What *does* work is a
zero-I/O ring buffer written on every tracker mutation and dumped from the
validation messenger when an error fires; that shape reproduced at the
unmodified rate. `std::source_location` as a **default argument** on `SetLayout`
/ `RegisterImage` attributes each write to its caller with zero call-site edits,
and is what turned "some transfer path" into "`ClearTextureUInt`, line 777".

*The decisive fact was in the message all along.* The error names the command
buffer. Every one of the eight occurrences named a **one-shot** buffer, never a
frame buffer — which alone rules out the entire ImGui/descriptor half of the
issue's candidate list. Read the handle in the message before reasoning about
who could have recorded the draw.

**The general rule.** Any CPU mirror of GPU state has to answer *at which point
in the queue*. If one field serves both "after what I am recording" and "right
now", the two agree on every steady frame and disagree on the first frame a
resource's lifetime changes — which is the frame nobody tests.
