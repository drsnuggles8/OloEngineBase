# Redundant light-evaluation paths must agree photometrically

**Origin: issue #435 (clustered froxel lighting).** The engine evaluates the same
scene lights through several code paths, and every pair that can render the same
scene is a parity contract:

- the brute-force `MultiLightUBO` loop (`PBRCommon.glsl::calculateLightContribution`,
  used by plain Forward and by the deferred fallback),
- the clustered Forward+ evaluator (`ForwardPlusCommon.glsl::fplusEvaluateTileLights`,
  consumed by Forward+, Terrain, **and Deferred** since #435),
- the froxel fog scatter's in-scatter loop (`FroxelFogScatter.comp`, plain GLSL
  compute — cannot include `PBRCommon.glsl`, mirrors the formulas inline).

## The two bugs this rule exists because of

Both were **pre-existing** in the fp evaluator and invisible until #435 routed the
Deferred path through it — at which point every deferred scene silently re-lit:

1. **Attenuation model drift** — the fp path used a UE4-style windowed
   inverse-square `(1−(d/r)²)²/(d²+1)` that **ignored the component's
   `m_Attenuation`**, while the UBO loop used `calculateAttenuation`
   (`1/(1+q·d²) · saturate(1−(d/r)⁴)²`). At the default `q = 2` that is ~1.9×
   brighter at typical distances, and the user-facing `m_Attenuation` knob was
   dead on every clustered path.
2. **Missing `NdotL`** — `calculateLightContribution` multiplies the cosine term
   after `cookTorranceBRDF` (which does **not** include it); the fp path composed
   `brdf * radiance` directly, leaking full diffuse onto grazing and back-facing
   surfaces ("flat bright cubes").

Fix shape: carry **every** light parameter into the GPU structs
(`GPUPointLight::ShadowAndAttenuation.y`, `GPUSpotLight::SpotParams.y`), call the
**same PBRCommon helpers** from the fp evaluator (`calculateAttenuation`,
`calculateSpotIntensity` — the includers already provide PBRCommon), and apply
`NdotL` exactly as the UBO loop does. The fog scatter mirrors the attenuation
inline (media have no surface normal, so no `NdotL` there) with a comment naming
the helper it mirrors.

## Rules

1. **One formula, one home.** Where the shader language allows, a shared helper
   (PBRCommon) is the only place a falloff/cone/BRDF composition may live. A
   compute shader that cannot include it must mirror the formula with a comment
   naming the canonical helper — and that mirror is a known drift risk to check
   when the helper changes.
2. **A GPU light struct that drops a component parameter kills that knob
   silently** on every path fed by the struct. When adding/auditing a packed
   light struct, diff its fields against the component's authored fields and
   account for every one (used, or deliberately excluded with a comment).
3. **Pin parity with a differential photometric test, not a structural one.**
   `ClusteredLightingVisualEvidenceTest.ClusteredAgreesStructurallyWithBruteForce`
   renders the same many-light scene on both paths and asserts per-band channel
   means within ±5 grey levels. "The two paths use different curves by design"
   was the rationalization that hid both bugs — if the curves really must
   differ, that difference should be a named, documented decision, not a test
   tolerance.
4. **Calibrate the tolerance against the measured noise floor.** Full-suite
   cross-test residue (TAA/exposure history) shifts two same-scene captures by a
   uniform ~2–3 grey levels; the real bugs measured 8–16 (attenuation) and 4–8
   (NdotL). Tolerance 5 separates the distributions. A differential contract
   that passes in isolation can still fail in-suite — calibrate in-suite.
5. **Pixel-diff any pre-existing evidence PNG your diff regenerates.** The
   byte-churn IS the signal: `deferred(old)` matched `forward` patch-for-patch
   to the decimal, `deferred(new)` was +55% — that one comparison found what
   4300 green tests missed. If a band-mean contract passes while the whole-image
   diff is large, the divergence lives in geometry the bands don't sample
   (edges, grazing angles) — chase it before accepting.
