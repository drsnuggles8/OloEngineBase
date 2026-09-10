# glTF physical material extensions — transmission, IOR and volume

What OloEngine imports and renders from `KHR_materials_transmission`,
`KHR_materials_ior` and `KHR_materials_volume` (issue #970), and exactly where it stops.

Read the support table first. Everything below it is the reasoning.

## Support table

| glTF property | Extension | Imported | Rendered | Notes |
|---|---|---|---|---|
| `transmissionFactor` | `KHR_materials_transmission` | yes | yes | The master gate. `0` (the default) bypasses the whole closure. |
| `transmissionTexture` | `KHR_materials_transmission` | **no** | no | Warned and counted; the material imports at its `transmissionFactor`. |
| `ior` | `KHR_materials_ior` | only when transmissive | yes | Drives the Fresnel weight and the refraction direction. Imported and consumed **only** while `transmissionFactor > 0` — see *IOR* below. |
| `thicknessFactor` | `KHR_materials_volume` | yes | yes | `0` is thin-walled: no absorption. `> 0` enables Beer-Lambert. |
| `thicknessTexture` | `KHR_materials_volume` | **no** | no | Warned and counted; the material imports at its `thicknessFactor`. |
| `attenuationColor` | `KHR_materials_volume` | yes | yes | Per-channel; this is what makes thick glass colour-shift, not just darken. |
| `attenuationDistance` | `KHR_materials_volume` | yes | yes | `+Infinity` (the default, and an omitted key) means no absorption. |

Rendering-path support:

| Path | Backend | Transmission |
|---|---|---|
| Forward / Forward+ | OpenGL, Vulkan | yes — `PBR_MultiLight{,_Skinned}.glsl` |
| Deferred | OpenGL, Vulkan | yes, via reroute — see *Deferred* below |
| G-Buffer proper | either | no, by construction — the G-Buffer has no transmission channels |
| Virtual geometry / visibility resolve | either | no — those writers are G-Buffer writers |
| Reference path tracer | — | no — it has its own closure set |

## The one limitation that matters

**Transmitted light comes from the prefiltered environment, not from the geometry behind
the surface.** Glass shows the environment map refracted through it; it does not show the
object standing behind it.

This is a deliberate stopping point, not an oversight. Sampling what is actually behind a
surface needs an opaque copy of the scene colour bound during the scene pass, and the
engine has no such copy there — `WaterRefraction` is produced inside `WaterRenderPass` for
the water and fluid passes alone. Adding a general one means a new render pass, which is
a larger change than this slice, in code another branch was rewriting at the time.

The practical consequences:

- A clear pane against a bright sky reads correctly.
- A clear pane in front of a distinctive object shows the *environment*, not that object.
- Absorption, colour shift with thickness, IOR-driven Fresnel and roughness-driven
  frosting all behave correctly regardless, because none of them depend on what is behind.

## Neutral defaults, and why legacy materials cannot move

Every field defaults to a value that makes the feature invisible:

| Field | Default | Effect |
|---|---|---|
| `TransmissionFactor` | `0` | The shader branch is not taken at all. |
| `IOR` | `1.5` | `((1.5-1)/(1.5+1))² = 0.04` — exactly the dielectric F0 the PBR shaders already hardcode. |
| `ThicknessFactor` | `0` | Thin-walled; `exp(-σ·0) = 1`. |
| `AttenuationColor` | white | `-log(1) = 0`, so σ is zero whatever the distance. |
| `AttenuationDistance` | `+∞` | A finite numerator over `+∞` is exactly `0`, so σ is zero whatever the colour. |

`MaterialTransmissionTest` asserts each of these, including that the default σ is *exactly*
`0.0f` rather than merely small.

### IOR

**IOR is read only for a material that already transmits.** Assimp maps
`KHR_materials_ior` onto the *generic* `$mat.refracti` key, which OBJ/MTL also writes on
ordinary opaque materials — frequently as `1.0`, and Assimp's OBJ importer supplies a
default even when the file does not. Importing it unconditionally would give every OBJ
material a non-default IOR, so they would re-serialize with a `PhysicalMaterial` block they
never had, and `1.0` means `F0 = 0`.

The cost is that a glTF carrying `KHR_materials_ior` with **no** transmission does not
round-trip its `ior`. The shading path consumes IOR only inside the transmission closure,
so on such a material the value has no observable effect either way.

The spec's `ior = 0` ("no refraction") is preserved by `SetIOR` and handled in the shader:
`oloIorToF0` returns `0` below 1.0 rather than evaluating `((0-1)/(0+1))² = 1`, which would
read as a perfect mirror and render fully clear glass solid.

## Where the absorption is computed

`KHR_materials_volume` defines transmittance through a slab of thickness *d* as

```
transmittance = exp(log(attenuationColor) · d / attenuationDistance)
```

The engine rewrites this as `exp(-σ·d)` with `σ = -log(attenuationColor) / attenuationDistance`
and **derives σ on the CPU**, in `Material::GetAttenuationSigma()`. The GPU only ever sees σ.

That split is load-bearing. The default `attenuationDistance` is `+∞`, and a finite value
over `+∞` is exactly `0` in IEEE-754 — so the neutral case reaches the shader as a plain
zero, and no infinity, and no `inf · 0` NaN, can exist in GLSL. An `attenuationColor`
channel of exactly `0` (legal glTF, meaning "infinitely absorbing") is floored to
`kMinAttenuationChannel` for the same reason: `log(0)` is `-∞`.

## Deferred

The G-Buffer carries albedo/metallic, normal/roughness/AO, emissive/flags, velocity,
entity-ID and baked GI. None of that can hold a transmission factor, an IOR, a thickness or
an extinction coefficient, so a transmissive material written into it comes back out of
`DeferredLightingPass` as an ordinary opaque surface.

So `Renderer3D`'s draw routing reroutes a transmissive PBR material to
**`ForwardOverlayPass`** — the same mechanism a forward-only shader override already uses.
That pass binds the scene framebuffer and runs *after* the deferred composite, so the
forward PBR shader (which has the closure) shades the surface over the finished deferred
image, and Deferred and Forward show the same glass.

If the overlay pass is unavailable, the draw is **counted and warned once** rather than
silently shaded opaque — `PhysicalMaterialStats::TransmissiveDrawsWithoutForwardOverlay`.

## What is reported rather than absorbed

`GetPhysicalMaterialStats()` (`Renderer/GltfPhysicalMaterial.h`) exposes three counters.
Each is warned once and counted always, so a test can assert the count instead of scraping
the log:

- `TransmissionMapsIgnored` — an asset carried a `transmissionTexture`.
- `ThicknessMapsIgnored` — an asset carried a `thicknessTexture`.
- `TransmissiveDrawsWithoutForwardOverlay` — see *Deferred*.

## Persistence

The fields are carried by every container that persists a material, each gated so older
data still loads with the neutral defaults:

| Container | Gate |
|---|---|
| Scene YAML (`MaterialComponent`) | Keys omitted at their defaults, so pre-#970 scenes re-serialize byte-identical. A missing `AttenuationDistance` means `+∞`. |
| `.omaterial` (`MaterialAssetSerializer`) | A `PhysicalMaterial` block, written only when the material is non-neutral. Deliberately *not* in the generic `Properties` bag, which round-trips into uniforms rather than the typed setters. |
| `.omesh` / asset pack (`ImportedMaterialCodec`) | Wire version `2`; a v1 blob stops before the block and keeps the defaults. `OMeshFormat::CurrentVersion` also moves 6 → 7, so an existing warm `.omesh` is re-imported rather than serving a v1 material blob — without that, an already-imported transmissive glTF would keep rendering opaque with no error. |
| Save-game | `kSaveGameFormatVersion` 29; a pre-v29 save stops before the block. |

## Authoring

The editor inspector has a **Transmission & Volume** section under Material, and the
**Glass** preset now authors a real transmissive material rather than faking it with a base
colour alpha.

The fixture at `OloEditor/assets/models/TransmissionTest/TransmissionTest.gltf` carries
three materials — thin glass, tinted volume glass, and an extension-free control — and is
what `GltfPhysicalMaterialImportTest` reads.

## Tests

| Test | Holds |
|---|---|
| `MaterialTransmissionTest` | Neutral defaults, the Beer-Lambert derivation against an independently written spec formula, boundedness, and the hostile-input sweep. |
| `GltfPhysicalMaterialImportTest` | The Assimp key mapping, against the real fixture rather than a mock. |
| `ImportedMaterialCodecTest` | Wire round-trip, v1 compatibility, and that `+∞` survives. |
| `MaterialCopyTest` | The three hand-maintained copy paths carry the new fields. |
| `TransmissionVisualEvidenceTest` | That the shader actually applies it, on the real pipeline. |
