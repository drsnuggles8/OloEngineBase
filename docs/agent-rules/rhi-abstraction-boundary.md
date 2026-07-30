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

- **The generation is load-bearing.** GL recycles object names, so today two
  genuinely different objects can compare equal through `Texture::operator==`
  (which compares `GetRendererID()`) when one was deleted and another created.
  `TransientPool`'s alias reporting — the tooling built in #607 specifically to
  answer "did these two plan entries get the same object?" — depends on telling
  those apart.
- **`HeapOffset` must stay layout-compatible with `u32`.** It gets written into
  a UBO and read by GLSL as an array index; it cannot be opaque.

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
