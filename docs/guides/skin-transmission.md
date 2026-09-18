# Skin: thickness-aware transmission

How to author a backlit ear, what the numbers mean, and the five ways the term silently does
nothing. Issue #1242, on top of #1231 (diffuse/specular split) and #1241 (screen-space diffusion).

The rules for agents working on this code are in
[agent-rules/notes-renderer.md](../agent-rules/notes-renderer.md); the maths and the energy argument
are in [`Renderer/SkinTransmission.h`](../../OloEngine/src/OloEngine/Renderer/SkinTransmission.h),
which is the file to read before changing any of it.

## Authoring, in four steps

1. **Set the material's kind to `Skin`** and give it a `.oloskin` profile (Scene Hierarchy →
   Material → *Material Kind* / *Skin Profile*).
2. **Move the profile to transport version 2** — `EvaluationModel: 2` in the `.oloskin`, shown in
   the inspector as `Transport version: ThicknessTransmission`. Version 2 is *everything version 1
   does, plus transmission*: it still diffuses.
3. **Author a thickness on the material**, in **metres**: *Thickness Factor (m)*. A human ear is
   about `0.002`; a cheek is closer to `0.03`. The inspector shows the millimetre equivalent.
4. **Optionally add a thickness map** — *Thickness Map*, red channel, unitless `[0,1]`, a per-pixel
   **modulation** of the factor. This is what makes an ear thin where the jaw behind it is not. It
   is glTF's `KHR_materials_volume` thickness texture, so an authored asset imports it directly.

## Units, once

| quantity | where | unit |
|---|---|---|
| `Thickness Factor` | material | **metres** (glTF `KHR_materials_volume`) |
| thickness map | material, red channel | unitless `[0,1]`, a multiplier |
| `ThicknessScale` | profile | unitless; `1000` is the m → mm identity |
| `ScatterRadiusMM` | profile | **millimetres**, per channel, mean free path |
| `ScatterColor` | profile | linear Rec.709, unitless `[0,1]`, transport albedo |

The transport works in millimetres. `thicknessFactor × map × ThicknessScale` is the thickness the
optical depth divides, and the engine computes that product on the CPU
(`SkinThicknessBaseMM`) — the shader never converts a unit.

**`ThicknessScale` is the transmission knob.** It does *not* affect #1241's blur radius; scale away
from 1000 to exaggerate or damp transmission without touching the scattering reach.

## What the three lobe parameters do

- **`Strength`** `[0,1]` — scales the whole term. `0` disables it even at version 2. It is capped at
  1 because above 1 the energy bound below stops holding; this is not a taste bound.
- **`Anisotropy`** `[0,1]` — how view-dependent the exit is. `0` is an isotropic exit (the volume has
  forgotten which way the light came in); `1` only glows when you look toward the light through the
  surface. `0.7` is a good ear.
- **`Power`** `[1,64]` — how sharp the view-dependent part is.

## Energy: why this does not double-count #1241's diffusion

Three facts, each pinned separately by `SkinTransmissionTest`:

1. **The two transports draw from disjoint incident directions.** The reflected diffuse lobe carries
   `saturate(dot(N, L))`; this term carries `saturate(-dot(N, L))` and is *exactly* zero over the
   whole near hemisphere. A photon is counted by one or the other, never both.
2. **Diffusion redistributes, it does not add.** #1241's kernel weights sum to 1 and the pass adds
   `blur(aux) - aux`, so it moves diffuse energy around the image without changing the total.
3. **The lobe never exceeds 1**, so the term is at most `incident × transmittance`, and
   transmittance is at most `ScatterColor`.

**There is no `Wrap` parameter, deliberately** — and this is the one place skin differs from the
foliage lobe it otherwise resembles. Foliage wraps its transmission into `dot(N, L) > 0` on purpose,
because a leaf is a thin sheet whose two faces are the same surface. A cheek has a near face and an
ear has a far one, so wrapping here would lay transmitted energy on top of the reflected diffuse
lobe over a whole hemisphere. Fact 1 is an equality, not an approximation, and adding a wrap would
break it.

**One transport albedo, used twice, is not a double-count.** `ScatterColor` and `ScatterRadiusMM`
feed both features: in the diffusion kernel they shape the *spatial distribution* of a fixed amount
of energy (normalised weights), and here they scale the *magnitude* of a different transport. There
is deliberately no separate "transmission tint" — a second colour would let an author set a
transmission the surface's own absorption says is impossible.

## The five ways it silently does nothing

The **first four** are `SkinTransmissionFallbackReason` values: counted in
`SkinProfileTable::GetTransmissionFallbackCount`, logged once per (reason, profile) pair, and called
out in the material inspector. They are counted **per submission**, not per pixel — the shader
cannot log, so every one of them is raised on the CPU by the site that can see the cause. Grep
`OloEngine.log` for `no thin-region transmission`.

The fifth row is **not** a fallback and is not counted: a profile below version 2 is an author who
has not opted in, which is the design working rather than a degradation.

| reason | what happened | fix |
|---|---|---|
| `NoThickness` | version 2, but `Thickness Factor` is 0 — so there is nothing for a map to modulate | set the factor, in metres |
| `ThicknessMapMissing` | a map was authored but could not be loaded; the **scalar** thickness is used alone, so the ear reads uniformly thick | fix the texture path |
| `RefractiveTransmissionConflict` | `KHR_materials_transmission` is *also* raised — two transmission closures over one surface. Skin's wins | set `Transmission Factor` to 0 |
| `DeferredThicknessLaneUnavailable` | the surface is lightmapped, so G-Buffer RT5 is holding baked irradiance and cannot carry a per-pixel thickness. On **Deferred** the term does not fire at all — the scalar never reaches that pass, since the channel is its only route. Forward and Forward+ are unaffected | unlightmap the head (it is animated geometry anyway), or use a forward path |
| profile below version 2 | the author never opted in. Not a fallback — it is the design | set `EvaluationModel: 2` |

**A missing thickness transmits *nothing*, not everything.** A zero thickness has two readings, and
only one is safe: "no volume authored" and "infinitely thin, so `exp(0) = 1`, so fully
transparent". The second renders the uniformly emissive head. The engine takes the first, so an
unauthored head looks *unfinished* rather than *wrong*.

## Where it is evaluated, per path

| path | thickness source | notes |
|---|---|---|
| Forward, Forward+ | sampled directly in `PBR_MultiLight{,_Skinned}.glsl` | full per-pixel map |
| Deferred | G-Buffer **RT5 red channel**, coverage 0 | second tenant of the channel foliage uses (#1234); see `oloSkinPackGBufferThickness`. A **lightmapped** skin surface keeps its irradiance and gets no transmission here |
| Virtual Geometry | **not reached in this slice** | a VG skin surface writes no thickness, so it reads 0 and transmits nothing — the conservative fallback, but by omission rather than by design |
| Vulkan RT | raster fallback, as the issue's contract states | — |

## Two limits, stated

**A region thicker than the shadow-map normal offset can still self-shadow.** A backlit surface's
shading normal points away from the light, so `oloSkinShadowNormal` flips it for the shadow lookup —
the same trick `oloFoliageShadowNormal` uses, and the reason backlit ears glow at all. But the
offset is a shadow-bias constant, not the authored thickness, so genuinely thick backlit geometry
can still read as self-shadowed and lose the term. That fails in the **conservative** direction (a
thick region should not transmit much anyway) but it fails for the wrong reason, so a very thick
region's falloff is not physically meaningful.

**Punctual lights on the ReSTIR DI and Forward+ tiers do not transmit.** Those tiers own the
punctual lights and the directional loop is what evaluates this term, exactly as for foliage
(#1234). The demonstrating case is unaffected — transmission is a sun effect and the sun is a
directional light.

## Inspecting a frame

`MaterialDebugView::Transmission` (`olo_render_capture_target`, or the renderer settings' material
debug combo) shows the transmitted term **alone**, black everywhere else. It is the first thing to
look at when a backlit ear does not glow:

- **black** → the data did not arrive. Check the kind, the profile slot, the version and the
  thickness, in that order.
- **non-black, but the composite looks unchanged** → the term is being computed and lost at the
  composite.
