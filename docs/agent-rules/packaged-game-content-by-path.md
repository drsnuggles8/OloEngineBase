# A packaged game carries the pack; everything a scene opens by path must be staged

**Rule.** The asset pack holds only what the registry serialises. Anything a scene opens **by
path**, through `ResolveContentPath`, `Texture2D::Create(path)`, `AnimatedModel(path)` or an
asset-relative field, ships only if `GameBuildPipeline` copies it to the same relative path in
the game directory. That copy is `StageSceneReferencedContent`, run from
`GameBuildPipeline::StageRuntimeContent`. When you add a component field that names a file:

1. Store it in `ResolveContentPath`'s spelling: `Assets/...` for project content, `assets/...`
   for engine content. Write it with `MakePortableSceneResourcePath`. A path relative to the
   asset directory that climbs out (`../../assets/...`) resolves outside a packaged game's
   directory, and no staging can fix that.
2. If the file opens other files itself (an `.obj`'s `mtllib`, an `.mtl`'s maps, a `.gltf`'s
   URIs), make sure `ReadContentDependencies` follows them.
3. Prove it on the cooked side. Lay the game out with `StageRuntimeContent`, mount it the way
   `OloRuntime` does, and compare against loose. The fixture to copy is
   `FloraLooseCookedParityEvidenceTest`: a `RuntimeAssetManager` with **no** pack (stricter than
   the real one), the game directory as the working directory, and a planted-downgrade test for
   every assertion.

## Why: every fallback renders something plausible (#1392)

Before #1392 the pipeline shipped the pack, the scenes and every loose PNG, but no `.obj` or
`.mtl`. In a shipped game:

- every authored foliage mesh failed to resolve, so the layer drew its flat card at all distances;
- the impostor is baked from that mesh, so no impostor was baked either, and the pines vanished
  at distance;
- a mesh part whose `.mtl` texture was missing drew the **card's** albedo.

Nothing crashed. Each failure was one log line. A census on the draw stream alone would not have
caught the `.mtl` case: the mesh still drew, with the wrong texture. The check that sees it is the
renderer's own record of what each surface sampled (`FoliageRenderer::GetAlphaCoverage`), compared
file by file.

The same class turned up in three more places while packaging the sandbox:

- `AnimationStateComponent::SourceFilePath` was asset-relative, so 91 animated entities pointed
  outside the game directory (#1539).
- `AssetPackBuilder` counted a failed load as loaded, because the asset manager returns a
  placeholder rather than null. "Loaded 188/188 (0 failed)" meant three assets were not in the
  pack.
- `ScriptFileSerializer` parsed every `.cs` as YAML, failed, and shipped no script assets. Its
  `Serialize` would have written YAML over the C# source.

## A pixel A/B control needs identical camera history

**Rule.** When two scene loads are compared pixel for pixel, every arm must see the same
sequence of camera poses and foliage on/off states, frame for frame. The flora's LOD hysteresis
remembers the previous frame's distance per instance, so an arm with a different history draws a
different frame.

The first parity run took the foliage-off baseline on the loose arm itself. That arm then sat
6–8% of the frame away from both the loose control and the cooked arm, which agreed with each
other to the pixel. A discarded warm-up capture did not help, because the cause was the history,
not the capture order. The tell is `loose-vs-control == loose-vs-cooked` to four digits: the arm
you are comparing against is the odd one out. The fix is a separate arm for the baseline. With
that, loose-vs-loose was at most 0.0004% and the 0.1% allowance meant something.
