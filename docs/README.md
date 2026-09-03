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

## guides/ — subsystem & tooling how-tos

- [guides/ai-goap.md](guides/ai-goap.md) — GOAP planner / AI action system.
- [guides/ai-perception.md](guides/ai-perception.md) — AI perception (sight/sound/awareness).
- [guides/cinematic-sequencer.md](guides/cinematic-sequencer.md) — cinematic sequencer / timeline.
- [guides/input-action-maps.md](guides/input-action-maps.md) — input action maps & contexts.
- [guides/localization.md](guides/localization.md) — localization & `LocalizedTextComponent`.
- [guides/mcp-diagnostics-server.md](guides/mcp-diagnostics-server.md) — the read-only MCP diagnostics server (tools, resources, prompts, attach flow).
- [guides/perf-stress-scenes.md](guides/perf-stress-scenes.md) — perf stress-scene generator + measurement runbook.
- [guides/player-camera-rigs.md](guides/player-camera-rigs.md) — reusable player + camera rigs (first-person / third-person follow spring arm).
- [guides/procedural-terrain.md](guides/procedural-terrain.md) — procedural terrain generation.
- [guides/ui-system.md](guides/ui-system.md) — runtime UI system.
- [guides/video-playback.md](guides/video-playback.md) — video playback component.

## design/ — design rationale & roadmap docs (cited from source for *why*)

- [design/water-ocean.md](design/water-ocean.md) — design record for the shipped water/FFT-ocean system. **Cited by ~57 code comments via bare `water-ocean.md §X.Y`** — keep this basename AND the section numbering stable; add new sections at the end. Open work lives in issues #1033-#1035, not here.
- [design/animation-retargeting.md](design/animation-retargeting.md) — animation retargeting (humanoid bone roles, rebasing).
- [design/locomotion.md](design/locomotion.md) — character locomotion (issue #631).
- [design/soundgraph-metasounds.md](design/soundgraph-metasounds.md) — SoundGraph / MetaSounds-style audio graph refactor.

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

## ops/ — build & deployment

- [ops/build.md](ops/build.md) — full Windows / Linux / WSL build matrix.
- [ops/deployment.md](ops/deployment.md) — OloServer deployment / packaging.
- [ops/self-hosted-gpu-runner.md](ops/self-hosted-gpu-runner.md) — the self-hosted AMD GPU CI runner.
- [ops/self-hosted-host-hygiene.md](ops/self-hosted-host-hygiene.md) — the box behind the runners: the update timer must not reboot under a job, the GPU resets during the suite, one host is shared.
- [ops/self-hosted-linux-toolchain.md](ops/self-hosted-linux-toolchain.md) — hosted Linux pins clang-23 from apt.llvm.org; the self-hosted box uses its system clang, and a missing sanitizer runtime warns, never installs.

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
- [adr/0011-rhi-neutral-resource-and-binding-model.md](adr/0011-rhi-neutral-resource-and-binding-model.md) — API-neutral RHI resource/binding model: identity vs binding address vs native handle. The decisions plus an index of all 88 amendments (what each decided, whether it still binds); the amendment bodies are in [adr/0011-amendments.md](adr/0011-amendments.md).
- [adr/0012-adopt-the-ue-container-library-for-engine-owned-data.md](adr/0012-adopt-the-ue-container-library-for-engine-owned-data.md) — adopt the UE container library for engine-owned data; close the half-adopted state by growing usage.
- [adr/0013-destructible-debris-asset-swap-not-runtime-fracture.md](adr/0013-destructible-debris-asset-swap-not-runtime-fracture.md) — destructible objects swap in pre-authored debris assets; no runtime mesh fracture.
- [adr/0017-windows-ci-critical-path-measure-before-a-self-hosted-runner.md](adr/0017-windows-ci-critical-path-measure-before-a-self-hosted-runner.md) — the Windows CI critical path is measured on a writable cache before any self-hosted Windows runner is built, and never on the interactive workstation.
- [adr/0018-gaussian-splats-gpu-ordering-and-merge-lod.md](adr/0018-gaussian-splats-gpu-ordering-and-merge-lod.md) — Gaussian splats order per view on the GPU and coarsen by merging; a CPU sort and a selection budget are both dead ends.

## bug-investigations/ — postmortems & deep-dives

- [bug-investigations/fog-ubo-binding-knockout-investigation.md](bug-investigations/fog-ubo-binding-knockout-investigation.md) — flaky `FogVisualEvidenceTest` (#446): a persistent UBO bound only in its ctor gets its slot knocked to 0 by cross-test buffer churn; re-bind on upload.
- [bug-investigations/nanite-foliage-white-fringe-investigation.md](bug-investigations/nanite-foliage-white-fringe-investigation.md) — Nanite (#629): Sponza foliage white-fringe investigation.
- [bug-investigations/rendergraph-ghosting-investigation.md](bug-investigations/rendergraph-ghosting-investigation.md) — render-graph ghosting investigation.
