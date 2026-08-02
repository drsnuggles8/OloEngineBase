# The OpenGL abstraction boundary — where it actually leaks, and how to measure it

Working notes for issue [#691](https://github.com/drsnuggles8/OloEngineBase/issues/691)
(add a Vulkan backend alongside OpenGL 4.6). The *decisions* live in
[ADR 0010](../adr/0010-vulkan-rhi-heap-bindless-only.md) (scope) and
[ADR 0011](../adr/0011-rhi-neutral-resource-and-binding-model.md) (the neutral
model, backend selection, PSO cache). This file is the part a future session
would otherwise have to rediscover by reading the whole renderer.

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
is what #691 Phase 8 is already warning about.

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

### How far "full bindless" actually reaches — measured, not estimated

After SSAO the counter stands at **230** (232 − 3 converted + 1 for the fallback
bind inside the new seam, which is a real bind site and correctly counted). The
remainder is not homogeneous, and the split decides whether the phrase means
anything:

| Bucket | Sites | Shape |
| --- | ---: | --- |
| Ordinary pass binds (35 files under `Renderer/Passes/`) | ~150 | Mechanical, exactly like SSAO. |
| `Renderer/Commands/` — the material path | ~21 | The redundant-bind cache. One indivisible unit, same shape as Phase 2 slice 6. |
| **`BindImageTexture` — storage images** | **38** | **Not modelled at all.** |

**The storage-image bucket is the real answer to "can we go full bindless".** The
heap produces *sampler* descriptors only: `ViewDesc` + `SamplerDesc` →
`glGetTextureSamplerHandleARB`. An `imageLoad`/`imageStore` binding is a
different descriptor — `glGetImageHandleARB`, with its own residency and a
format+layered+level key that `ViewDesc` does not carry. `ARB_bindless_texture`
supports it and `VK_EXT_descriptor_heap` treats it as just another descriptor
type, so this is a gap in *our* model rather than in either API — but it is ~16%
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

## 5. `ResourceTransition` looks backend-neutral and is neutral only by accident

`RenderGraph::ResourceTransition` carries `ResourceName`, `ProducerPass`,
`ConsumerPass`, `FromUsage`, `ToUsage`, `Range`, and cross-queue metadata. That
is very nearly a Vulkan barrier, and the barrier planner is genuinely
backend-agnostic — real RHI-shaped thinking.

But the transition is typed `RGWriteUsage FromUsage` → `RGReadUsage ToUsage`, so
it **structurally cannot express a write → write transition.** The planner *does*
emit WAW barriers. `BuildResourceTransitions` then looks for a matching **read**
declaration on the consumer, finds none, and silently defaults to
`t.ToUsage = RGReadUsage::ShaderSample`.

On GL this is invisible, because the actual synchronisation comes from
`barrier.Flags` (a `glMemoryBarrier` bitmask the planner derived correctly from
the *write* usage) and the bogus `ToUsage` is never read. On Vulkan, a backend
deriving `(dstStageMask, dstAccessMask, newLayout)` from `ToUsage` would lower a
storage-image WAW into `SHADER_READ_ONLY_OPTIMAL` with a read-only access mask —
wrong layout, wrong access, silent.

**Generalisable lesson: a record is not backend-neutral just because its field
types are.** This one is held up by a GL-specific field sitting next to it. When
auditing an abstraction for a second backend, ask what each field is *actually
read for today* — a field nothing reads is a field nothing keeps correct.

Related, same audit: `MemoryBarrierFlags` is a `glMemoryBarrier` bitmask that has
been promoted to the neutral currency (`PlannedBarrier::Flags`,
`RGCommandContext::MemoryBarrier`, `RendererAPI::MemoryBarrier`). The usage pair
is the truth; the flags are a GL lowering and belong in the GL backend.

And a non-gap worth recording so nobody adds it: **the neutral model deliberately
carries no image layout.** Layout is a pure function of usage
(`ShaderSample → SHADER_READ_ONLY_OPTIMAL`, `RenderTarget → COLOR_ATTACHMENT_OPTIMAL`,
`ShaderImage → GENERAL`, …), so the Vulkan backend derives it. Hoisting a layout
enum into the render graph leaks Vulkan upward for zero gain.

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
recreation cheap anyway. Destruction must go through `InflightFrameManager`,
since a replaced pipeline may still be referenced by in-flight command buffers.

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

`RendererAPI::s_API` is today a static initialised to `OpenGL` and **never
written by anything**. Giving it a real setter comes with a hard ordering
requirement:

> `RendererAPI::s_API` must be set **before `Window::Create`**.

This is not hypothetical — the seam already exists and is already used:
`WindowsWindow::Init` calls `Renderer::GetAPI()` before `glfwCreateWindow` (to
decide `GLFW_OPENGL_DEBUG_CONTEXT`), while `Application`'s constructor calls
`Window::Create` at line 62 and `Renderer::Init` only at line 72. The setter
belongs in `Application`'s constructor, before line 62, parsed from
`ApplicationCommandLineArgs` (which already has a `Contains()` helper for
mode flags like `--smoke-test`). Anything reading `Renderer::GetAPI()` during
static initialisation sees `OpenGL` regardless; nothing does today, and nothing
should start.

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
experimental) can fall back at runtime. `OLO_WITH_VULKAN=OFF` exists for
`OloServer`/WSL2 and lean CI, where no Vulkan SDK is present.

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
