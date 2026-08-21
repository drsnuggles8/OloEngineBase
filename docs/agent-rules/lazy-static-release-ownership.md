# Release a shared lazy static from the teardown that always runs, not from the one that happened to draw with it

Issue #814. A one-line release site in the wrong function leaked a GPU object in
every session that never initialised Renderer3D — invisibly for the whole life of
the OpenGL backend, and loudly the first time the Vulkan runtime path was run.

## The trap

`MeshPrimitives::GetFullscreenTriangle()` caches a lazy static
`s_FullscreenTriangleVA`: a VAO plus a 64-byte vertex buffer and a 16-byte index
buffer, built on first use and reused by every fullscreen pass in the engine. Its
only release site, `MeshPrimitives::Shutdown()`, had exactly one caller —
`Renderer3D::Shutdown()`, sitting between `IBLPrecompute::Shutdown()` and
`ImpostorBaker::Shutdown()`, which are genuinely 3D-only facilities.

But the triangle's *first* creator is not 3D at all. `Renderer::Init()` always
calls `ShaderWarmup::Init()` and `Renderer2D::Init()`, and `Renderer2D::Init()`
draws the shader-warmup progress frame through
`ShaderWarmup::RenderProgressFrame()` → `MeshPrimitives::GetFullscreenTriangle()`.
So a session that puts up the warmup screen and then exits without ever bringing
3D up creates the triangle and never releases it.

That session is not hypothetical. `OloRuntime` calls `Application::Close()` from
`RuntimeLayer::OnAttach` when it finds no start scene — before 3D init. `Shutting
down Renderer3D` appears **zero** times in its log, and the Vulkan teardown
forensics name the survivor with its creation stack:

```text
[Runtime] No scene files found. Cannot start game.
...
[Vulkan] surviving VertexArray (1 stream(s)) created at:
  ... MeshPrimitives.cpp(118):  MeshPrimitives::GetFullscreenTriangle
  ... ShaderWarmup.cpp(254):    ShaderWarmup::RenderProgressFrame
  ... Renderer2D.cpp(261):      Renderer2D::Init
[Vulkan] 1 shader/vertex-array object(s) survived full teardown
[Vulkan] 2 VMA allocation(s) still alive at allocator teardown (80 bytes)
  {"Type": "BUFFER", "Size": 64, "Name": "vmaCreateBuffer (VulkanVertexBuffer)"}
  {"Type": "BUFFER", "Size": 16, "Name": "vmaCreateBuffer (VulkanIndexBuffer)"}
Assertion failed: m_pMetadata->IsEmpty() &&
    "Some allocations were not freed before destruction of this memory block!"
```

**In a Debug build that last line is not a report, it is a hang.** The VMA
assertion raises the CRT assert dialog inside `vmaDestroyAllocator`, so the
process never exits — a shipped Debug runtime that fails to find its start scene
sits there forever. 80 bytes of leaked buffer is trivial; the exit behaviour is
not.

To reproduce it, the runtime's **working directory** has to satisfy two
conditions at once: `assets/shaders` and `assets/fonts` must resolve (or the
runtime dies earlier on a shader or font load), and there must be **no**
`Scenes/` directory and no `game.manifest` (or it finds a start scene, initialises
Renderer3D, and the leak does not occur). `OloEditor/` is exactly that
combination. From the repo root:

```powershell
$p = Start-Process -FilePath .\bin\Debug\OloRuntime\OloRuntime.exe `
                   -WorkingDirectory .\OloEditor `
                   -ArgumentList '--rhi=vulkan' -PassThru -NoNewWindow
# Pre-fix this never exits (the assert dialog above) — bound the wait and kill it.
if (-not $p.WaitForExit(60000)) { $p.Kill(); $p.WaitForExit() }
"$($p.ExitCode)"   # 0 once fixed
```

`-WorkingDirectory` there is a `Start-Process` parameter, **not** an OloRuntime
flag — the runtime has no such argument, and passing it to the exe does nothing.
Prefer this over `cd`/`Set-Location`/`Push-Location` into `OloEditor`: an agent
session shares one tracked working directory with its `PreToolUse` hooks, and
leaving it parked in a subdirectory wedges the build-lock guard for every
subsequent command. `Start-Process -WorkingDirectory` never touches it.

Read the result in `OloEditor/OloEngine.log`, which the runtime truncates on each
launch.

## The shape, stated generally

**A lazily-created shared static is released by whichever teardown its author
happened to be editing.** That is fine exactly while every session that can
*create* it also runs that teardown — and nothing in the code says so, so the
day a new entry path creates it earlier (or a gated path finally gets run), the
release site silently stops covering the object.

The three ingredients, all present here:

1. **Lazy creation** — nobody "owns" the object at a declared point, so there is
   no natural place the release belongs to.
2. **A cross-subsystem consumer** — the facility is engine-wide
   (`MeshPrimitives` is not 3D-only, which is *precisely why* a 2D/warmup path
   could reach it), but the release lives inside one subsystem.
3. **A conditional teardown** — `Renderer3D::Shutdown()` runs only
   `if (Renderer3D::HasInitialized())`.

## The rule

**Put the release in the narrowest teardown that is unconditional for every
session that can create the object.** For anything under `Renderer/` that is not
exclusively 3D, that is `Renderer::Shutdown()` — it owns both sub-renderers, so
it runs whether or not 3D came up. Reserve `Renderer3D::Shutdown()` for
facilities that cannot exist without a 3D scene (terrain, foliage, clouds, snow,
precipitation, shadows, occlusion, virtual geometry, IBL, impostors, the
command-dispatch/frame-data plumbing — all of those checked out).

Two corollaries worth not re-deriving:

- **Moving a release *later* in `Renderer::Shutdown` is safe on both backends**,
  which is not obvious. On GL, `FrameResourceManager::SubmitForDeletion()`
  executes the delete lambda immediately once the manager is shut down, so a
  release after `Renderer3D::Shutdown()` still deletes while the context is
  current. On Vulkan, buffer destructors enqueue into `VulkanDeferredReclaim`,
  and `VulkanContext::Shutdown()` runs a **second, final** `FlushAll()`
  immediately before `VulkanDevice::Shutdown()` — long after `Renderer::Shutdown`.
  What is *not* safe is releasing after the context is gone. Keep it before
  `RenderCommand::ShutdownGpuResources()` — but note that call only does anything
  on GL (`OpenGLRendererAPI` overrides it; the Vulkan path uses the empty
  `RendererAPI` base default), so the real Vulkan deadline is
  `VulkanContext::Shutdown()`, which runs later still, from `m_Window.reset()`.
- **Do not read "no error on GL" as "no leak on GL".** GL has no
  allocator-teardown assertion, so this exact leak was silent there for the whole
  life of the backend. The bug is backend-independent; only the *detection* is
  Vulkan-only. The corollary for new work: a lifetime bug found on Vulkan is a
  bug on GL too, and needs no separate GL reproduction to justify fixing.

## Guard

`RendererShutdown.WarmupReachableFacilitiesAreReleasedBySubsystemNeutralShutdown`
(`OloEngine/tests/Rendering/RendererShutdownTest.cpp`) is a source-text test, for
the same reason as its neighbour in that file: `Renderer3D` does not support
Init-after-Shutdown, so "bring it up warmup-only, tear it down, assert nothing is
left" cannot be written in-process without destroying the rest of the suite.

It collects every `Foo::Shutdown*()` / `Release*()` / `Clear*()` call in
`Renderer3D::Shutdown()` **or** in the teardowns that always run — the union, so
the very edit that fixes this cannot make the test vacuous — and fails any
facility the warmup path mentions that none of the always-run teardowns
releases. Against the unfixed tree it names exactly one facility,
`MeshPrimitives`; the other 32 are either 3D-only or already released.

"Always run" is `Renderer::Shutdown()` **plus the two teardowns it calls
unconditionally**, `Renderer2D::Shutdown()` and `ShaderWarmup::Shutdown()`.
Releasing a warmup-created static where it is created is equally correct — it
was fix option (a) on the issue — and a guard that only accepted
`Renderer::Shutdown()` would report that correct fix as a violation. Only
`Renderer3D::Shutdown()` is disqualified, and precisely because
`Renderer::Shutdown()` guards it with `if (Renderer3D::HasInitialized())`.

The union alone was **not** enough, and the way that surfaced is worth copying:
the first version was red-checked by moving the call back, which it caught — but
red-checking the *other* regression, deleting the call outright, made the
facility vanish from the discovered set entirely, so the test fired its
non-vacuity assertion with advice that pointed at the wrong thing. Hence
`kEngineWideFacilities`, a seeded roster the scan starts from. **Red-check a
source-scanning guard against every way the invariant can break, not just the
one you fixed** — a scan that discovers what to check can stop checking.

Its limit, stated so nobody over-trusts it: reachability is a **one-hop textual
scan** of `Renderer2D.cpp` and `ShaderWarmup.cpp`. A facility the warmup path
reaches through a third file is invisible to it. The empirical backstop is the
in-tree Vulkan teardown forensics (#794) — run the reproduction above and read
the log; it names *every* surviving object, not just the one this test knows
about.

One corroborating detail worth knowing before you conclude a static is fine
where it is: `VulkanPassSuiteTest`'s fixture teardown already had to call
`MeshPrimitives::Shutdown()` by hand, with a comment noting that the
fullscreen-triangle cache "is a process STATIC now holding Vulkan VMA buffers …
released here or the allocator teardown asserts on the leak". A test that has to
reach past a subsystem's teardown to release something is evidence the release
site is in the wrong place; it read as a test-fixture quirk for a whole phase.

---

# The sweep (#839): what the rest of the class looked like

#814 fixed one member and this document predicted there were others. There were
eight, and the part worth carrying forward is that **they were not all the same
shape**. "The release site is in the wrong teardown" turned out to be the
*rarest* of three species:

| species | what it looks like | members found |
|---|---|---|
| **(a) release in a conditional teardown** | a release call exists and runs, just not in every session that can create the object | the fullscreen triangle (#814) — and nothing else |
| **(b) no release site at all** | nobody ever wrote one; the object is simply immortal | the default font, the GL cubemap staging PBO, the inspector's scene reference, the inspector's undo-snapshot maps, the scene's default material |
| **(c) a release site nothing calls** | someone wrote the teardown — one of them with a comment saying when to call it — and never wired it up | `VideoSystem::Shutdown()`, `GPUTimerQueryPool::Shutdown()` |

Species (c) is the one to internalise. Both instances read as *finished* code: a
public `Shutdown()`, a correct body, and in `VideoSystem`'s case a doc comment
reading "Release the global fullscreen player. Call on engine/scene shutdown."
Grepping for "is this released?" finds the function and stops. The question that
finds the bug is **"who calls it?"**, and it has to be asked separately.

So the sweep is three scans, not one:

1. every process static holding a `Ref<T>` where `T` owns GPU memory — does its
   own translation unit reset it anywhere?
2. every `Shutdown()` / `Release*()` on a static or singleton facility — does
   anything call it?
3. every raw `static GLuint` / `static Vk*` in the platform layers — is there a
   matching delete?

Scan 3 is not garnish. It found a persistent `GL_PIXEL_UNPACK_BUFFER` staging
buffer in `OpenGLTextureCubemap`'s face upload, created on first use, grown by
`glNamedBufferData` to the largest cubemap face the session ever uploaded, and
never deleted. It is **GL-only**, so the Vulkan forensics that found everything
else were blind to it, and GL has no allocator assertion to complain. *A leak
class found on Vulkan still needs a GL-side pass.*

## The big one was not a teardown-placement bug at all

The headline number — 64 surviving vertex arrays, two per mesh, plus the BRDF LUT
— was not a misplaced release. Every one of those `MeshSource`s was owned by the
deserialised scene through `MeshComponent::m_MeshSource`, the BRDF LUT through
`EnvironmentMapComponent`, and the surviving albedo textures through the model
components. They survived because **the scene survived**, and the scene survived
because `SceneHierarchyPanel.cpp` kept a file-scope `Ref<Scene>`:

```cpp
// File-scope state for undo integration in DrawComponent (set by DrawComponents)
static CommandHistory* s_DrawComponentCmdHistory = nullptr;
static Ref<Scene>      s_DrawComponentScene      = nullptr;
```

`DrawComponents()` sets it every inspector frame so the templated
`DrawComponent<T>` helpers can build undo commands without threading two extra
parameters through ~95 call sites. That is a reasonable trick, and the comment
even scopes it to the call. Nothing ever unset it, so the last scene the
Properties panel drew was pinned for the rest of the process.

**The A/B that proves it**, same binary, same scene, `--rhi=vulkan`, close the
window:

| | vertex arrays | textures | VMA allocations | bytes | exit |
|---|---|---|---|---|---|
| no entity selected | 0 | 0 | 0 | 0 | 0, "Context shut down cleanly" |
| one entity selected | 12 | 6 | 30 | 147,330,896 | **3 (abort)** |

Selecting an entity is the whole difference: it is what makes the Properties
panel call `DrawComponents()` at all. Worth knowing when reading a teardown-leak
report — **this class of bug can be conditional on a UI interaction**, so "I
couldn't reproduce it" may only mean you did not click the same thing. It also
means the failure is worse than a leak: with a selection, closing the Vulkan
editor **aborts** rather than exiting.

Two things this rules out. `~Application` *already* detaches the layers and calls
`Project::Unload()` before `Renderer::Shutdown()`, deliberately, with a comment
explaining that the asset manager's Refs would otherwise outlive the graphics
context (#691 Phase 8). **The teardown order was right; the object had one more
owner than the order accounted for.** When a survivor is a whole aggregate — a
scene, a model, an asset pack — rather than a single cached resource, look for
the extra owner before you reach for the teardown order: reordering teardown to
"fix" a stray strong reference just converts a leak into a use-after-free
somewhere less visible.

The general rule for a scratch static like this one: **if a static exists only
for the duration of a call, clear it at the end of that call** — with a scope
guard, not a trailing assignment somebody can jump over. A `Ref<>` parked in
file-scope state "until next frame overwrites it" is a leak on the frame that
never comes.


## The one the guard cannot see, and how it was found

The sweep above got the Vulkan teardown from 12 vertex arrays / 6 textures / 30 VMA
allocations / 147 MB down to **1 vertex array and 2 allocations (18 KB)** — and the
acceptance bar is zero, so that last one mattered. Its creation stack named a `Model`
built by `ModelComponent::Reload` during scene deserialisation, and it only appeared
when an entity had been selected in the inspector.

The owner is an **eighth** member, and it is species (b) wearing a disguise:

```cpp
struct EditState { bool isEditing; bool snapshotValid; T snapshot{}; ... };
static std::unordered_map<u64, EditState> s_EditStates;   // inside DrawComponent<T>
```

`snapshot` is a **copy of the component**, so the `ModelComponent` instantiation of that
template owns a `Ref<Model>` and the texture-bearing ones own `Ref<Texture2D>`. Nothing
ever cleared the maps, so drawing an entity in the inspector once pinned its GPU
resources for the life of the process.

Two things to take from it.

**A declaration scan cannot find a Ref inside a container.** The guard looks for
`static Ref<GpuOwningType> name;`. Here the Ref is a member of a struct that is the
`mapped_type` of a static map, three type layers down — invisible, and deliberately not
chased, because "recursively decide whether an arbitrary static's type transitively owns
GPU memory" is a type-system question a regex cannot answer. **The guard covers direct
declarations only. Say so; do not let it imply coverage it does not have.**

**Run the teardown under a debugger before believing a leak count.** The VMA leak assert
*aborts*, and an abort discards spdlog's buffered tail — so `OloEngine.log` stopped at
"Pipeline cache saved", several lines before the forensics that name the survivors, and
the run looked like it might be a fresh crash in `~VulkanContext`. Under `cdb` the
inherited console showed the whole report and `sxe av` proved there was no access
violation at all: same old leak assert, far fewer survivors. Without that, the last
member would have been missed and the leak reported as fixed.

Also worth knowing when reading a teardown log: **a nonzero exit code with a log that
stops early is the abort, not a second bug.** Check the console, not the file.

## The rule, one level up

#814 phrased it as "for anything under `Renderer/` that is not exclusively 3D,
release from `Renderer::Shutdown()`". Two of #839's members live outside
`Renderer/`, and releasing them from `Renderer::Shutdown()` would have inverted
the layering. The rule generalises:

> Release from the **narrowest teardown that is unconditional for every session
> that can create the object**, and that still runs **while the graphics context
> is alive**.

which resolves, in widening order, to:

| creator | release from |
|---|---|
| a 3D-only facility | `Renderer3D::Shutdown()` |
| anything else under `Renderer/` | `Renderer::Shutdown()` |
| a backend-private object under `Platform/OpenGL/` | `OpenGLRendererAPI::ShutdownGpuResources()` |
| a non-renderer subsystem that can own GPU memory (video, scene defaults) | `Application::~Application()`, after `Project::Unload()` and before `Renderer::Shutdown()` |
| state that lives for one call | the end of that call |

That third row has a trap worth knowing. The obvious home for the GL staging PBO
was the `if (GetAPI() == OpenGL)` branch already sitting in `Renderer::Shutdown()`
next to `OpenGLFramebuffer::ShutdownSharedResources()` — but `OpenGLTextureCubemap.h`
includes `<glad/gl.h>` (that framebuffer header does not), so calling it from
`Renderer.cpp` would drag the GL loader into an engine-core translation unit and
undo the `sweep_glad_includes == 0` property `RHIBoundaryRatchetTest` pins.
**And the ratchet would not have caught it**: it counts *direct* `#include` lines
naming `glad/gl.h`, so an indirect one through a backend header breaks the
property while the counter stays at zero. `OpenGLRendererAPI::ShutdownGpuResources()`
is the correct home instead — it is inside the backend, it is GL-only by
construction (`Renderer::Shutdown()` reaches it through
`RenderCommand::ShutdownGpuResources()`, which the Vulkan path inherits as the
empty `RendererAPI` base default), and it already runs while the context is
current, which is exactly the deadline a `glDelete*` has.

## The guard, and what it deliberately does not cover

`RendererShutdown.EveryProcessStaticGpuResourceHasAReleaseSite` implements scans
1 and 3. The two older tests in that file both start from a teardown *body* and
ask "is this facility released from the right place?", which only ever sees
facilities somebody already thought to release — structurally blind to species
(b). So this one starts from the **declaration**: every process static holding a
GPU-owning `Ref` (seeded roster, `kGpuOwningRefTypes`) and every lazily created
raw GL handle must be reset somewhere in its own translation unit. A third seeded
roster sits alongside the type list and is just as load-bearing: **the file
extensions the scan reads**. It walks `.cpp/.cc/.cxx` and `.h/.hpp/.hh/.inl`
(the repo has 15 `.inl` files under those roots), because a static in a file
whose extension is missing from that list is never examined and the test passes
regardless — the same silent-coverage-loss failure the type roster exists to
prevent, one level down. CodeRabbit caught the `.cpp`/`.h`-only version of this
on review. Where that
reset is *called* from is the other two tests' business.

It does **not** implement scan 2. A prototype was written and thrown away:
resolving `Foo::GetInstance().Shutdown()` through aliases, member calls and
delegating bodies textually produced enough false positives to be noise, and a
curated roster of "teardowns that must be called" drifts by construction.
**Species (c) has no cheap static guard.** It is caught empirically, by:

- the Vulkan teardown forensics (#794), which name every survivor with its
  creation stack; and
- `RendererMemoryTracker::Shutdown()`, which used to `clear()` its live-allocation
  map without looking at it and now reports what is in it first. That map already
  held the type, size, name, file and line of every survivor — so the one
  backend-independent instrument threw the answer away at exactly the moment it
  had it. Its limit: `TrackAllocation` is called from the **OpenGL** resource
  classes only, so on Vulkan the report is empty and #794 remains the instrument
  there. (`RendererMemoryTracker::Initialize()` had no callers either, which left
  the `m_IsShutdown` latch its own comment exists to clear permanently set after
  the first `Renderer::Shutdown()` in a process; `Renderer::Init()` now calls it.)

  **It earned its keep on the first run.** With this sweep complete and Vulkan at zero
  survivors, the very first OpenGL session reported **115 surviving shader programs**
  (3.2 MB) — and nothing else: no textures, no buffers, no vertex arrays. `OpenGLShader`
  pairs its alloc/dealloc tracking correctly, so those are 115 objects genuinely still
  alive after `ShaderLibrary::Clear()`, `ShutdownFallbackShader()`,
  `ShaderWarmup::Shutdown()` and every `s_Data.*Shader.Reset()` have run — while the
  Vulkan backend releases the same set cleanly on the identical path. Filed as **#841**:
  a backend divergence in shader lifetime, not one of this class's scene-owned families.
  The instrument had been recording that answer for the whole life of the GL backend and
  discarding it at the one moment it mattered.

Red-checked against every way scan 1 can break: it reports exactly the four
species-(b) members against the unfixed tree and none against the fixed one, and
neither a declaration's own `= nullptr` initializer **nor an out-of-class static
member definition** (`Ref<Shader> ShaderLibrary::s_Fallback = nullptr;` in the
.cpp that pairs with the `static Ref<Shader> s_Fallback;` in the .h) counts as a
release. Both textually contain `= nullptr`; without excluding them, giving a
static an explicit null initializer — or simply *defining* a class static — would
be enough to "release" it, which would have made the check silently vacuous for
every class static in the engine.

And red-check it **in the compiled binary**, not only in a scratch copy of the
scan. Both halves of this guard were validated standalone first, but the *test's*
GL-handle branch still shipped broken: widening its regex to accept an
anonymous-namespace declaration (which is exactly what fixing the PBO looks like)
shifted the capture groups, and the body still read `match[1]` as the name — so it
searched for `glCreate...&<whitespace>`, matched nothing, and **skipped the entire
GL branch silently**. The test passed. It would have kept passing with the leak
reinstated. The scratch copy had the corrected indices, so only running the real
binary against a deliberately broken tree found it. Being a source-scan test makes
that cheap: delete the release, re-run the already-built binary, no rebuild needed.

## #841 resolved: not a species this document's scan covers, and not a code bug at all

The 115-shader survivor named above turned out to be neither scan 1's territory
(no owning static was misplaced) nor scan 2's (no missing `Shutdown()` call) — it
was `ShaderResourceRegistry::m_Shader`, a **member of a member**: `OpenGLShader`
embeds a `ShaderResourceRegistry` by value, and that registry's `SetShader()` had
been storing a strong `Ref<Shader>` back to the very `OpenGLShader` that owns it.
A shader's own resource registry keeping a strong reference to the shader is a
self-cycle neither scan can see — scan 1 only inspects *direct* static/lazy-handle
declarations, and this Ref lives two structs deep, reached only via
`Shader::Create()` → `InitializeResourceRegistry()`. Fixed by storing a non-owning
`const Shader*` instead (the registry can never outlive its enclosing shader — it
is not separately heap-allocated — so a raw back-pointer is safe by construction).
See `docs/agent-rules/component-serializer-codegen.md`-style "member of a member"
reasoning generalizes: scan 1's declaration-only reach is a documented limit, not
a bug in the guard.

**The much longer story was verifying it.** The fix read correct on every
re-derivation, yet four live rebuild-and-relaunch rounds each seemed to disprove
it — the survivor count didn't move, then live refcount tracing showed every
survivor pinned at exactly 1 with no reachable second owner, then a *different*
shader started reproducibly double-freeing on close. All of it was stale
incremental-build state, not the fix — see
[incremental-build-odr-staleness.md](incremental-build-odr-staleness.md) for the
full shape and the rule it generalizes to. The lesson worth carrying forward
specifically for *this* document's territory: a self-referential `Ref` through an
embedded member is now a fourth thing to check, beyond the three scans already
documented above, when a GPU-owning object survives every release call you can
find.
