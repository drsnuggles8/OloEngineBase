# KESTREL lives in the engine repo as a second project

Issue [#1727](https://github.com/drsnuggles8/OloEngineBase/issues/1727) set the project's north star
([north-star.md](../process/north-star.md)): **KESTREL**, a photoreal ski resort management game on real
mountains, with the milestone *KESTREL: vertical slice* (mid-2027). The user decided on 2026-11-21 that the
game is developed in the open, in this repository, beside `SandboxProject`, and that game and engine issues
share one queue ([decisions comment](https://github.com/drsnuggles8/OloEngineBase/issues/1727#issuecomment-7060665000)).

This ADR records that layout, the codename policy and the engine/game boundary, which are decided (§1–§3), and
proposes an answer to the question the layout leaves open: where the game's simulation code lives (§4).

## Status

- §1–§3: **Accepted** (the user's decisions on #1727).
- §4: **Proposed.** The user accepts or changes it in review of the PR that adds this file. Until then
  [#1731](https://github.com/drsnuggles8/OloEngineBase/issues/1731), which implements it, stays unstarted.

## 1. Layout

```
OloEditor/
  SandboxProject/   the engine's demo and test project (Sandbox.oloproj), unchanged
  KestrelProject/   the game (Kestrel.oloproj)
    Assets/         scenes, terrain, models, data tables, Scripts/ (Kestrel-Scripting C# assembly)
    Source/         the native game module, if §4 is accepted
```

- One repository, one CMake tree, one CI. The editor, `OloRuntime` and `OloServer` open either project; which
  one is data (a `--project` flag, then the editor's recent project, then SandboxProject), never a second
  hard-coded path ([#1730](https://github.com/drsnuggles8/OloEngineBase/issues/1730)).
- `SandboxProject` stays the engine's project: engine tests and fixtures keep mounting it
  ([ADR 0003](0003-functional-tests-mount-editor-asset-root.md)). Engine tests never depend on KESTREL content.
- Drift content stays in SandboxProject. Drift work waits until after the milestone (north-star.md, *Out of
  scope*).

## 2. Codename policy

The game has no public title yet. Paths, targets, the milestone and labels use the codename:
`OloEditor/KestrelProject/`, `Kestrel.oloproj`, `Kestrel-Scripting`, the milestone `KESTREL: vertical slice`, the
label `kestrel`. The concept is not secret: the #1727 comment and the issues stay public. When a title is chosen,
user-facing strings change; the paths and identifiers do not need to.

## 3. The engine/game boundary

**Anything another game could use goes in `OloEngine/`. Game rules, content, balancing and data go in
`KestrelProject`.**

| Goes in `OloEngine/` | Goes in `KestrelProject` |
|---|---|
| elevation import (GeoTIFF, reprojection), terrain LOD and streaming, forest far-field | the chosen mountain's data, tree species and tree line |
| calendar clock, climate-driven weather generator | the valley's climate table |
| a generic snowpack grid (accumulation, settling, melt, wind drift) | skier traffic, snow-quality effects on guests, opening rules |
| strategy camera rig, build-command framework with undo, rope sag helper, terrain overlays | buildings, lift types, trail rules, the overlay set |
| crowd rendering, agent-LOD scheduling if it generalises | guests, staff, economy, the tech-tree data, lifts as machines |
| runtime UI capabilities (binding, tables, theming) | HUD, menus, panels, the KESTREL style |

A critical-path issue says which of its parts is engine and which is game. When in doubt, the test is whether a
second, different game (a city builder, a survival game) would want the piece unchanged.

## 4. Where the game's simulation code lives (proposed)

The slice simulates thousands of skiers (target 5 000 on a busy day,
[#1747](https://github.com/drsnuggles8/OloEngineBase/issues/1747)), lifts, a whole-mountain snowpack and an
economy, inside a 16.6 ms frame on a 6-core CPU, next to the renderer. That budget decides the question.

| | A. Engine C++ systems in `OloEngine/` | B. The project's C# assembly | C. A native game module in `KestrelProject/Source/` |
|---|---|---|---|
| Per-tick cost for thousands of agents | native | every component read or write from C# is a Mono internal call; per-entity `OnUpdate` dispatch; GC pressure from per-agent objects | native |
| Task system and `SystemScheduler` (declared reads/writes, parallel systems) | yes | no: scripts run on the main thread inside the script update | yes |
| Boundary (§3) | broken: game rules compiled into every engine consumer, SandboxProject and the tests | clean | clean, by directory and target |
| Iteration speed | engine rebuild for each rule change | hot reload | rebuild of one static library and relink |
| Serialization, inspector, bindings | OloHeaderTool, as today | hand-written C# fields | OloHeaderTool, once it scans the module's headers |
| New engine work | none | none | a CMake convention, a registration entry point, OloHeaderTool scanning a second source root |

**Recommendation: C, a native game module, with C# kept for scenario scripting, tutorials and UI glue.**

- B fails the deciding constraint: the per-agent update must be data-oriented and parallel on the task system,
  and C# scripts reach neither the scheduler nor the worker threads. This is reasoning from the engine's
  structure, not a measurement made here; if the user prefers B, the first step is to measure 5 000 agents
  updated through internal calls against the same loop in C++ before the guest simulation is built on it.
- A meets the budget but puts ski-resort rules into the engine library every other consumer links, which is the
  boundary §3 exists to prevent.
- C costs one engine task, [#1731](https://github.com/drsnuggles8/OloEngineBase/issues/1731): a project with
  `Source/CMakeLists.txt` builds a static library that the three apps link when that project is configured
  (`OLO_GAME_PROJECT`), a `RegisterGameModule` entry point, and OloHeaderTool scanning the module so game
  components get serialization, the inspector and the script bindings the same way engine components do. Static
  linking first; a hot-reloadable DLL is a later, separate decision.

The game-simulation issues ([#1743](https://github.com/drsnuggles8/OloEngineBase/issues/1743),
[#1745](https://github.com/drsnuggles8/OloEngineBase/issues/1745),
[#1751](https://github.com/drsnuggles8/OloEngineBase/issues/1751),
[#1752](https://github.com/drsnuggles8/OloEngineBase/issues/1752),
[#1755](https://github.com/drsnuggles8/OloEngineBase/issues/1755)) list #1731 in `blocked_by`. If the user
chooses A or B instead, #1731 closes as not planned and those edges are removed.
