# Kenney vehicle models — CC0 1.0 Universal (public domain)

The `.glb` files in this directory, and the colormap textures under `Textures/`,
are by **Kenney** (<https://kenney.nl>) and are
released under [**Creative Commons CC0 1.0 Universal**](https://creativecommons.org/publicdomain/zero/1.0/)
— public domain. No attribution is required; this file is here as provenance, not
as an obligation.

Used by `Assets/Scenes/VehiclesTest.olo` (issue #438) so the vehicle demo shows
real models rather than boxes. All six come from the same author and share one
low-poly art style, which is why they were chosen over higher-fidelity models
from mixed sources.

| File | Source pack | Used as |
| --- | --- | --- |
| `sedan-sports.glb` | [Car Kit](https://kenney.nl/assets/car-kit) | Car — rear-wheel drive |
| `suv.glb` | [Car Kit](https://kenney.nl/assets/car-kit) | Car — front-wheel drive |
| `van.glb` | [Car Kit](https://kenney.nl/assets/car-kit) | Car — all-wheel drive |
| `boat-row-large.glb` | [Pirate Kit](https://kenney.nl/assets/pirate-kit) | Boat — runs straight |
| `ship-small.glb` | [Pirate Kit](https://kenney.nl/assets/pirate-kit) | Boat — circles under rudder |
| `ship-small-hull.glb` | derived from `ship-small.glb` | Drift's boat — hull + flags |
| `ship-small-sail.glb` | derived from `ship-small.glb` | Drift's boat — the rig, on its own entity |
| `craft_speederA.glb` | [Space Kit](https://kenney.nl/assets/space-kit) | Aircraft |

Only the individual models the scene uses were vendored, not the whole kits.

## The two derived files, and how to remake them (issue #899)

The two commands that produce them, from the repository root — they rewrite the
committed files byte-for-byte, so a regeneration that differs means the tool or
the source moved:

```bash
python scripts/split-glb-nodes.py \
    OloEditor/SandboxProject/Assets/Models/KenneyVehicles/ship-small.glb \
    --keep ship-small --drop sail-a --scene-name ship-small-hull \
    --out OloEditor/SandboxProject/Assets/Models/KenneyVehicles/ship-small-hull.glb

python scripts/split-glb-nodes.py \
    OloEditor/SandboxProject/Assets/Models/KenneyVehicles/ship-small.glb \
    --keep sail-a --re-origin --scene-name ship-small-sail \
    --out OloEditor/SandboxProject/Assets/Models/KenneyVehicles/ship-small-sail.glb
```

`ship-small-hull.glb` and `ship-small-sail.glb` are a **subset split** of
`ship-small.glb`, not new art. CC0 permits it without condition; this section is
here so the pair can be regenerated rather than reverse-engineered.

The reason for the split is that Drift's sail has to move. `Model` flattens the
imported node graph and draws every submesh with the entity's one transform, so
a part of a `ModelComponent` cannot be posed independently — the rig had to
become its own entity, which meant its own file.

The source has four named nodes (`ship-small`, `flag-a`, `flag-b`, `sail-a`), so
the split is a straight subset of the same glTF:

* **hull** — the `ship-small` subtree with the `sail-a` child dropped, every
  surviving node left at its ORIGINAL model-space translation, so the existing
  scene transform frames the boat identically;
* **sail** — `sail-a` alone, promoted to the scene root **with its node
  translation removed**. That re-origin is the load-bearing part: node transforms
  are baked into the vertices by `aiProcess_PreTransformVertices`, so without it
  a Y rotation on the sail entity would swing the sail around the BOAT instead of
  around its own mast. With it, the entity's Y rotation is the yard angle, and
  `Drift.olo` places the entity at the translation that was baked out
  (`[0, 3.13852, -0.6333724]`, in the hull mesh's own space) so the split is
  invisible.

Both keep the shared external `Textures/colormap-pirate.png` reference, so the
pair costs one extra 12 KB file and no extra texture memory. Geometry, UVs and
materials are untouched; only the node list, the accessor/bufferView packing and
that one translation differ.

Both are registered in `AssetRegistry.oar` — `.glb` is a supported asset
extension, and `AssetContentValidity.EverySupportedAssetOnDiskIsInTheRegistry`
fails on any that is not.

`ship-small.glb` itself is **kept**: `VehiclesTest.olo` still uses the whole boat,
and it is the source the pair is derived from.

## Why there are two colormaps, and why the models were repointed

Every model except `craft_speederA.glb` (which carries per-material
`baseColorFactor`s and needs no texture) is a UnityGLTF export with a single
`colormap` material sampling an **external** palette atlas. As shipped by Kenney
each kit's atlas is just `Textures/colormap.png`, and each model references it by
that relative path — but the Car Kit and Pirate Kit atlases are **different
images**, and they differ at *every* UV these five models actually sample. Since
all the models live in one directory, a single `colormap.png` could only ever be
correct for one kit; the other would render in the wrong palette entirely (the
cars come out uniformly dark, or the boats lose their timber).

So each kit's atlas is vendored under its own name and the models' `images[].uri`
was rewritten to match:

| Texture | Source pack | Used by |
| --- | --- | --- |
| `Textures/colormap-car.png` | [Car Kit](https://kenney.nl/assets/car-kit) | `sedan-sports`, `suv`, `van` |
| `Textures/colormap-pirate.png` | [Pirate Kit](https://kenney.nl/assets/pirate-kit) | `boat-row-large`, `ship-small` |

Only the URI string changed — geometry, UVs and the binary chunk are untouched.
Both textures are registered in `AssetRegistry.oar`; the reverse-consistency
check in `AssetContentValidityTest` fails on any supported asset that is on disk
but unregistered.

## Why the scene parents the model to the physics body

Each vehicle in `VehiclesTest.olo` is **two** entities:

* a parent carrying the `Rigidbody3DComponent`, collider and
  `VehicleComponent`/`BoatComponent`/`AircraftComponent`, and
* a child carrying only the `ModelComponent`.

That split exists because the engine's vehicle convention is **local +Z forward**
while these models are authored facing **-Z** (the usual glTF/Blender export
orientation). A single entity cannot satisfy both: rotating it to face the model
the right way would rotate the *physics* too, so the car would drive backwards.
Putting the visual on a child lets it carry its own corrective yaw and its own
scale, leaving the parent's transform purely physical.
