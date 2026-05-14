# OloEngine Context

Shared language for the OloEngine codebase. The first cluster captured here
is **engine testing**, prompted by the work to add UE5.7-style Functional
Tests. Other clusters will be added as they crystallise.

This file is the glossary. For *rules* see `.github/instructions/`; for
the *why* behind both the renderer pyramid and the Functional axis see
`docs/testing.md`.

## Language

### Testing

**Renderer Testing Pyramid**:
The 11-layer taxonomy (L1 property … L11 sanitizers + fuzzing) that
classifies tests of the **rendering pipeline**. Documented in
`docs/testing.md`. Every test under
`OloEngine/tests/Rendering/`, `OloEngine/tests/ShaderGraph/`, or
`OloEngine/tests/Streaming/` is assigned exactly one Layer.
_Avoid_: tier, category, kind. "Pyramid" alone is ambiguous — qualify with
"renderer" when it matters.

**Layer**:
One classification within the **Renderer Testing Pyramid** (L1–L11, plus
auxiliary ids `plumbing`, `cullinglod`, `shaderpipe`, `integration`,
`meta`). Renderer-only — *not* used for Functional tests.

**Functional Test**:
A multi-frame, cross-subsystem test that drives a real **Scene** through
**Ticks** and asserts on integration outcomes that no per-subsystem test
would catch (e.g., Animation → Physics → Scripting → Networking → Audio
→ Asset → Nav → Save-game → Gameplay → AI interactions). Tagged
`"Functional"` in `test_catalogue.json`. Lives on its own axis alongside
the Renderer Testing Pyramid, not inside it (see ADR 0001).
_Avoid_: integration test (the catalogue's `integration` id means
something narrower — feature-level renderer integration).

**Tick**:
One call to `Scene::OnUpdateRuntime(ts)` with a fixed `Timestep`. The
unit of simulation in a Functional Test.
_Avoid_: frame, step, update.

**Headless Tick**:
A **Tick** that does not call any `RenderScene*` method. The default
mode for Functional tests — runs without a GL context, parallelisable,
WSL-capable.

**Renderer-attached Test**:
A Functional test using a fixture base (deferred — see
`docs/testing.md` §6) that creates a GL context and calls
`RenderScene` after each **Tick**, for the rare case where a
cross-subsystem bug only manifests with rendering active.

**Latent Assertion**:
An assertion that depends on multiple **Ticks** of simulation, expressed
synchronously via `TickUntil(condition, timeout)` or
`RunFrames(N)` — never via a queued-command runner.
_Avoid_: deferred check, async assertion.

**Best-effort Determinism**:
The Functional test contract: each test runs with a fixed `Timestep` and
a seeded `FastRandom`. Wall-clock leakage in subsystems is fixed
reactively when a test exposes it, not preemptively across the whole
engine.

**Editor-mounted Asset Root**:
The Functional test contract for asset access: the harness mounts
`EditorAssetManager` against an isolated **temp copy** of
`OloEditor/SandboxProject/` (built per test under the OS temp dir), so
tests reference real editor handles (skeletons, clips, prefabs, scenes,
textures) the way the runtime does, without dirtying the working tree.
Editor content changes can break Functional tests at a distance —
that's accepted as a desirable signal (asset-pipeline regression caught
early) rather than test-suite flakiness. See ADR 0003.

**Scan Root**:
A directory listed in `test_catalogue.json` whose `.cpp` files are
required to be classified by **Layer** or tagged `"Functional"`. Adding
a new test directory means adding a Scan Root.

**Registration Contract**:
The invariant — enforced by the `test-catalogue-in-sync` pre-commit
hook — that every `.cpp` under a **Scan Root** appears in
`file_layer_map` (with a **Layer** or `"Functional"`) or `exclude`
(helpers only).

## Relationships

- A **Functional Test** is registered under the `"Functional"` tag in
  `file_layer_map`. Renderer tests are registered under a **Layer**.
- A **Functional Test** owns one or more **Scenes** and drives them via
  **Ticks**.
- A **Headless Tick** is the default for Functional tests; a
  **Renderer-attached Test** opts in via fixture base.
- A **Latent Assertion** is composed of N **Ticks** plus a predicate
  over component state.
- The **Registration Contract** governs every **Layer** and the
  `"Functional"` tag.

## Example dialogue

> **Dev:** "I want to verify that when an animated character walks into a
> trigger volume, the AI fires a dialogue event and the network
> replicates the dialogue state to the client. Is that an L1?"
>
> **Engine programmer:** "No — that's a **Functional Test**. Three
> subsystems on the seam. Build a **Scene** with the character and the
> trigger, **TickUntil** the character enters the volume (timeout 2s),
> then assert the dialogue state on the server entity and the
> replicated copy on the client entity. **Headless Tick** is fine —
> none of those subsystems need pixels. Register it under
> `\"Functional\"`."

## Flagged ambiguities

- "Integration test" was used to mean both (a) feature-level renderer
  integration (the existing `integration` id, e.g.,
  `RenderingRegressionTest.cpp`) and (b) cross-subsystem world-tick
  integration. **Resolved:** these are distinct. (a) keeps the
  `integration` id under the renderer pyramid; (b) becomes a
  **Functional Test** on a separate axis.
- "Tick" vs. "frame": **Tick** is the canonical word for one
  simulation step. "Frame" is reserved for renderer concerns (frame
  data, frame capture, framebuffer).
- "L12" was the prototype name for Functional tests when they were
  modelled as a 12th pyramid layer. **Superseded:** the layer was
  dropped in favour of a separate axis (ADR 0001). Old references in
  history are equivalent to today's `"Functional"` tag.
