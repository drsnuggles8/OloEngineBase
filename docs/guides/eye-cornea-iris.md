# Eyes: cornea, iris and tear line

Transport version 5 (`SkinEvaluationModel::OcularSurface`), issue #1244. It adds **one** thing to a
skin profile:

- a **corneal refraction** — the iris sits ~2.5 mm behind a transparent dome of index 1.336, so a
  viewer never sees it where it is. Version 5 refracts the view ray at that dome, finds the iris
  point behind it, and applies the iris's own depth response — a limbal ring, a pupil and a dish
  tilt — **at that point**.

Everything else an eye needs, version 4 already had. Maths and every physical decision:
[`Renderer/SkinOcularSurface.h`](../../OloEngine/src/OloEngine/Renderer/SkinOcularSurface.h).
Shader side: [`include/SkinOcularSurface.glsl`](../../OloEditor/assets/shaders/include/SkinOcularSurface.glsl).
The measured comparison the approximation was chosen on:
[`experiments/eye-cornea-reference/compare_refraction.py`](../../experiments/eye-cornea-reference/compare_refraction.py).

## The left/right convention

**Both eyes name the same `.oloskin`.** There is no left profile, no right profile, no mirrored
asset and no `IsLeftEye` flag.

The only per-eye quantity the shader needs is the **optical axis**, and it reads that from the
entity transform's **+Z** (`u_Model[2].xyz`), which every fragment stage that shades skin already
has. So two eyes in one head differ by their transforms' rotation and by nothing else:

- a **gaze change is a transform change** — rotate a globe's Y and its iris moves under its own
  cornea, with no material touched;
- an **iris colour change is a one-file edit**.

A flag would have put a per-entity fact in a per-asset record, so the day someone wants a third eye
looking somewhere else they would need a third profile differing in one bool.

> **Two requirements, both silent if broken.**
>
> **The mesh must be a uniformly scaled sphere.** The shader reads the globe's radial direction out
> of the interpolated normal; a non-uniform scale makes the normal stop being radial and the iris
> shears.
>
> **Each eye must be its own entity.** `u_Model` is the *draw's* transform, so eyes modelled as
> submeshes of one skinned head mesh all receive the head's forward axis while their normals come
> from the bone deformation — every eye refracting about the head's axis rather than its own, which
> reads as two eyes that will not converge. Resolving a per-eye *bone* transform in the fragment
> stage would need the bone index routed through a flat varying on four shaders and a palette lookup
> per pixel, to serve an asset shape this repo does not have.
>
> Neither is enforced per pixel: falling back to a painted iris would hide the authoring error behind
> a plausible frame, which is exactly what `no-silent-fallbacks` forbids.

`Assets/Scenes/Eyes.olo` is the worked example: two globes naming
`Assets/Materials/EyeIris.oloskin`, differing in `Translation.x` and `Rotation.y`.

## Authoring

Thirteen fields in the `.oloskin`'s `Ocular` block, read **only** at transport version 5.

| field | default | what it does |
|---|---|---|
| `OcularStrength` | `0.0` | **the master switch.** 0 is no eye and is bit-identical to the version-4 frame. |
| `RefractionStrength` | `1.0` | **the quality ladder.** 1 is the full model; 0 is a painted iris that still has a pupil, a ring and a dish tilt. Continuous in between. |
| `EyeRadiusMM` | `12.0` | the globe's radius — the reference length every other length is divided by. |
| `CorneaRadiusMM` | `7.8` | anterior corneal curvature. **Set it equal to `EyeRadiusMM` if your mesh already has corneal geometry.** |
| `IrisRadiusMM` | `5.85` | the visible iris radius. **Also sets the limbus**, so one length decides both. |
| `PupilRadiusMM` | `2.0` | a mesopic pupil. |
| `IrisPlaneDepthMM` | `2.48` | apex → iris plane. **Not the clinical 3.6** — see below. |
| `CorneaIor` | `1.336` | the refracting index. **Not 1.376** — see below. |
| `LimbalRingWidthMM` | `0.7` | the dark band's width. It sits **one width inside** the iris edge, leaving room for the edge fade. Floored at 2% of the iris radius — a zero span is a NaN in GLSL's `smoothstep`. |
| `LimbalRingStrength` | `0.0` | how much albedo that band takes. 0 returns exactly 1. |
| `PupilDarkening` | `1.0` | the pupil is an aperture, so it defaults to black. |
| `IrisConcavity` | `0.0` | the iris dish's tilt — the depth response. |
| `IrisColor` | `[1, 1, 1]` | the iris's own colour, **multiplied** into the albedo inside the disc. White is neutral. |

`OcularStrength` defaults to neutral, so **moving a profile to version 5 changes nothing** until a
field is authored. That is the identity arm `SkinOcularSurfaceEvidenceTest` captures on all three
raster paths, and the first thing to check if a head changes appearance on a version bump.

> **`IrisColor` is a multiply, not a replace.** That is what makes white neutral, and it is the
> physically sensible direction: an iris is pigmented tissue seen through the same chamber the
> sclera is, so it is darker and more saturated than the surface around it, never brighter. Without
> it the iris is exactly the colour of the sclera and the eye reads as a sphere with a dot — which
> is how this feature's first evidence capture came out.

> **Five clinical lengths and one index are authored; every ratio the shader wants is derived.**
> `12.0`, `7.8` and `5.85` are numbers you can find in Bennett & Rabbetts and check against a real
> eye. `0.7485`, `1.5385` and `0.8724` — the eta, the curvature ratio and the limbus cosine — are
> numbers nobody can check, so the CPU computes them once in the three lane packers.

### The tear line is authored, not branched

There is no tear-line code path. A tear film and a saliva film are the **same interface** — a thin
water layer over wet tissue — and their indices differ by 0.007: an F0 of **0.0208 against 0.0201**,
a 3% difference. A second implementation would be a second opinion about one number.

So a tear line is #1245's wet coat with the index of tears, on the lid-margin geometry:

```yaml
Oral:
  CoatStrength: 0.85     # a meniscus is standing liquid, not a film
  CoatRoughness: 0.025   # smoother than the cornea — this is what makes it a LINE
  CoatIor: 1.337         # tears
Ocular:
  OcularStrength: 0      # a meniscus has no cornea and no iris behind it
```

`Assets/Materials/EyeTearLine.oloskin` is the shipped example. This is the same shape #1245 used for
teeth: a genuinely different response reached by authoring rather than by a second code path.

## Two defaults that look wrong and are not

### `CorneaIor` is 1.336, the **aqueous humour** — not 1.376, the corneal stroma

This is the trap in the whole feature, because 1.376 is the number you look up for "cornea". The ray
spends 0.55 mm in the stroma and 3 mm in the aqueous behind it, so **the index that decides where it
lands is the one it ends in**. Worst-case RMS error against a two-surface trace, on a mesh with real
corneal geometry:

| index | RMS error, as a fraction of the iris radius |
|---|---|
| aqueous, 1.336 | **0.10%** |
| keratometric, 1.3375 | 0.07% |
| stromal, 1.376 | 1.04% |

The error is also in the direction that looks plausible — it *under*-refracts, so the eye reads
slightly painted.

> **Do not re-measure this on a sphere-primitive eye.** There the ordering *inverts* (aqueous 4.01%,
> stromal 3.48%) because a higher index refracts harder and refracting harder partly cancels the
> entry-point error a sphere's missing corneal bulge introduces. That is two errors cancelling, not
> a better model, and an index tuned that way would be wrong the day a real eye mesh arrived.
> Question 2b of the optical reference records both columns for exactly this reason.

### `IrisPlaneDepthMM` is 2.48, not the clinical 3.6

The clinical anterior chamber depth is measured from the **corneal** apex, and a sphere primitive has
no corneal bulge — its apex sits 1.12 mm further back. 3.6 − 1.12 = 2.48, which is arithmetic rather
than a fitted number.

An eye mesh **with** corneal geometry authors the clinical 3.6 here, together with
`CorneaRadiusMM == EyeRadiusMM`.

## What the approximation costs, and where it runs

The model is **one refracting surface** with the mesh normal bent into the corneal dome's by
`sin(phi) = (EyeRadiusMM / CorneaRadiusMM) * sin(theta)`. Worst-case RMS against a two-surface trace
through clinical radii, over a 0–60° sweep:

| model | RMS, as a fraction of the iris radius |
|---|---|
| painted (no offset) | 16.7% |
| **shipped, sphere-primitive mesh** | **3.9%** |
| shipped, mesh with corneal geometry | 0.10% |

The ground truth is itself validated first: it reproduces the literature's **1.13×** entrance-pupil
magnification as **1.123×**, by two independent methods agreeing to 0.001 mm.

**The 3.9% residual is an asset limitation, not a shader one.** A sphere has no corneal bulge, so
the ray enters 1.1 mm behind where it should — an *entry-point* error, which no parameter reaches.
Question 4b of the reference sweeps the authored depth to prove it: the curve is flat-bottomed at
~7% (max) and never approaches zero.

### It runs in the material stage, not the lighting stage

This is the structural difference from every skin version before it. A refraction changes **which
surface point you are looking at**, which is a property of the view alone — no light direction, no
radiance, no shadow factor. So it resolves once, in the fragment shader that resolves the material:

```
PBR_MultiLight.glsl / PBR_MultiLight_Skinned.glsl   (forward, Forward+)
PBR_GBuffer.glsl    / PBR_GBuffer_Skinned.glsl      (deferred)
```

`include/DeferredLightingShared.glsl` does not call it at all — by the time that file runs, the
G-Buffer already carries an albedo and a normal with the cornea in them. Three consequences:

- **the three raster paths agree by construction**, not by three matching edits;
- **no G-Buffer lane is touched** — the flags lane #1288 is the receipt for is untouched, and there
  is no fourth deferred profile table;
- **layer sorting cannot go wrong.** Cornea, iris and tear film are not three depth-sorted surfaces;
  they are three terms at one surface in a fixed code order — refract, then the iris response, then
  the surface closure, then the coat. There is no arrangement of them that can sort incorrectly.

### Known gap: virtual geometry

An eye rendered through **virtual geometry** shades painted — no refraction, no iris, no limbal
ring. `include/VirtualGBufferFragment.glsl` declares the four lanes (it must, or the trailing heap
offsets move and every material texture samples the wrong descriptor) but does not call
`oloSkinOcularApply`.

Not reached today: an eye is a small uniformly-scaled sphere and virtual geometry is for authored
high-poly meshes, so a sphere primitive never takes that path. Recorded here and in that file so the
missing iris has an explanation written down rather than a bisect.

## The quality ladder

`RefractionStrength` is the fallback for a path that cannot afford the full model. It is **authored,
never chosen by the renderer** — an effect that silently degrades is one nobody notices is missing.

| value | what you get |
|---|---|
| `1.0` | the full refracted model |
| `0.5` | half the parallax; the iris-plane landing point is interpolated, so this does not pop |
| `0.0` | a painted iris that still has a pupil, a limbal ring and a dish tilt |

The cost, measured at 384×384 offscreen, is recorded per path in
`OloEditor/assets/tests/visual/EyeCornea_Timing.txt`.

## A front-on screenshot cannot tell you whether this works

At a head-on view of a single eye the refracted and painted iris points **coincide by symmetry**, so
a front-on capture of a broken eye and a correct one are the same image. Everything the feature is
worth is off-axis:

| gaze | apparent pupil-centre shift, as a fraction of the iris radius |
|---|---|
| 0° | 0.0% |
| 10° | 7.9% |
| 20° | 14.9% |
| 30° | 19.7% |

So when checking an eye in the editor, **rotate it**. `Assets/Scenes/Eyes.olo` authors its globes
slightly converged rather than dead ahead for this reason: a scene whose default pose is straight
through the camera is a scene whose default screenshot proves nothing.
