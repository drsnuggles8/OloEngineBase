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
