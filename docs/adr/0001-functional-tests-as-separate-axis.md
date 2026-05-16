# Functional tests live on a separate axis from the renderer testing pyramid

OloEngine's existing 11-layer testing pyramid (L1–L11, documented in
[`docs/testing.md`](../testing.md)) is renderer-specific —
every layer pins a property of the rendering pipeline (math, shader semantics,
GL state, render graph, goldens, etc.). Cross-subsystem world-tick tests
(Animation × Physics × Scripting × Networking × Audio × Asset × Nav × Save-game
× Gameplay × AI on a real `Scene::OnUpdateRuntime`) are a distinct failure
class with no overlap with the renderer pyramid. We track them on a separate
axis tagged `"Functional"` in `test_catalogue.json` — documented alongside
the renderer pyramid in the same [`docs/testing.md`](../testing.md), but on
its own axis rather than extending the pyramid with a 12th layer.

## Considered options

- **A 12th renderer-pyramid layer ("L12 Functional").** What the prototype
  shipped with. Concise, but mis-categorises: a reader looking at the
  pyramid would expect L12 to also be about pixels. Mixing renderer
  contracts with cross-subsystem behavioural contracts under the same
  taxonomy obscures both.
- **Orthogonal "fixture-type" axis** (Unit / Functional / EndToEnd × L1–L11).
  More expressive, but doubles the metadata every test must carry and
  complicates `generate_test_catalogue.py` for marginal value. Most tests
  are pinned to one axis or the other in practice.
- **Outside the registration contract** — Functional tests under
  `OloEngine/tests/Functional/` without any classification. Lighter to set
  up but breaks the "every test is classified" invariant we enforce via
  the `test-catalogue-in-sync` pre-commit hook, and silently rots when
  someone forgets to register a new test.
- **Reuse the existing `integration` id.** Conflates feature-level renderer
  integration (the original meaning, e.g. selection outlines exercising
  the full render path) with cross-subsystem world-tick integration —
  different failure classes under one label.

## Consequences

- `test_catalogue.json` adds `OloEngine/tests/Functional/` as a scan root.
  Every `.cpp` under it must carry the `"Functional"` tag (or be listed in
  `exclude` for helpers / fixtures).
- The generator script writes two separate auto-catalogue blocks into
  `docs/testing.md`: one for the renderer pyramid, one for the Functional
  axis. Both blocks are regenerated together and live behind distinct
  marker pairs (`renderer-catalogue` / `functional-catalogue`).
- The single `docs/testing.md` declares both axes side by side so readers
  see one is renderer-specific and the other is cross-subsystem.
- The original L12 framing in the previous version of this ADR is
  superseded by this revision. The on-disk taxonomy was renamed in lockstep
  (test file headers, fixture class, catalogue tag, doc structure).
