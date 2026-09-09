# A tuning constant a shader consumes needs a unit that does not depend on the frame (issue #1119)

**State the unit of every authored render constant in the type that holds it, and pick a unit whose
meaning does not change with camera range, cascade index or resolution.** If the natural expression
is per-cascade or per-pixel, convert at the point of use from something the shader already has —
don't ask the author for a number in a space they cannot see.

`ShadowSettings::Bias` was a constant depth offset in the shadow map's **normalized [0,1] depth**,
subtracted from the receiver's comparison depth. Nothing about that number tells you what it means
in the world, because a CSM cascade's orthographic depth range is not fixed: `ComputeCSMCascades`
pads it with `zPadding = 200 m` on **both** sides so casters outside the camera frustum still
register, then adds the cascade's own extent. In the sample scenes that range runs 404–1300 m. So
the engine default of `0.005` meant

| cascade | depth range | 0.005 in world metres | ground gap at a 52° sun |
|---|---|---|---|
| 0 | 404 m | 2.02 m | 1.6 m |
| 1 | 412 m | 4.12 m | 3.2 m |
| 2 | 448 m | 6.71 m | 5.3 m |
| 3 | 665 m | 13.29 m | 10.4 m |

(the shader multiplied by `cascadeIndex + 1` on top, which is where the second column's growth comes
from). Every directional shadow in the project was displaced from its caster by metres — a box on
the ground had a shadow floating a caster-height clear of its base, and small casters lost their
shadow entirely.

## What made it hard to see

**The bug was filed against the wrong knob, with real evidence.** `DirectionalLightComponent`'s
*other* bias, `m_ShadowNormalBias`, was authored at `0.1` in 649 light blocks against an engine
default of `0.01`, and the issue reasonably read the 10× discrepancy as the cause. It is not:
normal bias offsets the receiver along its shading normal in **world metres** before the light-space
projection, so `0.1` moves the shadow `0.1 · tan θ ≈ 0.13 m` at that sun angle. A live A/B is
unambiguous — setting it to `0.01` and leaving the depth bias alone produces a frame with no visible
change, while zeroing the depth bias and leaving normal bias at the "wrong" `0.1` snaps the shadow
back onto the caster's base.

Two knobs sitting next to each other, one with an obviously wrong number and one with an obviously
right one, and the innocent-looking one was the bug. **A/B each knob separately before believing a
census.** A count of how far a value drifted from its default measures drift, not blame.

**And it hid behind a second value.** Because the depth bias was enormous, no scene ever showed
shadow acne, so nothing pushed back on it. It reads as "shadows are soft/offset here", which is
easy to accept as a look.

## The fix, and the shape to copy

`ShadowParams.x` now carries the bias in **shadow-map texels of the sampling cascade**, and
`calculateCascadedShadowFactorCSM` converts it using only the light-space matrix it already has:

```glsl
float lenRow0 = max(length(vec3(M[0][0], M[1][0], M[2][0])), 1e-8); // 1 / half-extent
float lenRow2 = length(vec3(M[0][2], M[1][2], M[2][2]));            // 2 / (far - near)
float cascadeBias = shadowParams.x * lenRow2 / (float(shadowMapResolution) * lenRow0);
```

The projection is an orthographic matrix composed with a `lookAt`, whose rotation is orthonormal, so
those two row lengths *are* the metres-per-texel and depth-per-metre factors. Texel snapping and the
camera-relative shift only touch the translation column, so neither disturbs them. Nothing extra is
uploaded, and the conversion cannot drift from the cascade it describes. The old `* (cascadeIndex + 1)`
heuristic is gone: a farther cascade needs more bias because its texels are bigger, which the unit now
expresses exactly rather than approximately.

Three consequences worth copying:

- **A renamed field beats a reinterpreted one.** `m_ShadowBias` became `m_ShadowDepthBiasTexels`, so a
  scene still carrying the old YAML key gets the engine default and a warning, instead of having its
  number silently re-read in a new unit. The save-game format bumped to v28 and reads the old float
  only to keep the stream in step. The rule generalises: *when a value's unit changes, change its
  name.*
- **One number cannot serve two spaces.** The same `ShadowParams.x` was also the local-light **atlas**
  bias, whose entries are perspective, so an unrelated directional light's authoring decided how spot
  and point shadows biased. The atlas now reads its own `u_AtlasDepthBias` lane — taken from the former
  `_shadowPad1` int, so the std140 size is unchanged.
- **Pin the unit, not the value.** `ShadowMapTest`'s `CSMDepthBias*` tests assert that converting the
  bias back to metres gives exactly N texels *of that cascade*, that the result stays centimetre-scale
  in cascade 0, and that `MaxShadowDistance` does not change what one authored texel means. A test
  that had only pinned `0.005` would have passed throughout.

## Related

- [procedural-generator-golden-coupling.md](procedural-generator-golden-coupling.md) — the sweep moved
  benchmark goldens, rebaked in the same PR.
- [no-silent-fallbacks.md](no-silent-fallbacks.md) — why the retired YAML key warns rather than being
  quietly ignored.
- [live-verification-noise-floor.md](live-verification-noise-floor.md) — the A/B above is only
  readable once you trust the frame you captured.
