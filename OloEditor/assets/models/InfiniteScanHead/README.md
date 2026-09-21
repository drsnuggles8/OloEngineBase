# Infinite Scan — human head

The engine's **human head reference**, and the subject of `Scenes/DigitalHuman.olo` (issue #1222).
A photogrammetry scan of a real head and shoulders: neutral expression, eyes closed, no hair.

## Why this model is here

There was no human head in this repository. `Suzanne` is a chimpanzee, and the acceptance capture
for the AAA skin epic had been rendering its subject as a sphere — which measures the material
correctly and looks like a snowman. Every other model here comes from Khronos
glTF-Sample-Assets, and that catalogue has no human head either.

This is the head the subsurface-scattering literature uses. It ships a high-resolution albedo, so
a skin material has real pigment variation to scatter through rather than a flat colour.

## Legal — attribution is REQUIRED

**[Creative Commons Attribution 3.0 Unported (CC BY 3.0)](https://creativecommons.org/licenses/by/3.0/)**
— SPDX: `CC-BY-3.0`. This is an attribution licence, unlike the CC0 assets elsewhere in this
directory, so the credit chain below must survive any redistribution:

- **Original scan:** Lee Perry-Smith ("Infinite", 3D Head Scan), based on a work at
  [triplegangers.com](https://www.triplegangers.com). Permissions beyond the scope of this licence
  may be available at [ir-ltd.net](http://www.ir-ltd.net/).
- **Conversion to convenient formats:** Morgan McGuire and Guedis Cardenas,
  [Williams College Computer Graphics Archive](http://graphics.cs.williams.edu/data/).
- **FBX packaging:** Keijiro Takahashi, [keijiro/InfiniteScan](https://github.com/keijiro/InfiniteScan).

The upstream licence text is kept verbatim in [`LICENSE.md`](LICENSE.md). Its own words:
*"Do what you want with the files, but always mention where you got them from."*

## Files

| file | what it is |
|---|---|
| `Head.fbx` | the scanned mesh, head and shoulders |
| `Textures/lambertian.jpg` | diffuse albedo, 8.6 MB |
| `Textures/bump-lowRes.png` | a BUMP map, not a tangent-space normal map — do not bind it to `u_NormalMap` without converting it first |

## Using it

It is authored at roughly 1.4 units tall for the whole bust and its origin is at the base of the
neck, **not** at the head's centre. `DigitalHuman.olo` therefore scales it by `0.38` and drops it
by `0.285` to put the head on the scene origin. Anything positioned relative to the face must be
placed against that transform — the first attempt kept coordinates authored for a 0.2 m sphere and
left eyeballs sitting on the forehead.

**It has closed eyelids and its own lips and ears.** So it is a subject for the SKIN children
(#1241/#1242/#1243) and not for the ocular (#1244) or oral (#1245) ones, which keep their own
fixtures.
