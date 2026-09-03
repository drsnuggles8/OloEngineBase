# A texture you create must be COMPLETE before anything samples it, or AMD reads zero and NVIDIA does not

Set the sampler state at creation. A single-level texture needs
`GL_TEXTURE_MAX_LEVEL = 0` (the default min filter mipmaps, and the levels it looks
for do not exist), and an INTEGER internal format needs `GL_NEAREST` on both
filters (GL 4.6 §8.17 makes any other filter incomplete for integer textures).
Sampling an incomplete texture is undefined: NVIDIA returns the data anyway, Mesa
returns **zero** — `texelFetch` included, which is the part that surprises people.

`RHI::IsIntegerFormat` (RHITypes.h) and `IsIntegerFormat` (Texture.h) are the two
predicates; `OpenGLRendererAPI::CreateTexture2D` / `CreateTextureCubemap` and
`OpenGLTexture2D::CreateStorage` are the two places that must apply them.

## It has cost two subsystems, and neither looked like a texture bug

**The text renderer (Texture.h's note).** A linear-filtered RG16UI band texture
sampled as all-zero on AMD, so every glyph covered no pixels. Geometry, draw calls
and logs all looked healthy; the text was simply not there.

**The virtual shadow map (issue #1015).** The R32UI physical page pool is created
through the RAW RHI handle path — `CreateTexture2D`, which set no sampler state at
all — and read with `texelFetch`. On the AMD CI box every resident page fetched 0,
`vsmDecodeDepth` reads 0 as an occluder at depth 0, and the whole floor sampled as
shadowed. Two visual-evidence tests failed for a year's worth of plausible reasons
before the real one: the frame was half the luma of the CSM frame in the LIT
region, which reads as "the shadow is in the wrong place", not "a texture returned
zero".

The tell, in hindsight, was the residency probe: the frame was wrong exactly where
pages were RESIDENT and right exactly where none were — because the sampler's
no-page path returns `1.0` (lit) without touching the texture at all.

## Why the dev machine cannot find this

Every developer here runs NVIDIA, where an incomplete texture returns its contents
and nothing is ever wrong. The bug only exists on Mesa, so it arrives as "the AMD
nightly is red" long after the code shipped. Two ways to catch it earlier:

- **A standalone probe settles it in seconds.** Compile a headless EGL program on
  the box (`gcc probe.c -lEGL -lGL`), create the texture the way the engine does,
  `texelFetch` it from a compute shader, and print what comes back. The #1015 probe
  returned `00000000` for the default state and the written value with
  `NEAREST` + `MAX_LEVEL=0`, which took the guesswork out entirely.
- **Suspect it whenever a GPU feature is "wrong everywhere it is active and right
  everywhere it is not"** on one vendor. A sampled zero is not noise; it is a
  specific value with a specific meaning in whatever decode follows it, and that
  meaning is what you see on screen.

## Guard

`RHIFormatCompleteness` (RHIEnumLoweringTest.cpp) pins both predicates: every
integer format is named as one, every one of the other 18 formats is named as not,
and a `static_assert` on the enum's size fails the build when a format is appended
without being classified. Enumerating by hand is only as good as the next person
remembering; the assert is the part that holds.
