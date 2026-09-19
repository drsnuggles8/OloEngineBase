# Skin: lips, oral surfaces and teeth

Transport version 4 (`SkinEvaluationModel::OralSurface`), issue #1245. It adds two things to a skin
profile:

- a **wet coat** — a thin dielectric film in *front* of the tissue, which **takes** energy from the
  response underneath rather than adding to it;
- a **cavity weight** — which gates the version-2 thin-region transmission by the material's own
  occlusion, so the inside of a closed mouth stops glowing.

Maths and every physical decision: [`Renderer/SkinOralSurface.h`](../../OloEngine/src/OloEngine/Renderer/SkinOralSurface.h).
Shader side: [`include/SkinOralSurface.glsl`](../../OloEditor/assets/shaders/include/SkinOralSurface.glsl).

## Authoring

Four fields, in the `.oloskin`'s `Oral` block. All four are read **only** at transport version 4; a
profile stays where its author left it.

| field | default | what it does |
|---|---|---|
| `CoatStrength` | `0.0` | how much wet film is present. **0 skips the coat entirely** and is bit-identical to the version-3 frame. |
| `CoatRoughness` | `0.1` | the film's own perceptual roughness. Only consulted when `CoatStrength > 0`. |
| `CoatIor` | `1.33` | the film's index of refraction, seen from air. Saliva is 1.33; **enamel is 1.63**. |
| `CavityOcclusion` | `0.0` | how much of the material's AO is spent on the **transmitted** term. 0 leaves it exactly as #1242 shipped it. |

`CoatStrength` and `CavityOcclusion` both default to neutral, so **moving a profile to version 4
changes nothing** until a field is authored. That is the identity arm
`SkinOralSurfaceEvidenceTest` captures on all three raster paths, and the first thing to check if a
head changes appearance on a version bump.

> `CoatIor` is authored, **F0 is not.** The conversion
> `F0 = ((n-1)/(n+1))^2` happens once, on the CPU, in `SkinOralCoatF0`, and the profile lane carries
> the result. The IOR is the number that exists in a reference table; `0.0201` and `0.0574` are
> numbers nobody can check by eye.

## Teeth are authored, not branched

There is no `Enamel` code path. A tooth is a version-4 profile that differs from the mucosa beside
it in **three** places, and all three are authored:

| | mucosa | enamel |
|---|---|---|
| `Oral.CoatIor` | `1.33` (saliva) | `1.63` — roughly 3× the reflectance at normal incidence |
| `ScatterColor` | red-dominant, e.g. `[0.86, 0.50, 0.42]` | near-achromatic, e.g. `[0.92, 0.90, 0.88]` |
| `ScatterRadiusMM` | `[2.1, 1.05, 0.7]` — a lip is translucent | `[0.42, 0.40, 0.38]` — short and neutral |

A single differing field would be a tuning difference wearing the acceptance criterion's clothes.
Changing only the IOR leaves a tooth scattering red like a dermis, which reads as a gum with a
highlight.

The shipped set is `Assets/Materials/Oral{Lip,Tongue,Gum,Enamel}.oloskin`, assigned per entity in
`Assets/Scenes/OralSurfaces.olo`.

## Why the coat takes energy

The film is a layer in front of the tissue; light it reflects never reaches the tissue. So:

```
attenuation   = 1 - strength * F_coat
out.Diffuse   = base.Diffuse  * attenuation
out.Specular  = base.Specular * attenuation + coatSpecular * strength
```

`strength * F_coat` and `attenuation` sum to **exactly one** — a partition, not a blend. That is
what makes *"wet specular stays distinct from diffusion"* a structural property rather than a tuning
result: the coat's lobe goes into `.Specular`, which is the half `oloSkinDiffusionOutput` does
**not** hand to the screen-space blur, and the diffuse half is only ever attenuated. No amount of
coat can put energy into the diffusion.

"On average" is load-bearing and is not hidden: a GGX lobe concentrates the energy it is given, so
the coat *can* be brighter than what it replaced in the highlight's few pixels. What is bounded is
the hemispherical integral, and `SkinOralSurfaceTest` integrates the lobe numerically rather than
asserting it in prose.

## Why the cavity weight gates the transmission and nothing else

Because the transmitted lobe is the only term in the skin transport that is not already occluded:

- the direct lobes carry `lightVisibility` — the shadow map, the cloud shadow, the RT mask;
- the ambient ladder is scaled by the material's AO, at the same site on all three paths;
- the **transmitted** lobe carries `lightVisibility` and nothing else.

A closed mouth casts no shadow onto its own interior at any shadow-map resolution this engine runs
at, so under a back light the lips stay lit from inside. Applying the weight to the ambient as well
would apply the AO twice, which is as wrong as applying it never.

The signal is the material's **existing AO map**, not a new mask: a second texture would need a
slot, a heap offset, an importer path and an authoring convention, and would introduce a way for the
occlusion to disagree with the AO the ambient ladder already uses on the same texels.

## Limits, stated

- **IBL and light probes do not get a coat.** An environment coat means a second prefiltered
  cubemap fetch per pixel, and [`skin-layered-specular.md`](skin-layered-specular.md) already
  declined to spend one for its second lobe on measured grounds. A mouth lit *only* by an
  environment map will not read as wet; the fix is a key light, which is what every close-up in the
  evidence matrix uses.
- **Sphere area lights get no coat either.** `oloLightSample` declines to give them a direction, and
  the representative-point approximation splits the BRDF differently — evaluating a second
  interface along an invented `L` would be an expression with no physical reading.
- **There is no oral anatomy in this repo.** `Assets/Scenes/OralSurfaces.olo` builds its cavity from
  primitives, because Suzanne's mouth is a closed slit with no interior and the issue's scope
  boundary excludes generating anatomy. Every claim the fixture supports is a claim about the
  *material*, which does not know what mesh it is on — dropping a scanned head in changes the
  subject and nothing else.

## Appending a version 5

The versions are **cumulative** and every list that tests them is spelled out explicitly, so
appending one means editing each. `git grep LayeredSpecular -- '*.cpp' '*.h' '*.glsl'` — the `.glsl`
lists are the ones a shader grep finds and they are not all of them. The CPU side that has been
missed twice is `Renderer/SkinDiffusion.cpp`'s kernel builder: miss it and a head silently loses its
subsurface scattering. `SkinOralSurfaceEvidenceTest`'s neutral-identity A/B is the assertion that
catches it.
