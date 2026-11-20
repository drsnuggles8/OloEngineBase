# Ridgeback harbour crane

Two independent static meshes form the loading berth on Ridgeback's southern shore in
`Scenes/Drift.olo`. The crane pivots about the centre of its bedplate. Its local +X
axis points down the jib. The landing's origin is at deck height under the bedplate.
Both glTFs use metres and +Y up. In Drift both roots are at **(−106, 1.48, 158.5)**,
with a 90° yaw and unit scale. Place the crane alone at identity on level ground to
use it elsewhere.

## Machine and site

The 1854 hand crane at Dudbridge Wharf supplies the mechanical reference. Historic
England records a 7.3 m wooden jib at 47°, two iron stays, a revolving pillar, a
geared winch and chain tackle. Ridgeback is an original smaller machine. The 5.47 m
oak timber rises at 44° and a wire rope runs in grooved sheaves. At each end of
their travel the paired stays have a forged eye on a transverse pin. The jib tip
stops behind the wheel and the two cheek plates carry it beyond the end grain.
[Historic England, list entry 1392897](https://historicengland.org.uk/listing/the-list/list-entry/1392897)

The crank centre is 1.22 m above the deck. A 16-tooth pinion drives the 48-tooth
barrel wheel. Both use 20° involute flanks at a 54.49 mm circular pitch. The
chamfered teeth carry polished contact strips on opposite flanks. A sweep of 193
paired positions through one tooth pitch found zero outline crossings. The
opposite side carries the ratchet and a lever-operated band brake.
One line winds ten times around the drum, leaves it on a tangent, leads over the
pillar and head sheaves, and descends into the hook socket. Its 4.856 m outreach
places the hook 2.77 m beyond the outermost fender bolt. The lowest part of the hook
hangs 1.57 m above still water, with about 13.2 m of wire wound on the drum.
The existing boat's 2.60 m beam determined the outreach and the length of the
fendered face.

The shoreward deck edge crosses ground at 1.40–1.41 m at three Jolt ray samples.
The same live survey measured a bed elevation of −3.19 m beneath the hook and
−3.52 m on the alongside track at z = 152.70. The drawn hull has 0.65 m of draft.
That ground is a site in Ridgeback's recipe (`ridgeback_sites` in
`tools/drift-islands/olo_islands/recipes.py`), which restates the survey the
landing was first built against (1.31–1.49 m, −3.21 m, −3.52 m): a hard standing
behind the deck, a rock face under it and a ledge for the berth. After changing
Ridgeback's coast, run the survey again and update these three numbers.
The timber structure comprises individually charted deck boards, longitudinal
joists, transverse caps, driven piles, bracing, fenders and two mooring bollards.

Approach from the west along the fenders and shorten sail before bringing the
bow under the static hook. At a recorded boat position of (−108.66, 0.06, 154.10),
the hook hung 0.86 m above the foredeck. The hull kept at least 0.94 m of clearance
from the outer fender plane over 20 alongside samples. The exported hull, mast,
yard and furled sail cleared both crane and landing at the loading position.

## Import and rebuild

Keep both `.gltf` descriptors, their same-name `.bin` buffers and the PNGs in
one directory. For a single standalone asset take its descriptor, buffer and the
images listed under `images` in that descriptor. In the editor import the glTF
as a MeshSource and assign it to a MeshComponent. The two authored Drift entities
already reference registered mesh-source handles. Their textures are registered
individually for asset-pack loading.

From the repository root, with Blender 5.3.0 (build `02f0899d78e3`) on PATH:

```powershell
$model = 'OloEditor/SandboxProject/Assets/Models/DriftHarbourCrane'
blender -b --factory-startup --offline-mode --python-exit-code 1 --python "$model/build_crane.py" -- "$model"
```

Give a fresh output directory as the last argument for a clean build. The generator
reads the six scan inputs from the adjacent archive. Blender's bundled Python and
NumPy suffice offline. The generator builds in a temporary directory, checks both
descriptors' dependencies, replaces changed payloads, then publishes the descriptors.
An ordinary write error restores the previous package. After regeneration, reimport
the changed textures and both MeshSources in the editor, then reopen Drift. Import
`HarbourCrane.gltf` alone in an empty scene to inspect its mounting pivot.

If a process stops during publication, first check that its Blender process has
exited. Keep a copy of the destination's `.harbour-publish` directory. For every
name in `transaction.plan`, restore its `.previous` file if present, or remove the
newly added destination file if it has no backup. A missing plan means that staging
or cleanup was interrupted and the destination is consistent. Move the recovery
directory outside the project and rerun the build.

## Delivery budget

| Asset | Local bounds, min → max (m) | Triangles | Material primitives |
| --- | --- | ---: | ---: |
| Crane | (−1.719, 0, −0.646) → (5.000, 5.077, 0.786) | 56,600 | 9 |
| Landing | (−3.060, −8.650, −4.430) → (2.083, 0.410, 4.430) | 10,352 | 5 |

The 23 runtime files occupy 25.10 MiB. The scan archive adds 4.47 MiB to the
authoring package. The 19 distinct square textures comprise three at 2048, three
at 1024, ten at 512 and three at 256 pixels per side. They represent 70.75 MiB at
RGBA8 native resolution, or approximately 94.33 MiB with full mip chains before
GPU compression. The two meshes share metal and timber maps. Geometry retains the
tooth profiles, open gear web, rope turns, hook throat and pin fasteners in silhouette. For comparison,
the Drift boat hull has 30,860 triangles and Stacks Light has 82,975. The large wood
maps serve both the five-metre jib and the entire landing. A separate end-grain map
turns the annual rings across the cut ends. The rope's 256² maps repeat around six
strands and along a 150 mm lay.

## Materials and credits

The machine, landing, foundry lettering, crosscut oak and wire textures were
authored for Drift. Rust follows exposed patches in the paint and runs below cheek
bolts. Contact surfaces use separate finishes at the crank grip, brake track and
sheave grooves. The wet pile section changes at the splash line. The scan sources
shipped in `SurfaceSources.zip` are CC0. [Poly Haven asset licence](https://polyhaven.com/license)

| Source | Credit | Use |
| --- | --- | --- |
| Weathered Brown Planks | Dimitrios Savva, photography. Rico Cilliers, processing. | Directional board faces and tide-darkened oak. [Source](https://polyhaven.com/a/weathered_brown_planks) |
| Rusty Metal 03 | Amal Kumar | Enamel loss, pitting and tool-metal relief. [Source](https://polyhaven.com/a/rusty_metal_03) |

## Reproducing the review views

Open Drift, set the time of day to 12.5 hours, and set the viewport to 1600 × 900.
The editor camera accepts these world-space eye and target positions. FOV is vertical.
For dusk use 18.85 hours, which puts the sun 3.13° above the horizon in this scene.

| View | Eye | Target | FOV |
| --- | --- | --- | ---: |
| Helm approach | (−129, 2.22, 142) | (−106, 3.65, 156.6) | 39° |
| Alongside profile | (−122, 2.20, 153.4) | (−106, 3.65, 156.9) | 35° |
| Winch | (−102.6, 3.55, 161.5) | (−106, 2.89, 159.03) | 34° |
| Reverse footing | (−115, 1.05, 148) | (−106, 1.0, 157.5) | 39° |
| Sheave | (−104.3, 6.8, 152.5) | (−106, 6.23, 154.05) | 36° |

The first engine pass showed bare rod ends at the jib head, a high hook and
featureless wood. The revised view shows the eye and keeper on the sheave pin,
the lowered hook, board grain and individual fastenings. Use `olo_screenshot`
with the camera pose, `settleFrames=8` and `maxWidth=1600` to repeat the comparison.
The delivery regression runs under `--gtest_filter=DriftHarbourCraneDelivery.*`.

The review gallery includes `reproduce.py` for the three headless OpenGL paths.
It runs `BenchmarkCapture.RunWhenRequested` in a scratch project with a private
registry and cache, using the checkout's assets and the two camera poses above.
Pass it `--repo`, `--tests` (the built test executable), and `--out` (the results
directory). Each path produces `approach/Beauty.png` and `winch/Beauty.png`.
[Review gallery and capture script](https://gist.github.com/drsnuggles8/3e79c7e4f070dd3832864268bd23f8bf)
