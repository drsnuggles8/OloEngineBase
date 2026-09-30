# OloEngine documentation

Index of everything under `docs/`. Most subsystem code comments link here by path,
so keep references in sync when moving a file (`git grep "docs/<name>"` before a rename).

**Adding a doc? Add it here too.** This index drifted to 5-of-46 on `agent-rules/` once already;
an unlisted doc is an unread doc.

## agent-rules/ — guidance for AI agents working in this repo

Two genres: **postmortems** (one real failure each) and the **`notes-*.md` reference guides**
(accumulated per-subsystem gotchas). Two indexes, deliberately:

- **[agent-rules/README.md](agent-rules/README.md) Part A** — indexed by **subsystem**, one sentence
  each. Use this when you know what you're *touching*.
- **[agent-rules/README.md](agent-rules/README.md) Part B** — the same set indexed by **failure
  mode** (green-but-wrong, one-contract-several-mirrors, silent drop, your-instrument-is-lying,
  ordering/lifetime, never-actually-called). Use this when you know what you're *doing* but not
  what can go wrong.

Read the relevant file before non-trivial work; don't duplicate its content into `CLAUDE.md`.

## Testing

- [testing.md](testing.md) — the canonical testing opinion doc: *why* we test what we test, the renderer L1–L11 pyramid + Functional axis, value heuristic, anti-patterns, classification. The hub the test suite, CI, and `agent-rules/testing-architecture.md` all point at.
- `test-catalogue.{renderer,functional,unit}.md` — **generated & git-ignored** per-file catalogues, rendered from `test_catalogue.json` + in-file `// OLO_TEST_LAYER` tags by `OloEngine/tests/scripts/generate_test_catalogue.py`. Not tracked; regenerate on demand.
- [testing/restir-pt-1211-evidence.md](testing/restir-pt-1211-evidence.md) — numerical validation of the experimental ReSTIR PT prototype (#1211): what it supports, what makes it stand down, and the measured bias and variance.
- [testing/restir-pt-1211-live.md](testing/restir-pt-1211-live.md) — the same prototype in motion in a live Vulkan editor, and what it still gets wrong.

## Renderer — status, support contract and roadmap owners

Where the renderer's current state is written down, and which issue owns what is left. Read the
contract docs before claiming support; read the issues before claiming a gap is unowned.

- [guides/renderer-support-matrix.md](guides/renderer-support-matrix.md) — which geometry, material, backend, path, sample-count and upscale combinations the renderer supports, approximates or refuses, checked against the executable rows in `Renderer/Support/RendererSupportRows.h` (#1334).
- [analysis/renderer-docs-reconcile-1357.md](analysis/renderer-docs-reconcile-1357.md) — the September 2026 reconcile of renderer comments and docs against the code (#1357): each correction with its evidence, the owners, and why convex reflection weights were not an energy proof.
- **Roadmap owners** (checked 2026-09-29): #1359 is the single index for the September 2026 renderer review. #654 owns the virtual-geometry gap list (#1143, #1152–#1155, #1049). #979, the hybrid-to-path-traced roadmap, closed on 2026-09-22 with every phase delivered and stays as the design record; its open follow-ups are #1346, #1355 and #1356. #812, the post-#691 Vulkan umbrella, was closed on 2026-09-29 because it wrapped only #805, which stands on its own.

## guides/ — subsystem & tooling how-tos

- [guides/ai-goap.md](guides/ai-goap.md) — GOAP planner / AI action system.
- [guides/ai-perception.md](guides/ai-perception.md) — AI perception (sight/sound/awareness).
- [guides/cinematic-sequencer.md](guides/cinematic-sequencer.md) — cinematic sequencer / timeline.
- [guides/gltf-material-extensions.md](guides/gltf-material-extensions.md) — what the glTF transmission / IOR / volume extensions import and render, and where that stops.
- [guides/skin-material-profiles.md](guides/skin-material-profiles.md) — what a `.oloskin` profile holds, in which units and colour space, how a skin material names one, and how to look at the four outputs a skin surface exposes.
- [guides/foliage-leaf-material.md](guides/foliage-leaf-material.md) — the vegetation material: which fields a foliage layer's leaf material has, why its transmission is split into a shadowed direct half and an environment half, what the distant impostor keeps and what it loses, and how to look at the transmission term on its own.
- [guides/foliage-hierarchical-wind.md](guides/foliage-hierarchical-wind.md) — authoring grass and woody plants with stiffness, branch and leaf-flutter wind weights, and what zero weights keep from the legacy sway.
- [guides/foliage-interaction-bending.md](guides/foliage-interaction-bending.md) — `FoliageInteractionComponent`: how a character, animal or boulder presses foliage aside.
- [guides/skin-transmission.md](guides/skin-transmission.md) — the thin-region transmission term: the unit chain from an authored thickness in metres to an optical depth, the three-premise argument for why it cannot double-count the diffusion beside it, and the four ways it silently does nothing.
- [guides/skin-diffusion.md](guides/skin-diffusion.md) — how the screen-space skin scattering works, which diffusion profile it uses and the reference comparison that chose it, what its radius is in world units, and exactly where it stops.
- [guides/skin-layered-specular.md](guides/skin-layered-specular.md) — the two-lobe skin specular, the pore/normal filtering that stops it sparkling, and the expression-driven detail band: which five fields an author sets, the measured comparison that chose the model, why the variance is read from screen space and not from the normal map, and why the detail follows the APPLIED morph weights.
- [guides/skin-oral-surfaces.md](guides/skin-oral-surfaces.md) — lips, gums, tongue and teeth: the wet coat that TAKES energy from the tissue rather than adding to it, the cavity weight that stops a closed mouth glowing under a back light, why teeth are a matter of authoring rather than a second code path, and the two limits (no coat under IBL, no oral anatomy in this repo).
- [guides/eye-cornea-iris.md](guides/eye-cornea-iris.md) — the eye: the corneal refraction that moves the iris you see, the left/right convention that needs no mirrored asset, why the index is the aqueous humour's and not the cornea's, why the tear line is #1245's wet coat rather than a second code path, and why a front-on screenshot cannot tell you whether any of it works.
- [guides/groom-animals.md](guides/groom-animals.md) — every groom child on three moving subjects (two horses, a human head): turn TAA on before judging it, why both animals are horses, and how the coats, bindings and scene are regenerated rather than authored.
- [guides/dog-showcase.md](guides/dog-showcase.md) — the groom system's close-up subject: one furred dog on a lawn, what each entity is, how the body, coat and scene are regenerated, what the headless twin asserts, and where the cost record lives.
- [guides/skin-digital-human.md](guides/skin-digital-human.md) — running skin, eyes and mouth on ONE subject: a complete face spends the whole seven-slot skin-profile budget and an eighth profile silently renders as not-skin, the material debug views are deferred-only, how to switch the three lighting rigs without changing two things at once, and why the decomposition is checked by cross-talk rather than by a sum.
- [guides/input-action-maps.md](guides/input-action-maps.md) — input action maps & contexts.
- [guides/localization.md](guides/localization.md) — localization & `LocalizedTextComponent`.
- [guides/mcp-diagnostics-server.md](guides/mcp-diagnostics-server.md) — the read-only MCP diagnostics server (tools, resources, prompts, attach flow).
- [guides/oloctl.md](guides/oloctl.md) — `oloctl`, the CLI frontend generated from the automation registry (spelling rules, arguments, exit codes, the closed write path).
- [guides/perf-stress-scenes.md](guides/perf-stress-scenes.md) — perf stress-scene generator + measurement runbook.
- [guides/renderer-benchmarks.md](guides/renderer-benchmarks.md) — renderer benchmark scenes, capture manifests and hero frames (#974): the manifest schema, the AOVs a capture can take, and the ones it declares unavailable.
- [guides/benchmark-reference-fixtures.md](guides/benchmark-reference-fixtures.md) — the five head, groomed-animal and vegetation reference fixtures (#1239) and the provenance every benchmark asset carries; the measured baseline is in [guides/benchmark-reference-fixtures-baseline.md](guides/benchmark-reference-fixtures-baseline.md) (append, never overwrite).
- [guides/integrated-renderer-benchmark.md](guides/integrated-renderer-benchmark.md) — the integrated renderer workload (#1338): how `IntegratedRenderer.olo` is generated and captured; the measurements are in [analysis/integrated-renderer-budgets-1338.md](analysis/integrated-renderer-budgets-1338.md).
- [guides/unified-image-layouts-validation.md](guides/unified-image-layouts-validation.md) — validating Vulkan unified image layouts (#1181) against both the clean baseline and the runtime-disabled policy before attributing a change to them.
- [guides/player-camera-rigs.md](guides/player-camera-rigs.md) — reusable player + camera rigs (first-person / third-person follow spring arm).
- [guides/procedural-terrain.md](guides/procedural-terrain.md) — procedural terrain generation.
- [guides/ui-system.md](guides/ui-system.md) — runtime UI system.
- [guides/video-playback.md](guides/video-playback.md) — video playback component.

## design/ — design rationale & roadmap docs (cited from source for *why*)

- [design/water-ocean.md](design/water-ocean.md) — design record for the shipped water/FFT-ocean system. **Cited by ~57 code comments via bare `water-ocean.md §X.Y`** — keep this basename AND the section numbering stable; add new sections at the end. Open work lives in issues #1033-#1035, not here.
- [design/animation-retargeting.md](design/animation-retargeting.md) — animation retargeting (humanoid bone roles, rebasing).
- [design/locomotion.md](design/locomotion.md) — character locomotion (issue #631).
- [design/soundgraph-metasounds.md](design/soundgraph-metasounds.md) — SoundGraph / MetaSounds-style audio graph refactor.
- [design/restir-gi-reconnection-shift.md](design/restir-gi-reconnection-shift.md) — ReSTIR GI (#1169): the measure convention, the reconnection shift and its Jacobian, the DDGI hand-off, and why the DI chain is not renameable. **Cited from source as `restir-gi-reconnection-shift.md §X`** — keep the section numbering stable.
- [design/restir-pt-shift-mappings.md](design/restir-pt-shift-mappings.md) — ReSTIR PT (#1211): suffix measures, reversible shifts and resampling, written before the code. The prototype it describes is experimental and off by default.
- [design/vulkan-rt-vegetation.md](design/vulkan-rt-vegetation.md) — Vulkan ray-query vegetation: ray geometry comes from canonical plants and the shared raster wind producer, never a rest-pose canopy or a camera-facing card.

> Roadmap docs describe intended/future work — verify "doneness" against the **code**, not these files.

## process/ — how we run the project

- [process/issue-scoring.md](process/issue-scoring.md) — the rubric for rating issues/tasks (WSJF-derived, engine-tuned: Capability/Craft/Stability/Decay over Effort, plus Learning/Fun). Drives `/start-work` task picking; raw axes live in an `olo-score` block in each issue body, ranked on demand by `scripts/issue_scores.py` (nothing derived is stored).
- [process/task-loop.md](process/task-loop.md) — the **worker-session contract**: what a session started from `HANDOVER.md` does, from implementing through self-review, PR, CI and CodeRabbit, to a green thread-clean PR. Absorbed the former `/finish-pr` and `/pr-status` commands. Stops short of merging.

The three workflow slash commands live in [`.claude/commands/`](../.claude/commands/) and are versioned with the repo because they reference repo content: `/start-work` (pick + scaffold), `/cleanup-worktree` (reclaim merged worktrees, heal the registry), `/resume-worktrees` (reopen windows).

## analysis/ — quality & code-health reports

- [analysis/dead-code.md](analysis/dead-code.md) — dead-code analysis (LOC reduction).
- [analysis/perf-stress-findings-2026-07.md](analysis/perf-stress-findings-2026-07.md) — perf stress-scene battery findings (2026-07-03).
- [analysis/sonarqube-rules.md](analysis/sonarqube-rules.md) — SonarCloud rule tuning suggestions & high-volume-rule decisions.
- [analysis/ue5.8-hzb-occlusion-analysis.md](analysis/ue5.8-hzb-occlusion-analysis.md) — UE 5.8 HZB occlusion culling, source-level analysis.
- [analysis/automation-transactability-boundary.md](analysis/automation-transactability-boundary.md) — which automation commands may be transaction steps (#1127): only those that declare how they are undone and that they are main-thread safe.
- [analysis/integrated-renderer-budgets-1338.md](analysis/integrated-renderer-budgets-1338.md) — integrated renderer measurements (#1338, 2026-09-23), with source, build and load qualifiers; historical evidence, annotated with current owners.
- [analysis/material-reference-validation-1255.md](analysis/material-reference-validation-1255.md) — skin, leaf and fibre models judged against independently computed references (#1255), and the limits written down where none exists.
- [analysis/multi-animal-scheduling-budgets-1258.md](analysis/multi-animal-scheduling-budgets-1258.md) — multi-animal scheduling and budgets (#1258), every number an assertion in a committed test.
- [analysis/vulkan-material-selection-805.md](analysis/vulkan-material-selection-805.md) — Vulkan per-instance material selection through the GPU Scene (#805, ADR 0011 amendment (101)).
- [analysis/vulkan-parallel-recording-1013.md](analysis/vulkan-parallel-recording-1013.md) — Vulkan parallel command recording measurements (#1013): elapsed region time kept separate from summed worker time.
- Groom (epic #1223) measured comparisons: [strand visibility (#1246)](analysis/groom-strand-visibility-1246.md), [fibre scattering (#1247)](analysis/groom-fibre-scattering-1247.md), [coat self-shadowing (#1248)](analysis/groom-coat-self-shadowing-1248.md), [guide simulation (#1250)](analysis/groom-guide-simulation-1250.md), [representation LOD (#1252)](analysis/groom-representation-lod-1252.md), [ray-tracing proxies (#1253)](analysis/groom-rt-proxies-1253.md).
- [analysis/renderer-docs-reconcile-1357.md](analysis/renderer-docs-reconcile-1357.md) — the renderer documentation reconcile (#1357); see the Renderer section above.

## ops/ — build & deployment

- [ops/build.md](ops/build.md) — full Windows / Linux / WSL build matrix.
- [ops/deployment.md](ops/deployment.md) — OloServer deployment / packaging.
- [ops/shipping.md](ops/shipping.md) — shipping a game to Steam (#894): what of the built folder ships, how it is identified and uploaded, and what still needs a person.
- [ops/self-hosted-gpu-runner.md](ops/self-hosted-gpu-runner.md) — the self-hosted AMD GPU CI runner.
- [ops/self-hosted-host-hygiene.md](ops/self-hosted-host-hygiene.md) — the box behind the runners: the update timer must not reboot under a job, the GPU resets during the suite, one host is shared.
- [ops/self-hosted-linux-toolchain.md](ops/self-hosted-linux-toolchain.md) — both Linux arms take clang-23 and LLD 23 from the same LLVM release tarball at `/opt/llvm-23.1.0` (the box keeps its own ICU 70 beside it); on the box a missing pin or sanitizer runtime warns and falls back, never installs.

## adr/ — architecture decision records

- [adr/0001-functional-tests-as-separate-axis.md](adr/0001-functional-tests-as-separate-axis.md) — Functional tests live on a separate axis from the renderer testing pyramid.
- [adr/0002-headless-tick-default-for-functional-tests.md](adr/0002-headless-tick-default-for-functional-tests.md) — headless `Scene::OnUpdateRuntime` is the default tick model for Functional tests.
- [adr/0003-functional-tests-mount-editor-asset-root.md](adr/0003-functional-tests-mount-editor-asset-root.md) — Functional fixtures mount an isolated copy of the editor asset root.
- [adr/0004-lock-free-allocator-singleton-init.md](adr/0004-lock-free-allocator-singleton-init.md) — lock-free link allocator uses a never-destructed magic static.
- [adr/0005-mcp-script-tools-lua-sandbox.md](adr/0005-mcp-script-tools-lua-sandbox.md) — script-defined MCP tools run in a capability-stripped `sol::state`, not engine bindings.
- [adr/0006-progression-databases-as-assetmanager-assets.md](adr/0006-progression-databases-as-assetmanager-assets.md) — progression data ships as AssetManager asset types, not static registries.
- [adr/0007-ddgi-hit-point-cache-gather.md](adr/0007-ddgi-hit-point-cache-gather.md) — DDGI gathers rays from a relit static hit-point cache, not per-frame cube rasterization.
- [adr/0008-no-mcp-endpoint-in-cooked-builds.md](adr/0008-no-mcp-endpoint-in-cooked-builds.md) — no MCP endpoint in cooked builds; deferred behind seven preconditions.
- [adr/0009-scripting-bindings-from-reflection-emitted-schema.md](adr/0009-scripting-bindings-from-reflection-emitted-schema.md) — scripting bindings come from a reflection-emitted, language-neutral schema.
- [adr/0010-vulkan-rhi-heap-bindless-only.md](adr/0010-vulkan-rhi-heap-bindless-only.md) — add a Vulkan backend alongside GL 4.6: heap-bindless only, no legacy descriptor-set path.
- [adr/0011-rhi-neutral-resource-and-binding-model.md](adr/0011-rhi-neutral-resource-and-binding-model.md) — API-neutral RHI resource/binding model: identity vs binding address vs native handle. The decisions plus an index of the amendments, numbered up to (101) (what each decided, whether it still binds); the amendment bodies are in [adr/0011-amendments.md](adr/0011-amendments.md).
- [adr/0012-adopt-the-ue-container-library-for-engine-owned-data.md](adr/0012-adopt-the-ue-container-library-for-engine-owned-data.md) — adopt the UE container library for engine-owned data; close the half-adopted state by growing usage.
- [adr/0013-destructible-debris-asset-swap-not-runtime-fracture.md](adr/0013-destructible-debris-asset-swap-not-runtime-fracture.md) — destructible objects swap in pre-authored debris assets; no runtime mesh fracture.
- [adr/0014-visual-script-execution-model.md](adr/0014-visual-script-execution-model.md) — visual scripting pushes exec tokens with synchronous branch descent; §2 is superseded by the visual-script ADR 0023.
- [adr/0015-editor-is-windows-only-linux-ships-runtime-only.md](adr/0015-editor-is-windows-only-linux-ships-runtime-only.md) — Linux ships as a native runtime, not under Proton, and the editor stays Windows-only.
- [adr/0016-pbr-closure-v2-versioned-evaluate-sample-pdf-contract.md](adr/0016-pbr-closure-v2-versioned-evaluate-sample-pdf-contract.md) — the material closure carries a version (`PBRModel`) and a pinned `Evaluate`/`Sample`/`Pdf` contract shared by raster, the CPU reference tracer and the GPU path tracer.
- [adr/0017-windows-ci-critical-path-measure-before-a-self-hosted-runner.md](adr/0017-windows-ci-critical-path-measure-before-a-self-hosted-runner.md) — the Windows CI critical path is measured on a writable cache before any self-hosted Windows runner is built, and never on the interactive workstation.
- [adr/0018-gaussian-splats-gpu-ordering-and-merge-lod.md](adr/0018-gaussian-splats-gpu-ordering-and-merge-lod.md) — Gaussian splats order per view on the GPU and coarsen by merging; a CPU sort and a selection budget are both dead ends.
- [adr/0019-windows-ci-self-hosted-routing-lands-switched-off.md](adr/0019-windows-ci-self-hosted-routing-lands-switched-off.md) — the Windows jobs can be routed to a self-hosted runner behind a `vars.` kill switch and a fork guard; the switch stays off until a runner exists and both paths are measured.
- [adr/0020-reflection-tier-selection-contract.md](adr/0020-reflection-tier-selection-contract.md) — the four reflection sources are competing estimates of one quantity; they composite bottom-up by confidence with the bottom tier pinned at 1, so the weights sum to exactly one and a double-count is impossible by construction.
- [adr/0021-structural-connectivity-is-derived-from-collider-adjacency.md](adr/0021-structural-connectivity-is-derived-from-collider-adjacency.md) — a destructible structure's support graph is derived from collider adjacency, not authored per piece; the component carries only anchor/timing policy; same-course neighbours support each other so a lintel can cantilever, but the flood charges for each sideways step, because free lateral transfer makes a wall all-or-nothing.
- [adr/0022-reference-tracer-owns-its-sampling-model.md](adr/0022-reference-tracer-owns-its-sampling-model.md) — the reference path tracer's scene description may be as rich as a bake needs, but its sampling model is its own (level 0, never the raster's mip chain) and is pinned by tests that do not involve the raster path; parity fixtures stay in the subset both worlds express, so they keep pinning what they always pinned.
- [adr/0023-virtual-geometry-is-ray-traced-through-a-fixed-proxy.md](adr/0023-virtual-geometry-is-ray-traced-through-a-fixed-proxy.md) — a cluster LOD DAG cannot be a BLAS (the cut is per view, per frame), so each virtual-mesh part is ray-traced through one FIXED proxy built from the DAG's coarsest cut: watertight and view-independent by construction, classified Static, built once. Whatever still gets no proxy stays counted in `GPUSceneUnsupportedCategory::Virtualized`, now per part.
- [adr/0023-visual-script-exec-stack-owned-by-the-vm.md](adr/0023-visual-script-exec-stack-owned-by-the-vm.md) — visual scripting: the exec stack moves onto the instance, superseding ADR 0014 §2. (Two ADRs share number 0023 and two share 0024; they are cited by file name, so neither pair is renumbered.)
- [adr/0024-material-kind-is-not-the-closure-version.md](adr/0024-material-kind-is-not-the-closure-version.md) — a material carries three independent selectors (what the surface IS, which version of the closure evaluates it, and which version of the skin transport its profile was authored against); none is derived from another, and the G-Buffer headroom that separation costs.
- [adr/0024-skinned-virtual-geometry-stays-on-the-hardware-rasterizer.md](adr/0024-skinned-virtual-geometry-stays-on-the-hardware-rasterizer.md) — skinned virtual-geometry clusters never go to the software rasterizer; they stay on the hardware route.

## bug-investigations/ — postmortems & deep-dives

- [bug-investigations/fog-ubo-binding-knockout-investigation.md](bug-investigations/fog-ubo-binding-knockout-investigation.md) — flaky `FogVisualEvidenceTest` (#446): a persistent UBO bound only in its ctor gets its slot knocked to 0 by cross-test buffer churn; re-bind on upload.
- [bug-investigations/nanite-foliage-white-fringe-investigation.md](bug-investigations/nanite-foliage-white-fringe-investigation.md) — Nanite (#629): Sponza foliage white-fringe investigation.
- [bug-investigations/rendergraph-ghosting-investigation.md](bug-investigations/rendergraph-ghosting-investigation.md) — render-graph ghosting investigation.
