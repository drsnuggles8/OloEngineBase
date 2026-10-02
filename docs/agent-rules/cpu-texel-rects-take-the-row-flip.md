# A texel rect the CPU computes is in the rows the sampling reads

Applies to: CPU code that picks texels of a map the GPU rendered from a camera or light matrix — a
copy region, a readback region, a scissor, a clear rect. Today that is the shadow pass's opaque-copy
rect (`ShadowReceiverTexelRect`, #1533).

## The rule

Project through `RHI::AdjustProjectionForShaderReconstruction(matrix)`, the matrix the sampling
reads through, never through the raw GL-convention matrix the CPU keeps. On Vulkan every off-screen
target is stored top-down (#691), so a world point lands on memory row `(1 − v)·size`, and copy and
readback calls address memory rows. The adjusted matrix is identity on GL, so the change cannot move
a GL frame.

It is the CPU half of [screen-uv-unpack-takes-the-row-flip.md](screen-uv-unpack-takes-the-row-flip.md):
values computed on the CPU never pass through a matrix upload, so nothing in
[capture-paths-take-the-projection-seam.md](capture-paths-take-the-projection-seam.md) catches them.

## What it looked like

The shadow pass copies only the texels a coat's strands can read into the opaque copy they sample,
so the body shadows its fur. The rect came from the raw matrix, so on Vulkan the copy landed on the
mirror image of the coat's rows. The strands read whatever an earlier camera's copy had left in
theirs.

Live on Vulkan, the full-body view after the low hero view showed near-black, straight-edged patches
on the muzzle and in a V across the flank and hind leg: 4,440 dark pixels in the dog's box against
GL's 1,429. It stayed that way for as long as the camera held still. After other views the coat
looked clean but had lost the body's shadow on its inner legs (1,135). Both states are the same bug.

## Why nothing caught it

- Every headless test that renders the coat runs on GL.
- The mirror often overlaps the real rows. The rect is padded by 96 texels, and a coat near the
  cascade's centre row is close to its own mirror.
- The symptom depends on history. Stale texels shadow only if an earlier camera put something
  nearer the light there, so a capture from a clean start looks right, and so did the E1 runs.
- A missing body shadow on fur reads as a softer look, not as a fault.

## The diagnostic that found it

- Replay a fixed sequence of views, then shoot the suspect view again: without moving, after more
  frames, and after each other view. A history-dependent count (a dark-pixel count in the subject's
  box is enough) points at stale data, not at the shading.
- Straight edges on a curved body are a texel rect projected onto it. Look for the CPU code that
  chose the rect.

## Checklist

- `grep -n "CopyImageSubDataRegion\|ReadTextureSubImage"`: wherever the region comes from a
  projection, it goes through the reconstruction matrix.
- A CPU test models the lookup itself (the uploaded matrix, then `*0.5+0.5`, then the tile), under
  `RendererAPI::SetAPI(Vulkan)` as well as GL. It needs a case whose rows and mirror are disjoint, and
  a negative control that keeps the raw-matrix rect and expects it to miss
  (`ShadowReceiverTexelRectTest`).
