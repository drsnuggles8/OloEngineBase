# The keeper of Stacks Light

The keeper stands on the top granite tread of Stacks Light in `Scenes/Drift.olo`.
The source models use metres, +Y up and +Z forward. The origin is on the ground
between the boots. The uncapped height is 1.750 m. Chin to crown is 0.232 m,
or 7.54 heads per standing figure. The eyes are 1.634 m above the soles, halfway
between chin and crown. The eye centres are 64 mm apart.

The [character guide](../../../../../docs/guides/drift-keeper.md) shows the
historical references, modelling passes, expression studies and scan comparison.

## Asset contract

| File | What it supplies |
| --- | --- |
| `Keeper.gltf`, `KeeperLOD1.gltf`, `KeeperLOD2.gltf`, and their `.bin` buffers | Three skinned bodies with the same skeleton, expression order, materials and UV charts. Each tier carries the garments, hands, lantern and mouth. |
| `Keeper*.olomesh` | The scene handles for the body tiers. Their upper distance bounds are 18, 78 and 1,000 m. The last tier continues beyond its bound. |
| `KeeperEyeball.gltf`, `.bin`, `KeeperIrisColor.png` | A unit globe and planar iris chart shared by both eye entities. Each globe scales to a 12.6 mm radius and turns about its own centre. The eye profile supplies the cornea and iris optics. |
| `KeeperHairFoundation.gltf` and `.bin` | Pigmented skin beneath the beard, moustache, brows, lashes and exposed scalp. This head attachment has one topology across body tiers and facial targets for its moving regions. It is the surface the groom binds to. |
| `Keeper.abc` | Source curves, organised by anatomical region and pigment. The game reads the cooked groom and its matching binding in `Assets/Grooms/DriftKeeper/`. |
| `Keeper*.oloskin` | The six profiles assign skin, eye, lip, gum, tongue and enamel distinct responses. Face, ears and hands use the skin profile. |
| `Keeper.rig.json` | Local transform relative to the tower, source and count for each LOD, eye joints and positions, clip and morph names, material/profile assignments and groom source. |

`build_keeper.py` writes glTF directly. The skull uses interpolated sections.
The face removes patches for the two eye sockets and the mouth, then builds the
lids and lips around their openings. The dental arches, gums and tongue sit
behind the lips. Moving the lower tissue with the open-mouth shapes exposes the
interior. The expression order is `BlinkL`, `BlinkR`, `JawOpen`, `Smile`,
`BrowRaise`, `Squint`, `AA`, `EE`, `OO`. The engine exposes them to Lua as
`MorphTarget_0` through `MorphTarget_8` in that order.

The shoulders, sleeves, palms, individual fingers, boots and layers of the
coat are built from sections and swept curves. The cap peak, shoulder cape,
cuffs, welted pockets and brass buttons have geometry. The material generator
bakes periodic weave, grain, variations in pigment and surface normals onto the
same charts for all three tiers. The hair generator grows seven-point curves
on the foundation with separate length, direction, width and curl per region.

The skeleton branches at the pelvis, chest and head. Each arm has an upper
arm, forearm and hand. Each leg has a thigh, shin and boot. The eyes follow
their own joints, and the lamp follows the left hand. `Idle` shifts the chest
and head over a five-second cycle. `Wave` takes 3.6 seconds. `LanternRaise`
takes four seconds. The Lua controller supplies blinking, greeting and gaze.

## Regenerating

Run from the repository root with Blender 5.3 and an Alembic-enabled engine
test build. In PowerShell, substitute the path to your Blender executable if
it is installed elsewhere.

```powershell
$models = (Resolve-Path 'OloEditor/SandboxProject/Assets/Models/DriftKeeper').Path
$blender = 'C:/Program Files (x86)/Steam/steamapps/common/Blender/blender.exe'
& $blender -b --factory-startup --python-exit-code 1 --python "$models/build_keeper.py" -- $models
& $blender -b --factory-startup --python-exit-code 1 --python "$models/build_keeper_groom.py" -- $models
& $blender -b --factory-startup --python-exit-code 1 --python "$models/preview_keeper.py" -- $models build-cached/keeper-preview --study --groom
```

The generator stages files outside the watched project, then publishes the buffers
and textures before their descriptors. If an interrupted publication leaves a
`.stacks-light-recovery` directory, follow the recovery instructions in the
[lighthouse README](../DriftLighthouse/README.md).

The body generator also runs with `python "$models/build_keeper.py" $models`
in a Python environment with NumPy. The groom and previews use Blender.
Omit `--groom` for faster Workbench shape studies. The `--study` option adds
clip poses, expressions, LOD masks and the licensed reference bust. The scan
is loaded only for the reference frame.

Cook the resulting archive against the freshly generated foundation. This
writes two files into a fresh scratch directory. Run the test from the
repository root, then copy both files together after it passes.

```powershell
$tests = (Resolve-Path 'build-cached/OloEngine/tests/Debug/OloEngine-Tests.exe').Path
$cook = Join-Path (Resolve-Path build-cached) ("keeper-cook-" + [guid]::NewGuid())
$grooms = 'OloEditor/SandboxProject/Assets/Grooms/DriftKeeper'
$env:OLO_KEEPER_COOK_DIR = $cook
try {
    & $tests --gtest_filter=DriftKeeperAuthoring.CooksTheGroomAndBinding
    if ($LASTEXITCODE -ne 0) { throw "Keeper cook failed ($LASTEXITCODE)" }
    New-Item -ItemType Directory -Force -Path $grooms | Out-Null
    foreach ($file in 'Keeper.ologroom','Keeper.ologroombinding') {
        Copy-Item -LiteralPath (Join-Path $cook $file) -Destination $grooms
    }
} finally {
    Remove-Item Env:OLO_KEEPER_COOK_DIR
}
& $tests '--gtest_filter=DriftKeeperDeliveryTest.*:DriftKeeperEvidenceTest.*'
& $tests --olo-rhi=vulkan '--gtest_filter=DriftKeeperEvidenceTest.*'
```

After a change to the foundation topology, recook both files. A binding
addresses imported triangles by index. Keeping the previous binding can leave
the hair behind as the keeper turns his head. Reimport the edited glTFs
before inspecting an open editor session. When changing the tier distances,
triangle counts, placement, profile names or attachments, update the keeper
entities in Drift from the new rig contract. Test a packaged run with the
source model folder temporarily renamed, then restore that folder.

## Licence

The keeper's geometry, rig, motion, groom and material maps were generated in
this repository and use the [repository licence](../../../../../LICENSE).
The photographs and the separate scan image shown in the guide have their own
credits there.
