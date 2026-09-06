# Cross-test renderer state

Two rules, in the order they cost us time:

1. **Never shut down a process-wide singleton you did not start.** `Renderer3D::Init` owns
   `CommandDispatch`, `GPUPassTimerPool`, `ParticleBatchRenderer` and `MeshPrimitives`. A test that
   tears one down leaves every later rendering test in the process running against a dead one. Guard
   every `Shutdown()` with whether *you* brought it up.
2. **Leave the process-global renderer configuration as you found it.** The rendering path, the
   settings structs and the culling toggles are all one static. A guard now restores these
   automatically, so a leak is repaired rather than propagated — but it is still a bug in your test,
   and the run names you in a `[ RENDERER STATE ]` summary.

Rule 1 is what actually caused issue #1074. Rule 2 is what everyone assumed had caused it, including
the issue itself; the guard built to enforce it found 212 leaking tests and fixed **zero** failures.
That asymmetry is the most useful thing on this page.

## Why this class exists at all

`OloEngine-Tests.exe` run as ONE process shares a single `Renderer3D::s_Data`, one GL context and one
set of engine singletons across ~7,600 tests. **CI cannot see any of it**: `tests/CMakeLists.txt`
uses `gtest_discover_tests`, which registers every case as its own ctest entry, so CI runs one
process per test and no two of these tests ever share an address space. The monolithic run is the
only configuration where this class of bug exists, and it is not part of the PR gate.

## The failure signature, and why it points nowhere near the cause

The victim is a visual-evidence test whose feature-on and feature-off captures come out **identical**,
or whose differential reads ~0 where it expected a large one — `LightmapBleedRoom` wants
`meanAbsDiff > 3.0` and measures `0.0007`. In the case that was actually diagnosed,
`VirtualGeometryVisualEvidence` counted **107** red pixels where it needed 800: the sphere simply was
not drawn.

Three properties make it expensive:

- **The victim passes in isolation**, 10/10 under its own `--gtest_filter`.
- **The engine logs are identical.** Same passes, same draws, same resource creation — only the
  pixels differ. There is nothing to grep for.
- **The poisoner is far away and needs company** (see below).

## The two traps that make bisection lie

### Trap 1 — a reduced configuration reproduces a *different* bug

The obvious method is "prefix + victims, binary-search the prefix". It converges quickly and it can
converge on the wrong thing. Running `AssetSceneLoad` immediately before the victim suites
reproduced 21 failures at every bisection step — but as **SEH access violations (0xc0000005)**,
while the full run has **zero** crashes. Removing suites had created a *new* use-after-free that had
nothing to do with the 102.

**So a bisection step must check that the victim fails the same WAY, not merely that it fails.**
Pin a signature first from the full run — here, "10 failures, 0 SEH, red-pixel counts 107–118" — and
reject any arm that does not match it. The bisection harness must print the mode, every time.

### Trap 2 — one poisoner is not enough

Neither ingredient reproduced anything on its own:

| Configuration | Result |
|---|---|
| all 65 victim suites together | passes |
| all 1150 non-victim suites + victim | passes |
| `FogVolumeSelfShadowVisualEvidenceTest` + victim | passes |
| `VulkanParallelRecordingDevice` + victim | crashes (wrong mode — trap 1) |
| both, plus ~150 suites of other work | **reproduces exactly** |

Both suites are individually *necessary* and neither is *sufficient*. A two-suite repro that does
not reproduce proves nothing; do not conclude "not reproducible" from a minimal pair.

## The actual defect (issue #1074)

`VulkanParallelRecordingTest.cpp` unconditionally called, in three different tests:

```cpp
GPUPassTimerPool::GetInstance().Shutdown();
CommandDispatch::Shutdown();
MeshPrimitives::Shutdown();
ParticleBatchRenderer::Shutdown();
```

Every one of those is owned by the renderer's lifecycle — `Renderer3DLifecycle.cpp` initializes the
first three at `Renderer3D::Init`, and `Renderer.cpp` owns `MeshPrimitives::Shutdown` in
`Renderer::Shutdown`. Run standalone (as CI runs it) the renderer is not up, the test owns them, and
the teardown is correct. Run in one process after the renderer has come up, it is a teardown of
somebody else's singletons.

### Why "just don't shut them down" is the wrong fix

The obvious fix — guard each `Shutdown()` on whether this test brought the singleton up — is wrong
here, and it fails loudly in two different ways:

1. **VMA aborts.** These singletons hold buffers allocated against the **Vulkan** device the test
   selects, and they must be released before it tears its memory blocks down. Skipping the shutdown
   trades the cross-test leak for a hard abort: *"Some allocations were not freed before destruction
   of this memory block"*.
2. **The test starts asserting on someone else's state.** Inheriting the renderer's timer pool
   instead of building its own left `GraphRecordsSharedUnboundUniformAndTimesOrderedPasses` reading
   18 accumulated pass timings where it asserts on exactly 2.

Restoring *eagerly* — shut down, then immediately republish — is wrong too, and this one is subtle:
it leaves GL-currency handles sitting in the dispatcher across the Vulkan suites that run next.
`VulkanPassSuiteTest.cpp` documents exactly that hazard ("live GL-currency objects with no setter to
re-home them onto this device"), and doing it moved the VMA abort from one suite to another.

**The fix that works is a lazy self-heal in the engine.** The teardown stays unconditional, and
`Renderer3D::BeginScene` calls `ReclaimSharedRenderState()`, which re-arms whatever was shut down —
`RepublishCommandDispatchBindings()` and the timer pool — at the moment the renderer is about to draw
and on the renderer's own backend:

```cpp
if (!CommandDispatch::HasUBOReferences())
    RepublishCommandDispatchBindings();
if (auto& passTimers = GPUPassTimerPool::GetInstance(); !passTimers.IsInitialized())
    passTimers.Initialize();
```

That closes a real engine gap rather than papering over a test: before this there was **no way back**
from a `CommandDispatch::Shutdown()` short of a full renderer restart. The same sequence is what
device-lost recovery or a runtime RHI switch would need, which is why it lives in the renderer.

So the rule's second half: **before guarding a `Shutdown()`, ask what backend owns the memory it
releases.** When the answer is "a device that is about to die", the teardown is not the thing to
change — give the survivor a way to re-arm itself instead.

The same file already had the right pattern, one line away, for a fourth singleton:

```cpp
const bool ownsFrameData = !FrameDataBufferManager::IsInitialized();
if (ownsFrameData) FrameDataBufferManager::Init();
// ... and only Shutdown() if OwnsFrameData
```

The fix applies that ownership test to its neighbours. This is the second time this exact defect has
been found here — a fixture calling `CommandDispatch::Shutdown()` dropped the UBO references
`Renderer3D` publishes once at `Init()`, so every later rendering test drew with no camera or
material UBO bound.

## The configuration guard (rule 2)

`RendererStateCheck` installs a GoogleTest listener beside the GL-error one. After every test it
restores the configuration captured at `OnTestStart`, counts the leak, names it in an end-of-run
`[ RENDERER STATE ]` summary, and — under `--olo-strict-renderer-state` — fails the polluting test at
its own source location.

Restoring centrally is deliberate: a fixture that saves "the two settings I remembered" leaves the
other nine free to leak, and every settings struct added later silently joins the hazard. The restore
ends with `ApplyRendererSettings()`, because putting the structs back is not the same as putting the
renderer back — the graph's `ActiveGraphPath` is what selects the pipeline.

**What it measured, and why that is worth knowing:** 212 of ~7,600 tests leak configuration, and all
of it is scene-published per-frame state — cloudscape, underwater fog, and the three water fields,
which the scene republishes every frame anyway. Zero tests leaked `RendererSettings`, a rendering
path, or a culling toggle. Restoring all of it changed the failure count by **0**. The
"fixture leaks a settings struct" hypothesis was wrong, and the guard is what proved it rather than
another five 20-minute bisection runs.

Adding a settings struct to `Renderer3D`? Add it to `RendererState::Snapshot` and the
`OLO_RENDERER_STATE_STRUCT_ENTRIES` X-macro — one entry drives capture, comparison and restore
together, because the original bug was a save list and a restore list that had drifted apart.
**Do not snapshot a derived value**: `s_Data.DepthPrepassEnabled` looks independent and is not
(`ApplyRendererSettings` recomputes it from `RendererSettings`), so snapshotting it reports a leak the
first time anything calls `ApplyRendererSettings` after `Init`. Snapshot the input, not the output.

## Running the monolithic configuration

```powershell
# The whole binary in ONE process — the only configuration where this exists.
.\build-cached\OloEngine\tests\Debug\OloEngine-Tests.exe

# Same, but fail every test that leaks renderer configuration.
.\build-cached\OloEngine\tests\Debug\OloEngine-Tests.exe --olo-strict-renderer-state
```

~20 minutes, and it holds the test binary — a concurrent rebuild dies `LNK1104`. It also runs
`AppLaunchSmoke`, which launches the real editor: expect windows to appear.
