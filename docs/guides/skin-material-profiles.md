# Skin material profiles

What a `.oloskin` file holds, what units it is in, and how to look at the four outputs a skin
surface exposes. Issue #1231; the design decision behind the three separate selectors is
[ADR 0024](../adr/0024-material-kind-is-not-the-closure-version.md).

Scattering itself is **not** here — that is issue #1241. What this ships is the material/profile
contract it needs and the separated diffuse/specular outputs it will blur.

## The three selectors, and why they are three

| Selector | Question | Set where |
|---|---|---|
| `MaterialKind` | What is this surface? `Generic` / `Snow` / `Skin` | Material inspector → **Material Kind** |
| `PBRModel` | Which version of the closure evaluates it? `Legacy` / `Closure V2` | Material inspector → **PBR Model** |
| `SkinEvaluationModel` | Which version of the skin transport was this profile authored against? | The `.oloskin` file's `EvaluationModel` |

They are independent. A skin material shades with either closure version; correcting a BRDF does
not change what a surface is; and #1241 will ship a new transport version without touching a single
material's kind.

## Authoring a profile

A `.oloskin` is YAML. Drop one in `Assets/` and drag it onto a Skin material's **Skin Profile**
slot in the inspector.

```yaml
SkinProfile:
  Name: Reference Head
  EvaluationModel: 0                 # SkinEvaluationModel; 0 = DiffuseSpecularSplit
  Units: "colours: linear Rec.709 (unitless 0..1); lengths: millimetres"
  ScatterColor: [0.85, 0.55, 0.45]   # transport albedo, LINEAR Rec.709, 0..1
  ScatterRadiusMM: [1.55, 0.8, 0.55] # per-channel mean free path, MILLIMETRES
  ThicknessScale: 1000               # material thickness (metres) -> mm; 1000 is identity
  SpecularTint: [0.97, 0.93, 0.9]    # multiplies the SURFACE specular, LINEAR Rec.709, 0..1
```

`OloEditor/SandboxProject/Assets/Materials/ReferenceHead.oloskin` is a working example.

**Units and colour space are the contract, not documentation.** Every colour is linear Rec.709
with no transfer function applied, because that is the space the lighting stack works in; every
length is millimetres, because a scattering radius authored in metres is a number nobody
recognises as wrong. The editor labels them the same way and the serializer writes the same names.

**Bounds are rejection bounds.** A scatter radius of 0 divides by zero in any diffusion profile, so
the floor is positive (1e-3 mm). Finite out-of-range values clamp; non-finite values (`nan`,
`.inf` — both legal YAML floats) revert to the default and are logged, because `std::clamp` passes
NaN straight through.

## What a skin material does today

`SpecularTint` is the only profile parameter with a visible effect before #1241: it multiplies the
**specular half** of the lighting and leaves the diffuse half alone. That asymmetry is the whole
point of the split — a combined `kD * albedo / PI + specular` cannot be tinted, or blurred, without
dragging the other half along.

Everything else in the profile is carried, validated and transported, waiting for the diffusion
that reads it.

## Missing profiles are loud

A `MaterialKind::Skin` material with no profile, an unloadable handle, a handle pointing at
something that is not a profile, and more than seven distinct profiles in one frame are each
reported once per handle and **counted** (`SkinProfileTable::GetFallbackCount`). The frame still
renders — the substituted parameters are real and finite — but the log says so, because a head
quietly shaded with neutral defaults looks exactly like a correct frame.

Seven is the budget because the deferred path names a profile per pixel in three bits of the
G-Buffer flags lane. See ADR 0024 for what that lane costs.

## Looking at the four outputs

`olo_render_set_debug_view { mode: ... }`, or the same tokens from the editor:

| Token | Shows | Units |
|---|---|---|
| `materialdiffuse` | the diffuse half of the split | linear HDR radiance, Rec.709 |
| `materialspecular` | the specular half | linear HDR radiance, Rec.709 |
| `skinprofileid` | per-pixel profile identity, one hue per slot; black = names no profile | — |
| `skinmask` | per-pixel scattering mask | unitless 0..1, greyscale |

**Deferred path only.** Those four are produced by the deferred lighting pass, which is the only
stage that has all four per pixel and the only one with a fullscreen substitution point. The
forward and forward+ paths compute the same split — `SkinProfileParityEvidenceTest` asserts that
the profile reaches the specular half, and only the specular half, on all three — they just have no
pass at which to swap it for the composite.

The scattering mask is `kind == Skin ? 1 - metallic : 0`: where subsurface transport *may* happen,
not how strong it is. A metal has none. An authored mask map arrives with #1241, together with the
diffusion it masks.

## When you add a field to a profile

Three places, in this order:

1. `SkinProfileParameters` in `OloEngine/src/OloEngine/Renderer/SkinProfile.h`, plus its bounds and
   its line in `Sanitize()` — that function is the only validation gate and every reader routes
   through it.
2. `SkinProfileSerializer` (YAML + asset pack share one body), and the inspector's read-only echo
   in `SceneHierarchyPanel.cpp`.
3. Only if the shader needs it: `PBRMaterialUBO` (and **all eight** GLSL blocks that mirror it —
   see [notes-renderer.md](../agent-rules/notes-renderer.md)) for the forward paths, and
   `DeferredControlsData::SkinProfileParams` for the deferred one. A field the shader does not read
   yet stops at step 2.
