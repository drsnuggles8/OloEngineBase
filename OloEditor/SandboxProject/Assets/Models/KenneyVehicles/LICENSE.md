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
| `craft_speederA.glb` | [Space Kit](https://kenney.nl/assets/space-kit) | Aircraft |

Only the individual models the scene uses were vendored, not the whole kits.

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
