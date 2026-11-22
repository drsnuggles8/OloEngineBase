# Renderer memory budget: admission, reduction and recovery

The renderer admits each frame's quality configuration against a GPU memory budget before it
allocates anything for it. When the request does not fit, quality is reduced along one fixed
ladder until it does, and it climbs back once the pressure has gone. The request itself is never
edited: every surface that reports quality shows the **requested** configuration beside the
**realized** one, and why they differ. Issue #1343; the accounting it stands on is
[renderer-memory-accounting.md](../agent-rules/renderer-memory-accounting.md) (#1342).

Code: the policy is `Renderer/RendererMemoryBudget.{h,cpp}` (plain data, unit-tested without a
device); Renderer3D feeds and applies it in `Renderer/Renderer3DMemoryAdmission.cpp`.

## The budget

| Source | When | Reported as |
|---|---|---|
| `Renderer3D::SetMemoryBudgetOverride`, `olo_memory_budget_set` | tests, live pressure injection | `injected` |
| `OLO_RENDERER_MEMORY_BUDGET_MB` | set in the environment; the only budget OpenGL can have | `configured` |
| `VK_EXT_memory_budget`, device-local heaps | Vulkan, extension present | `osReported` |
| VMA's fixed share of the heap | Vulkan, extension absent | `allocatorHeuristic` |
| none | OpenGL with no variable | `none`: nothing is enforced, reason `budgetUnknown` |

Usage is charged as **resident** bytes: live plus retiring. Memory an owner released stays
charged until the backend's deletion queue frees it (two frames on both backends). On Vulkan the
OS figure also counts what the tracker cannot see (the swapchain, the ImGui backend). A reserve of
`max(64 MiB, 5 %)` of the budget is kept free for in-flight replacements.

## The ladder

Level *N* is the first *N* rungs applied, in this order (`kMemoryLadder`). A rung that has
nothing to remove on a view (MSAA on a forward path, a technique that is off) changes nothing and
is not reported.

| # | Rung | What it gives back |
|---|---|---|
| 1 | `transientPoolSlack` | idle pooled transients beyond one frame's demand |
| 2 | `historyResolution` | the SSGI chain and its four histories go to half resolution |
| 3–5 | `msaa4`, `msaa2`, `msaaOff` | Deferred G-Buffer samples (and its resolve target) |
| 6 | `optionalDetailPools` | optional groom / vegetation detail falls to its pinned floor (#1257's governor) |
| 7–16 | `restirGIFallback` … `bloomFallback` | one optional technique at a time: ReSTIR GI, ReSTIR DI, ReSTIR PT, RT reflections, RT shadows, SSGI, SSR, motion blur, depth of field, bloom |

There is no history-**precision** rung: every registry history is either RGBA16F already or a
running sum or packed reservoir that half floats cannot hold. The path tracer is counted but never
removed, because it replaces the lit image rather than adding to it.

## The minimum complete image

Every level is a complete image. No rung removes geometry, changes the render path, or changes the
display or scene resolution. A removed technique leaves its fallback in place:

| Removed | What draws instead |
|---|---|
| ReSTIR GI, SSGI | DDGI where present, otherwise image-based ambient |
| ReSTIR DI, RT shadows | analytic lights with shadow maps |
| ReSTIR PT | the raster lit scene |
| RT reflections, SSR | image-based reflections |
| motion blur, depth of field, bloom | nothing; the image without the effect |
| optional groom / vegetation detail | the pinned card / coarse floor |

The last level is the **minimum complete image**. When even that does not fit, the renderer keeps
drawing it and reports `overBudgetAtFloor` rather than dropping more.

## When it decides

- **Once per physical frame**, before any view of the frame allocates. One level applies to every
  view, so two views share one budget.
- **On a resize**, before the new targets are allocated (`Renderer3D::OnWindowResize`). A G-Buffer
  the new size is admitted at another sample count for is dropped instead of being resized.
- **On a hot reload, from the moment it allocates.** `Texture2D::Reload` decodes and allocates in
  one step, so a reload is not admitted beforehand: its old storage stays charged while it retires,
  the overshoot is the reload's own new bytes, and the next frame's decision reduces quality.
  Admitting it beforehand means deferring the reload, a behaviour call tracked in #1764. Shader
  reload replaces programs and pipelines, which hold no budgeted memory of note.
- **Down at once, up slowly.** A reduction is taken the frame pressure is seen, then nothing is
  judged for a few settle frames while the old storage retires. A climb needs headroom beyond the
  reserve and a recovery band for `RecoveryFrames` consecutive frames (`recoveryHold` while
  timing). A climb that meets pressure again is undone, and the next climb waits twice as long.
  A descent that gave back more than the model predicted (a technique's transients are only
  measurable) charges the difference to every later climb, so the climb is not tried just to be
  undone.
- **A descent that itself allocates** (fewer samples is a new G-Buffer) is staged through a deeper
  level when the old and new storage would not fit together, then climbs to the target once the
  old storage has retired.

## Reading it

| Where | What |
|---|---|
| `olo_memory_report` → `admission` | `reason`, `level` of `floorLevel`, `activeRungs`, the budget and its source, predicted steady and peak bytes, recovery progress, counters, and per view `requested` beside `realized` |
| Renderer Memory window → Physical report | the same, with the realized line coloured when reduced |
| Statistics → Memory | `Quality: as requested` or `Quality: REDUCED by the memory budget (…)` |
| Renderer settings → Quality preset | a warning under the preset when the frame is reduced |
| `olo_representation_streaming_stats` | `optionalDetailWithheld` |
| the log | one `[MemoryAdmission] level a -> b …` line per change, never one per frame |

Reasons: `admitted`, `budgetUnknown`, `deviceBudget` (the request does not fit), `transitionOverlap`
(it would fit once the old resources retire, but not beside them), `recoveryHold`,
`overBudgetAtFloor`.

## Driving it

- Live: `olo_memory_budget_set { megabytes: 2048 }`, then `olo_memory_report` a few frames later;
  `olo_memory_budget_set { clear: true }` removes the injected budget, and the next source in the
  table above applies: `OLO_RENDERER_MEMORY_BUDGET_MB` when it is set, otherwise the device's.
- `OLO_RENDERER_MEMORY_ADMISSION_OFF=1` admits everything: the A/B control for a frame that looks
  reduced.
- Tests: `Renderer3D::SetMemoryBudgetOverride` and `SetMemoryAdmissionPolicy` (short clocks).
  `RendererMemoryBudgetTest.cpp` pins the policy; `RendererMemoryAdmissionEvidenceTest.cpp` runs it
  on real frames in both test processes.

## Measured budgets

RTX 4090, Windows 11, Debug build, 2026-11-22, the three-cube evidence scene with SSGI, SSR and
bloom on the Deferred configurations
(`RendererMemoryBudgetMeasurement.LowMemoryAndHighResolutionMsaaConfigurationsOnThisDevice`, which
writes `assets/tests/memory/MemoryBudgets_<Backend>.json`). Resident bytes include the renderer's
fixed base (shadow maps, IBL, GPU Scene, other resident state of the test process), about 245 MiB
on OpenGL and 300 MiB on Vulkan. OpenGL bytes are format estimates; Vulkan's are VMA's committed
sizes.

| Configuration | Requested GL / Vulkan | Minimum complete image GL / Vulkan | Under 4 GiB (Vulkan) | Under 2 GiB (both) |
|---|---|---|---|---|
| Forward 720p, Performance upscale | 280 / 333 MiB | 279 / 332 MiB | admitted | admitted |
| Deferred 1080p, 4x MSAA | 1 140 / 1 328 MiB | 471 / 601 MiB | admitted | admitted |
| Deferred 2160p, 4x MSAA | 3 914 / 4 397 MiB | 1 235 / 1 568 MiB | level 4: 2x MSAA, half-res SSGI, 3 133 MiB | level 13: no MSAA, no SSGI or SSR, 1 319 / 1 656 MiB |

The device's own budget here is 23 370 MiB (`osReported`), so a 4090 admits every configuration
above unreduced. The 2 GiB and 4 GiB rows are what an 8 GB or 4 GB card would leave a game after
the OS, the compositor and the application's own assets, injected through the override.
