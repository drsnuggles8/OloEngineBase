# Author the head's hair from the rigged scan

`build_human_hair.py` authors a centre-parted, jaw-length dark-brown bob for
`assets/models/InfiniteScanHead/HeadRigged.gltf` (#1593). The scan, skin texture,
rig, LookAround animation and measured thickness map are read only.

The visual reference is the Alberta Ferretti SS21 runway photograph in
[Marie Claire's centre-part gallery](https://www.marieclaire.it/bellezza/capelli/g34409654/tagli-capelli-riga-in-mezzo/)
([photograph](https://hips.hearstapps.com/hmg-prod/images/riga-centrale-capelli-ss-2021-alberta-ferretti-1603091441.jpg),
consulted 2026-10-11). It provides a narrow scalp part, swept forehead locks,
continuous scalp-to-length flow, a dark body with localized uncoloured highlights,
and separated tips above the shoulders. The photograph is a reference only;
it is not redistributed, traced or used as a texture. The groom is authored here.

## Reproduce and cook

Use Python 3.13 with Blender's `bpy==5.2.2`, `numpy==2.5.4` and `scipy==1.18.1`
in an isolated environment. The `bpy` wheel includes Alembic and runs headlessly.
From the repository root:

```powershell
python OloEditor/SandboxProject/Assets/Grooms/HumanSource/build_human_hair.py `
  --out <scratch>/Human.abc --blend <scratch>/Human.blend
$env:OLO_HUMAN_HAIR_ABC = '<absolute scratch>/Human.abc'
$env:OLO_HUMAN_HAIR_EXPORT = '<absolute scratch>/cooked'
build-cached-core/OloEngine/tests/Release/OloEngine-Tests.exe `
  --gtest_filter=GroomAnimalsAcceptanceEvidenceTest.ExportsHumanHairPair
```

Build through `.claude/skills/run-oloengine/build-lock.ps1` before cooking.
The engine's Alembic importer, groom cooker and binding builder produce the pair.
The cook exports only `Human.ologroom` and `Human.ologroombinding` to scratch;
install both in `Assets/Grooms/Animals` after inspection. Existing registry handles
are preserved. Regenerate Acceptance with `scripts/perf/generate-acceptance-scene.py`
after updating HumanCoat's settings in GroomAnimals. Set its render budget from
the recipe's strand count and disable extra length jitter: variation and locks
are already authored into the source curves.

Alembic and `.blend` are generated authoring outputs, kept outside the project
asset directory. The script and the cooked pair are the shipping sources.
`Human.recipe.json` records the geometry digest and counts; a count is an identity
check, never a visual-quality verdict. Repeated authoring produces the same curve
arrays; Alembic container metadata is not used as a geometry determinism oracle.

## Coordinates and contracts

Roots are sampled on the scan's triangles with their interpolated glTF root UVs.
There is a curved frontal hairline, clearance above the ears, and a narrow part.
Small hairline irregularities and a 3 mm density taper soften the forehead edge.
The part's roots have 2.5 times the base density. Short regrowth from those
sampled follicles combs backwards beneath the long locks, keeping the part
covered through motion without turning the two sides into separate sheets.
Regrowth uses the existing undercoat role and tapered 60–85 micrometre fibres;
the scene's guide caps remain 512 long-hair and 128 undercoat guides.
Voronoi locks are separated at the part; each lock follows the scalp before a
tangent-continuous hanging section. Arc-length sampling concentrates controls
near the scalp to follow its curvature. The curves
carry tapered 65–95 micrometre root diameters. Guides and card-cook settings ride
on Blender's hair-curve attributes and Alembic user properties. Locks gather
less at their tips and individual fibres shorten along the tip tangent by up
to 12 mm. Short emergence hairs soften the front boundary above the eyes.
The ellipsoid is only a styling scaffold: non-root controls that enter the scan
are projected onto its triangles with 6 mm crown volume and 1 mm lower clearance.
The extra crown volume gives the dynamic guides room to sag above the scanned
surface; flush arcs sank under the scalp despite correct root binding.
The sampled roots stay
on the actual bind surface. The recipe records the number and largest correction.

HumanCoat uses the existing guide solver with stiffness 2000, damping 12 and
gravity `[0, -4, 0]`, within its supported range. This is the same reduced-gravity
authoring convention used by the short-coat showcase, with full simulation and
collision enabled. The earlier stiffness 90 treated the scalp arcs like rope,
pulling them into the head and across the eyes during runtime even though the
static editor groom and binding checks looked correct.

The historical Human groom frame contains GroomAnimals' head placement
`[0, 1.24, 3.6]`. Blender uses Z up and faces -Y; its Alembic export converts
back to the engine's Y-up, +Z-facing frame. The binding is cooked with the body's
placement in that same frame. Changing it requires re-cooking both files.

`GroomAnimalsCookedBinding.HumanHairFramesTheFaceAndClearsTheShoulders` pins the
rest geometry's eye/cheek corridor and shoulder clearance. Cold/warm binding,
pack parity, runtime motion and inspected multi-angle frames remain separate
requirements; the rest-geometry contract cannot establish those.
`SkinRiggedHeadCloseUp.NativeHairAuthoringEvidenceOnEveryPath` captures 1080p
views, physical fibre lobes, a hair-off control and the full 7.24-second LookAround
clip through the real OpenGL Scene pipeline. `NativePackedHairKeepsItsCookedPairAndBinding`
checks the shipping pair through AssetPackBuilder and RuntimeAssetManager.
The full generated image matrix is ignored; curate review images separately.

For a cold standalone game check, set `OLO_HUMAN_HAIR_GAME` to an absolute scratch
directory and run `SkinRiggedHeadCloseUp.ExportsHumanHairGame`. It calls the real
GameBuildPipeline with this pair, the actual skin profile and staged model dependencies.
Launch `HumanHair1593/HumanHair1593.exe --rhi=opengl` or `--rhi=vulkan` with its game
directory as the working directory. The native runtime client was inspected on both
backends; see [the evidence report](../../../../../docs/analysis/digital-human-hair-quality-1593.md).

The scan-derived hair roots retain the scan's **CC BY 3.0** attribution chain:
Lee Perry-Smith / Infinite, Morgan McGuire and Guedis Cardenas, and Keijiro
Takahashi. See [the scan's licence](../../../../assets/models/InfiniteScanHead/LICENSE.md)
and [attribution](../../../../assets/models/InfiniteScanHead/README.md).
