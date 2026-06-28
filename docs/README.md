# OloEngine documentation

Index of everything under `docs/`. Most subsystem code comments link here by path,
so keep references in sync when moving a file (`git grep "docs/<name>"` before a rename).

## Testing
- [testing.md](testing.md) — the canonical testing opinion doc: *why* we test what we test, the renderer L1–L11 pyramid + Functional axis, value heuristic, anti-patterns, classification. The hub the test suite, CI, and `agent-rules/testing-architecture.md` all point at.
- `test-catalogue.{renderer,functional,unit}.md` — **generated & git-ignored** per-file catalogues, rendered from `test_catalogue.json` + in-file `// OLO_TEST_LAYER` tags by `OloEngine/tests/scripts/generate_test_catalogue.py`. Not tracked; regenerate on demand.

## guides/ — subsystem & tooling how-tos
- [guides/ai-goap.md](guides/ai-goap.md) — GOAP planner / AI action system.
- [guides/ai-perception.md](guides/ai-perception.md) — AI perception (sight/sound/awareness).
- [guides/cinematic-sequencer.md](guides/cinematic-sequencer.md) — cinematic sequencer / timeline.
- [guides/localization.md](guides/localization.md) — localization & `LocalizedTextComponent`.
- [guides/mcp-diagnostics-server.md](guides/mcp-diagnostics-server.md) — the read-only MCP diagnostics server (tools, resources, prompts, attach flow).
- [guides/procedural-terrain.md](guides/procedural-terrain.md) — procedural terrain generation.
- [guides/ui-system.md](guides/ui-system.md) — runtime UI system.
- [guides/video-playback.md](guides/video-playback.md) — video playback component.

## design/ — design rationale & roadmap docs (cited from source for *why*)
- [design/WATER_FUTURE_IMPROVEMENTS.md](design/WATER_FUTURE_IMPROVEMENTS.md) — water/FFT-ocean rendering design & roadmap. **Cited by ~40 code comments via bare `WATER_FUTURE_IMPROVEMENTS.md §X.Y`** — keep this basename stable.
- [design/GPU_INSTANCING_FUTURE_IMPROVEMENTS.md](design/GPU_INSTANCING_FUTURE_IMPROVEMENTS.md) — GPU instancing / indirect-draw design & roadmap (basename cited from code; keep stable).
- [design/animation-retargeting.md](design/animation-retargeting.md) — animation retargeting (humanoid bone roles, rebasing).
- [design/soundgraph-metasounds.md](design/soundgraph-metasounds.md) — SoundGraph / MetaSounds-style audio graph refactor.

> Roadmap docs describe intended/future work — verify "doneness" against the **code**, not these files.

## analysis/ — quality & code-health reports
- [analysis/dead-code.md](analysis/dead-code.md) — dead-code analysis.
- [analysis/sonarqube-rules.md](analysis/sonarqube-rules.md) — SonarQube rule suggestions & high-volume-rule decisions.

## ops/ — build & deployment
- [ops/build.md](ops/build.md) — full Windows / Linux / WSL build matrix.
- [ops/deployment.md](ops/deployment.md) — deployment / packaging.

## adr/ — architecture decision records
- [adr/0001-functional-tests-as-separate-axis.md](adr/0001-functional-tests-as-separate-axis.md)
- [adr/0002-headless-tick-default-for-functional-tests.md](adr/0002-headless-tick-default-for-functional-tests.md)
- [adr/0003-functional-tests-mount-editor-asset-root.md](adr/0003-functional-tests-mount-editor-asset-root.md)
- [adr/0004-lock-free-allocator-singleton-init.md](adr/0004-lock-free-allocator-singleton-init.md)

## agent-rules/ — guidance for AI agents working in this repo (read before non-trivial work)
- [agent-rules/cpp-coding-quality.md](agent-rules/cpp-coding-quality.md) — C++ idioms, float comparison, `auto`, IWYU, MSVC quirks.
- [agent-rules/glsl-shaders.md](agent-rules/glsl-shaders.md) — SPIR-V constraints, UBO bindings, MRT.
- [agent-rules/sonarqube-review-alignment.md](agent-rules/sonarqube-review-alignment.md) — match local `/code-review` to the cloud C++ Extended profile.
- [agent-rules/testing-architecture.md](agent-rules/testing-architecture.md) — the renderer pyramid + Functional axis decision tree & registration contract.

## bug-investigations/ — postmortems & deep-dives
- [bug-investigations/rendergraph-ghosting-investigation.md](bug-investigations/rendergraph-ghosting-investigation.md) — render-graph ghosting investigation.
