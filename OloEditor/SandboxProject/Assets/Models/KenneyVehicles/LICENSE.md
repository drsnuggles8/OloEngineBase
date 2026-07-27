# Kenney vehicle models — CC0 1.0 Universal (public domain)

The `.glb` files in this directory are by **Kenney** (<https://kenney.nl>) and are
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
