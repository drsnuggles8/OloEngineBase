# A lookup table is read between its first and last texel centres

Applies to every shader that reads a 2D table by a parameter on [0, 1]: the split-sum BRDF table
(`u_BRDFLutMap`), and any table added later (a transmittance table, a fit, a ramp).

## Rules

1. **Clamp the coordinate to `[0.5 / size, 1 - 0.5 / size]` before the read.** A `Texture2D`'s
   sampler repeats on both backends. Within half a texel of either end, a bilinear tap reads the
   opposite edge of the table.
2. **Read the BRDF table through `oloEnvBRDF(lut, NdotV, roughness)`** (`include/PBRCommon.glsl`).
   No shader calls `texture()` on it directly; ten sites did.
3. **Test a table at the ends of its range, on a flat surface.** A surface that faces the camera
   (`NdotV = 1`) and a fully rough one (`roughness = 1`) are the ends of the BRDF table, and a
   curved test subject spreads the fault over a few pixels of its silhouette where nobody looks.
   `ProbeBakeVisualEvidenceTest` holds a flat rough wall to one colour where it faces the camera
   and a few degrees to the side.

## What happened (#1352)

The first pictures `ProbeBakeVisualEvidenceTest` wrote showed a faint disc on the wall behind its
spheres, 2.5 degrees across, exactly where the view ray met the wall at a right angle. Its position
in a second picture from another camera put it at infinity along the wall's normal, so it was not
an object: it was the set of pixels with `NdotV` within half a texel of 1.

The table is read at `(NdotV, roughness)`. At `NdotV = 1` the bilinear tap wrapped to the
`NdotV = 0` column, whose bias term is near 1, and at `roughness = 1` to the mirror row. The wall
(roughness 1, seen head-on) got a quarter of the unattenuated environment colour added: 120 against
98 of 255 in green and blue, on both backends and on every rendering path. Every fully rough
surface was also 2 of 255 brighter than it should be, everywhere, from the roughness wrap alone.

It had been in every frame lit by an environment. No test looked at a flat rough surface head-on
under one: the subjects were spheres, where the affected pixels are a dot at the centre.
