# Measuring one lighting term across paths: five traps that each make the comparison vacuous

**Rule.** When a test or live script separates a lighting term by rendering its source OFF and
ON and differencing, it must: read the colour *inside* the frame, confirm that every estimator
it toggles actually engaged, measure a temporal reconstruction converged, and read each state
from the target that state actually wrote. Assert the engagement; never infer it from a plausible
number.

Issue #1347 built the cross-path lighting matrix
(`OloEngine/tests/Rendering/CrossPath/`, live half `scripts/cross-path-matrix-live.py`).
Each trap below produced either a green comparison that measured nothing or a red one that blamed
the renderer for the fixture's mistake. Every one was found by a number that did not fit, never
by a failing assertion.

## 1. A transient render-graph target read after the frame holds a later pass's output

`EASUColor` read at the end of the frame on the spatial-upscale arm returned the tone-mapped image:
a directional term of 7.6 read as 0.76, and an authored emission of (4, 1, 0.25) read as
(0.903, 0.729, 0.481), which is Reinhard followed by gamma 2.2. The graph had reused EASU's memory
for a later pass. `SceneColor` happened to survive, which is why the first rows passed.

**Do:** read the target from a `RenderGraph::AddPostPassHook` right after the pass that writes it,
keyed from `GetResourceLifetimes()` (`CrossPathLightingMatrixTest.cpp` `Capture`, and the
state-machine harness's `PinTargets`). Live, pass `afterPass` to `olo_render_probe_pixel`.

## 2. GTAO is not on until it is the active technique

`GTAOEnabled = true` alone runs nothing: `GTAORenderPass` checks `ActiveAOTechnique == GTAO`, and
the default is SSAO. Every "direct light is invariant under GTAO" probe passed while GTAO never
ran. Set `ActiveAOTechnique` and `m_AOTechniqueOverride`; the override flag is also what makes
the scene serializer write the choice.

**Do:** pair every invariance probe with a positive control that the estimator changes *something*
in the same fixture (here, the `ScreenSpaceAO` row).

## 3. An estimator can stand down with a documented reason, and the frame still looks right

ReSTIR DI with one point light matched the raster loop bit for bit, because it had not run.
`olo_restir_stats` said `status: fallback`, "the scene has no more emitters than this tier samples
per pixel". With 64 lights it engaged, and it measured ×1.5–3.5 brighter than the raster loop on
specular tiles (#1483).

**Do:** require the tier's own statistics to report it active before comparing
(`cross-path-matrix-live.py` fails the cell otherwise).

## 4. A temporal upscaler carries history across the source switch

FSR2 does not reset when a light turns on under a static camera; its motion vectors describe
nothing. After 24 frames, 14 % of the snow term was still missing; after 96 frames, 7 %. The
fixture now drops the history the way production does on an extent change (resize away and
back), then re-accumulates.

Two properties remain after convergence, and they have their own stated tolerances, not a
widened general bound:
- FSR2's output moves 0.8–1.3 % of the full signal.
- A sub-pixel HDR peak averages 41–47 % low through FSR2's reversible tone map.

## 5. When the source *is* an estimator, the two states end the scene band at different targets

With SSGI off there is no `SSGIColor`; the OFF frame's colour ends at `SceneColor`. Probing the OFF
state at `SSGIColor` / `afterPass: SSGIPass` failed with "did not execute this frame".

**Do:** record, per state, which target was read and after which pass. The export's
`target`/`targetOff` fields do this.

## Related

- [lighting-signal-contract.md](lighting-signal-contract.md): which estimator owns which term.
- [live-verification-noise-floor.md](live-verification-noise-floor.md).
- [no-silent-fallbacks.md](no-silent-fallbacks.md).
- `docs/testing.md` §6.4: how to add a row.
