# Skin thickness bake

Bake a skin **thickness map** (issue #1242's `ThicknessMapPath`) from a mesh's own geometry, instead
of painting one. Written for issue #1394, where it produced
`OloEditor/SandboxProject/Assets/Textures/InfiniteScanHead_Thickness.png` for the scanned reference
head.

```powershell
python -m venv .venv
.venv\Scripts\python -m pip install numpy pillow ufbx trimesh embreex
.venv\Scripts\python tools\skin-thickness\bake_thickness.py        # ~30 s, reproduces the committed map
```

## What it measures

For every texel the mesh's UV layout covers, it recovers the surface point and the interpolated
normal and casts 16 rays into the surface, in a 30° cone around −N. The texel's thickness is the
**median** hit distance. The median is used because a single ray slipping through a crack in a
photogrammetry mesh would drag a mean to "very thick", and a single ray grazing a neighbouring fold
would drag a minimum to "paper thin".

Three cases are written as **thick** (the value 255), the conservative reading from
[skin-transmission.md](../../docs/guides/skin-transmission.md) ("a missing thickness transmits
nothing, not everything"):

- a crossing whose **far side is enclosed**, see below;
- a texel where most rays miss, because the surface is open there;
- a texel the UV layout does not cover. Covered texels are dilated 8 texels outward over these, so
  bilinear filtering and mips never blend a thin ear with the empty atlas.

## Encoding

`value = min(thickness, 20 mm) / 20 mm`, 8-bit, **linear**, grey in all three channels (the shader
reads red). The material's `ThicknessFactor` is therefore **0.02** (20 mm in metres), and
`factor × map` is the measured thickness. Twenty millimetres is where the reference profile's red
transmittance falls below 1e−5, so clamping there loses nothing that renders.

The file name carries no colour keyword (`albedo`, `color`, `_d.` …), so
`TextureCompression::IsLikelyColorTexture` loads it linear. A name that tripped that heuristic
would put a gamma curve through the optical depth.

## The far side must be open

Local thickness alone is not enough, and the closed eyelids are why. Each is a 3–6 mm fold of skin
over the scan's eye pocket. The inward rays cross it and exit into that pocket, then re-enter the
head after a median 19 mm (86% of them). The ear's rays mostly exit into open air (only 36–38%
re-enter within 40 mm). A backlight can reach the far face of an ear, but it reaches the far face
of an eyelid only by crossing the whole head.

So each crossing is kept only if a cone of 8 rays from its exit point, 60° around the direction it
was travelling, mostly escapes the mesh (`--far-open-fraction`, default 0.5). This takes the eyelids
from 2 999 thin texels (<5 mm) to 475 and the lip-and-jowl band to almost none. The ears keep
about half of theirs: the helix and rim, the parts that stand free of the skull.

The renderer cannot do this itself, which is why the bake has to. The directional shadow pass culls
**front** faces, so a closed mesh never occludes its own far side. A shadowed backlight also makes
a ~3 mm ear occlude **itself** at editor cascade resolution (see *Two limits, stated* in the
transmission guide). `DigitalHuman.olo`'s backlight is therefore unshadowed, and this test is what
keeps the eyelids dark.

What remains are thin lines at the eyelid crease, the lip seam and the nostril rims. At those folds
the far face opens into the crease gap, which faces the camera rather than the light. A
direction-agnostic test cannot separate that case. Raising the threshold to 0.9 keeps the same
eyelid-to-ear ratio and loses most of the ear.

## Scale

`--scale` is metres per mesh unit. The default, `0.38`, is `DigitalHuman.olo`'s own transform, so the
map agrees with the geometry a viewer sees. That scene's head is roughly 10–15% under adult size:
0.146 m across the ears and 0.174 m deep. The measured thicknesses are therefore that much under
anatomical ones, which is within what a scan's ear geometry can resolve anyway. If you place the head
at another scale, scale `ThicknessFactor` by the same ratio. The map itself does not change.

## What it produced on the reference head

Texels under 20 mm, after the far-side test:

| region | texels | p10 | median |
|---|---|---|---|
| left ear | 4 806 | 3.1 mm | 7.0 mm |
| right ear | 3 942 | 3.5 mm | 5.7 mm |
| closed eyelids (the crease line) | 1 123 | 2.8 mm | 5.9 mm |
| nostril rims | 322 | 3.8 mm | 10.7 mm |

98% of the atlas clamps to 20 mm.

## Checking a new bake

The UV convention (row 0 at the top, v running up) matches the head's own albedo,
`Textures/lambertian.jpg`. Overlay the thin texels on that albedo: they must land on the ears. A map
sampled upside down puts the glow on the neck, and
`SkinReferenceHeadBacklit.TheEarTransmitsUnderBacklightAndVersionOneDoesNot` fails on it. That test
compares the ear with the forehead and with the closed eyelids.
