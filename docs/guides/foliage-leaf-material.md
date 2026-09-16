# The foliage leaf material

What a foliage layer's leaf material holds, what each field does to the image, and how to look at
the transmission term on its own. Issue #1234.

A leaf is a **thin two-sided surface** — one lamina a fraction of a millimetre thick — so light that
enters the lit face leaves the other face in essentially the same place. That is why this is its own
`MaterialKind` (`Foliage`) rather than a small-radius [skin profile](skin-material-profiles.md):
skin's transport diffuses energy *across* the surface, which is the one thing a leaf does not do and
which would blur every leaf edge into its neighbour. What the two share is the plumbing — the
material kind, the G-Buffer slot field, the per-frame profile table — and nothing else.

Before #1234 the deferred path hard-coded a foliage pixel's surface (roughness `0.9`, full AO, the
raw interpolated normal) and the forward path lit it with one directional light, no shadow and a
flat `albedo * 0.3` ambient. All of that is gone.

## Turning it on

Select a terrain with a `FoliageComponent`, open a layer, and find **Leaf Material** in the
inspector. **Transmission Strength is the master switch and it defaults to 0.** A layer authored
before #1234 — in a scene or in a save-game — loads at 0 and renders exactly as it used to; the
material has to be asked for.

`OloEditor/SandboxProject/Assets/Scenes/FoliageLeafTransmission.olo` is a working example: a backlit
clump of authored pines and a shrub layer, under a low sun placed behind them.

## The fields

| Field | What it does | Notes |
|---|---|---|
| **Normal Map** | tangent-space leaf normals — veins, curl | LINEAR data, not sRGB |
| **Normal Strength** | tangential scale of that map | 0 = the geometric normal, 1 = the map |
| **Roughness** | the surface roughness | now read by *all three* paths; a Roughness Map multiplies it |
| **Roughness Map** | greyscale, red channel | multiplies Roughness, so a white map is a no-op |
| **Transmission Strength** | how much light gets through | **0 = the material is off** |
| **Transmission Color** | the colour light takes on the way through | linear, 0..1 |
| **Thickness** | optical thickness of the lamina | 0..1; the Thickness Map multiplies it |
| **Thickness Map** | greyscale, red channel | thin tips glow, thick midribs stay dark |
| **Lobe Distortion** | how far the exit direction bends back towards N | 0 = a hard glint only when the eye is in line with the light; higher spreads the glow |
| **Lobe Power** | falloff exponent of the forward-scattering lobe | clamped to ≥ 1 — below 1 the lobe turns inside out |
| **Lobe Wrap** | Lambertian back-face mix | without it a leaf lit at 90° grows a hard terminator down its middle |
| **Environment Transmission** | how much irradiance arriving on the FAR face is transmitted | the *indirect* half of the term |

The three maps are authored as **paths**, like the layer's Albedo Path and Mesh Path beside them,
and are loaded directly by `FoliageRenderer`. A path that will not open is reported once in
`OloEngine.log` and the layer shades with its authored constant — never silently with a black map.

## What "shadowed" means here, and why it matters

The term is deliberately split in two:

- the **direct** half is per light and is multiplied by the **same shadow factor the reflected lobe
  uses**, so a leaf behind a trunk stops glowing;
- the **indirect** half is the environment irradiance sampled along `-N` — the sky *behind* the leaf
  — scaled by Environment Transmission.

Neither half is a constant. #1234's second acceptance criterion names "an unshadowed ambient
constant" as the wrong answer, and the split is how that is avoided rather than asserted.

One consequence worth knowing when tuning: a backlit leaf's shading normal points away from the sun,
so the shadow lookup is biased along the **lit-side** normal instead
(`oloFoliageShadowNormal`). If you ever see backlit leaves that refuse to glow, check the scene's
authored `ShadowNormalBias` before the lobe — some sample scenes author 10× the engine default.

## One evaluation, four programs

Everything above is evaluated by `OloEditor/assets/shaders/include/FoliageSurface.glsl`, which is
included by the forward foliage shader, the deferred G-Buffer writer, the impostor pair and the
deferred lighting pass. Forward and Forward+ are literally the same program (`SelectFoliageRenderStream`
routes both to `FoliageRenderPass`), so "the paths preserve the same material meaning" is a property
of the build rather than a claim.

**The distant impostor card transmits at a CONSTANT thickness.** An octahedral impostor atlas bakes
albedo + coverage and an object-space normal/depth pair; it has no thickness channel. What is lost
past the impostor hand-over is the map's *variation*, not the transmission.

## Looking at the term on its own

`olo_render_set_debug_view { mode: 'materialtransmission' }` replaces the composite with the leaf
transmission term alone, in linear HDR radiance (Rec.709). **Deferred path only** — the material
debug views are produced by the deferred lighting pass.

| Mode | Shows | Units |
|---|---|---|
| `materialdiffuse` | the diffuse half of the split | linear HDR radiance, Rec.709 |
| `materialspecular` | the specular half | linear HDR radiance, Rec.709 |
| `materialtransmission` | the leaf transmission term alone | linear HDR radiance, Rec.709 |

It is **black on every pixel that is not `MaterialKind::Foliage`**, which makes it a direct test of
whether the kind and the thickness lane reached the G-Buffer at all — not only of the lobe's shape.

## The slot budget

The lobe's per-layer parameters reach the deferred lighting pass through a **seven-slot table**
(`FoliageLeafProfileTable`), indexed by the same three-bit G-Buffer field skin's profile slot uses.
Layers that authored *identical* parameters share a slot, so a forest of one species costs one.

An **eighth distinct** leaf material is refused loudly: it logs once, is counted, and that layer
renders as `Generic` — no transmission rather than somebody else's transmission — **on the deferred
path only**. The forward paths carry the parameters per draw and need no slot, so they keep shading
it correctly. If a scene ever hits this, merge layers that share a leaf material.
