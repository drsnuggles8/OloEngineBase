# A coat's colour: map, root UVs and pigment (#1558)

Read before touching a groom's colour map (`GroomCoatComponent::m_ColorMap`, `GroomRegionMap`), the
root UVs a groom source writes (`build_dog_groom.py`, any DCC export), a generator's coat-map bake
(`build_dog.py`'s `bake_coat_map`), or a coat with regions of very different pigment — black, rust
and white on one dog.

## The rules

1. **A groom's root UVs are in the engine's runtime texture convention: v up.** The engine loads
   every image bottom row first (`stbi_set_flip_vertically_on_load_thread(1)`), Assimp's glTF reader
   flips mesh v to match, and `GroomRegionMap` samples a texture's rows in that order. So a strand's
   root UV must be the UV its skin has *after import* — Blender's own, not glTF's v down. A source
   written in glTF's convention reads every strand's colour from the vertically mirrored place in
   the atlas. Pinned by `TheRootUVsAreTheSkinsUnderThem`: some skin vertex within 1.5 cm of a root
   carries the root's UV.

2. **Check a colour pipeline with a map whose regions contrast.** `build_dog_groom.py` wrote glTF's
   v down from #1533 until #1558, and the golden retriever's coat looked right throughout: its
   palette is gold, cream and red-gold, so a mirrored read is still a gold. The first coat with
   black, rust and white showed white fur over the black shoulders, ears and thighs. Two
   experiments settled it in minutes: a uniformly black map made the white vanish (the white came
   from the map, not the shading), and dumping each strand's root UV with the colour the engine's
   own `GroomRegionMap` sampled showed the sample matched the PNG read upside down (mean |Δ| 0.003)
   and not upright (0.36). Every shipped coat, the golden's included, is cooked from the fixed
   generator since #1558 (which closes #1560).

3. **The coat map's bytes are the groom's LINEAR tint.** `Scene::ResolveGroomRegionMap` reads the
   texture's stored bytes and never decodes sRGB; DogSkin's albedo, the same texture, is decoded on
   the GPU. So the generator writes the tint itself into the PNG, and the skin a gap shows is the
   tint's sRGB decoding, a shade darker. The golden's palette was graded as the sRGB encoding the
   groom read, so `coat_colour` encodes it and its look stays as it shipped; every other breed's
   palette is the tint.

4. **A tint on the light a fibre returns cannot make dark fur. Put the pigment inside the fibre.**
   Under `GroomFibrePigmentMode::BaseColor` a strand's coat tint multiplies TT, TRT and the
   multiple scattering after the fibre is shaded with the coat's base colour. Over a nearly clear
   base fibre, backlit TT is dozens of times the lit coat, and a black tint of 0.02 leaves a
   fiftieth of it: a Bernese's black read grey, and white where the coat was thick. Under
   `BaseColorPerStrand` the strand's absorption comes from its own colour (base × tint, through
   BaseColor's own inversion, tabulated as `GroomFibreParams::PigmentTable`), TT and TRT are absorbed
   inside it, and what is left of a black strand is its sheen. Measured backlit on the TT lobe alone,
   0.276 exit-tinted against 0.0000 per strand on every rendering path. The neighbours' forwarded
   light and back-scatter scale with the strand's tint, because the neighbours share its pigment.
   A coat whose regions differ only in shade (the golden) is fine on BaseColor.

5. **Bake a UV map so every island owns the texels its bilinear taps read.** Eroding the bake's
   edge texels two deep and filling the gutters from what was left emptied every island narrower
   than five texels — the slivers material contours cut beside the eyes, lips and ears — and the
   fill gave them the colour of whatever island lay nearest in the atlas. Measured on the golden's
   shipped map, 3.2% of face corners read more than 10/255 off their vertex colour (worst 71).
   `raster_uv` writes each triangle's own value into every texel within 1.5 texels of it (islands
   lie 8 apart), and the bake logs the read-back error at every corner.

## How to verify a colour change

- Render a breed with contrasting regions, not only the golden.
- A/B the map: uniform black (or one flat colour) against the authored one. What changes is the map's.
- Separate the lobes (`OLO_DOG_FIBRE=<debug mode>`: 1 R, 2 TT, 3 TRT, 6 multiple scattering) and
  measure a patch in linear values; the lobe that carries the error names the mechanism.
- Read the bake's own log line: `the map at every corner: 99th percentile …`.

## Related

[groom-dual-scattering.md](groom-dual-scattering.md) (the coat tint never colours R),
[coat-authoring-and-per-role-budgets.md](coat-authoring-and-per-role-budgets.md) (key coat decisions
on the root UV), [docs/guides/dog-breeds.md](../guides/dog-breeds.md).
