# Foliage interaction bending

Put a `FoliageInteractionComponent` on any entity that should press foliage
aside — a character, an animal, a rolling boulder. Its entity transform is the
influence's world position. Nothing else has to be kept in sync.

**The component is the feature's switch.** While no entity in the scene carries
one, the influence set is empty and the foliage shaders contribute exactly
`0.0` — not a small number — so every scene, save game and golden image
authored before this feature renders bit-identically. `FoliageLayer::InteractionResponse`
therefore defaults to **1**, not to 0 as the other per-layer opt-ins do: a
response of 1 multiplying an empty set is still nothing, and a scene that gains
an actor gets bending everywhere it should rather than only where someone
remembered to raise a slider. Set a layer's response to 0 to make that species
ignore actors.

## What an author sets, and what each number means

| Component field | Meaning |
|---|---|
| `m_Radius` | horizontal reach, world units. The bend falls to nothing at exactly this distance. |
| `m_Height` | how far **above** the entity a plant's root may sit and still bend. The influence is a vertical cylinder, not a sphere, because an actor's origin is at its feet and a blade's root is on the ground. |
| `m_Strength` | peak displacement in world units, before the layer's response. Capped at `kFoliageInteractionMaxStrength` (1.0). A bigger bend is asked for on the **layer**, via `InteractionResponse` — see *Bounds* for why. |
| `m_Falloff` | radial exponent; 1 is linear, higher concentrates the bend under the actor. |
| `m_RecoverySeconds` | **seconds** to stand back up. A time constant, never a per-frame rate. |
| `m_TrailSpacing` | distance travelled before a new influence is planted and the old one is left behind to recover. 0 = no trail. |

`FoliageLayer::InteractionResponse` scales the whole influence set for one
species, and is also what the instance bounds are padded by — see *Bounds*.

## Recovery is an analytic critically damped spring, and that is the whole point

`FoliageSpringStep` integrates `x'' = -2w x' - w^2 (x - T)` in closed form, so
one step of `dt` is exact for any `dt`:

```
e = exp(-w dt)
b = v + w (x - T)
x' = T + ((x - T) + b dt) e
v' = (v - b w dt) e
```

Three properties follow, and each is a failure this replaces:

- **Frame-rate independence.** `x += (T - x) * k` converges at a rate that
  depends on how often it is called, so the same motion recovers at a different
  speed at 30 fps and at 144. The closed form composes: N steps of `dt` equal
  one step of `N*dt`. `FoliageInteractionContract.FrameRateIndependenceHolds…`
  pins that at four cadences plus a spike, and it is the test that fails if
  anyone ever puts a lerp back.
- **No oscillation.** The critically damped root is real and repeated, so there
  is nothing to ring with; and `(1 + u) e^-u <= 1` means a release from rest
  never overshoots. A large `k` with one long frame does both.
- **A spike does not teleport the bend.** `dt` is clamped to
  `kFoliageInteractionMaxStep` first. The spring would handle a four-second step
  correctly; "correctly" would still be a visible cut.

Attack is `kFoliageInteractionAttackRatio` times the authored recovery, chosen
per step from whether the bend is growing or relaxing, so `m_RecoverySeconds`
always names the recovery and never the attack.

## The state is per INFLUENCE, not per plant

A per-plant bend buffer would be `O(instances)` of GPU state every pass has to
agree on. Here the state is a handful of floats per influence, capped at
`kFoliageInteractionSlots`, and the per-plant bend is a pure function of that
snapshot evaluated in the vertex stage.

The cost of that choice is that recovery is a property of the influence rather
than of the blade — so an influence that simply followed a running actor would
let the grass snap upright the instant the actor passed. **Trail spacing is the
answer**: with `m_TrailSpacing > 0` the influence is *planted* and does not
follow. When the actor gets a spacing away, the planted slot is orphaned from
its source, keeps the bend it had, relaxes to zero on its own clock and is then
retired; a fresh one is planted underfoot. A trail is a sequence of independent
stationary footprints, with no extra buffer.

## Where it composes, and why it cannot diverge

`foliageDeform` in `include/FoliageWind.glsl` is the single deformation
producer. Interaction is added **inside** it, for both the current and the
previous frame, so colour, G-Buffer, depth, shadow and velocity all receive one
displacement. The normal transport (`foliageWindNormal`) evaluates the
interaction term in its Jacobian too, and its early-out was widened: a layer
that never opted into hierarchical wind still transports its normal while an
influence is bending it, because shading a flattened blade with its upright
normal is the same defect the cofactor transport exists to prevent.

`ApplyFoliageInteraction` is the only writer of the UBO lanes and all three
raster fill sites call it — the colour draw, the shadow draw and the command
dispatch. The set is read there, from `FoliageInteractionField::GetGPUData()`,
rather than carried on a draw command, so a command recorded early in a frame
cannot bend one pass against last frame's influences.

**Velocity carries a previous snapshot per slot**, not just a current one. A
bend evaluated only at the current frame reprojects to the plant's rest
position, and the difference smears through the temporal history exactly where
the actor is. A slot with no history reports its current state as its previous
one, which makes its velocity exactly zero for one frame — the same shape, and
the same reason, as `FoliageUBO::WindHistoryValid`.

**Ray-traced vegetation (#1240) gets the same lanes**, on the same refresh
cadence its wind phase already uses: the surface cache's key deliberately
excludes time, so interaction reaches RT as a periodic snapshot, not per frame.
`FoliageInteractionField::GetMaximumBendRate()` is added to that cache's
velocity bound so a snapshot taken before an actor arrived is not served while
the actor is running through it.

## Bounds

The shader clamps the **summed** push to `u_InteractionParams.z`, which is
`FoliageInteractionMaximumDisplacement(response)`. That is what makes the bound
hold for any influence count: no number of overlapping actors can move a plant
further. `FoliageBoundsProfile::m_InteractionDisplacement` pads every instance
AABB by exactly that, and it adds to the wind term because a gust and a foot can
displace the same plant at once.

That padding is paid by every foliage layer in every scene, whether or not
anything in it emits an influence, which is why the strength cap is 1.0 and not
larger: a typical grass layer's wind padding is about 0.34 world units, so the
default already costs roughly three times the wind term in permanently looser
culling. A layer that wants a bigger bend raises its own `InteractionResponse`,
which widens the bound exactly where the bend was asked for.

The padding is derived from the **authored** response, never from the live
field. The profile is hashed into each instance's identity (`FoliageInstanceRegistry`),
so a bound that moved with a passing actor would retire and re-issue every id in
the layer as the actor walked by.

## No permanent artifacts

Three mechanisms, at three scales:

- a slot whose bend decays below `kFoliageInteractionRetireEpsilon` is **retired**
  — set to exactly zero and freed — so removal is a finite-time guarantee rather
  than an asymptotic one;
- a **teleport** (a jump larger than four radii) sheds the old influence where it
  was and starts the replacement with no history, so the grass the actor was
  standing in recovers in place and nothing streaks across the jump;
- `FoliageInteractionField::Reset()` runs at every scene-lifecycle boundary
  (`OnRuntimeStart`/`Stop`, `OnSimulationStart`/`Stop`). The centres are
  absolute world coordinates and residuals outlive the actor that shed them, so
  a scene switch or region unload would otherwise leave a bend pressed into new
  content with nothing standing there to explain it.

Every one of these is measured rather than argued:
`FoliageInteractionContract` for the field, and
`FoliageInteractionEvidenceTest.EveryRasterPathBendsUnderAnActorAndRecoversAfterIt`
for the pixels — which asserts the residual RMSE against the control is a small
*fraction of the bend it came from*, not merely "small".

## Not covered

No script binding (C#/Lua) and no physics-body source: an influence comes from a
component's transform or from a direct `FoliageInteractionField::Update` call.
The bend is horizontal only, exactly like `foliageWindOffset` — adding an
arc-shortening vertical drop to one producer and not the other would make the
two disagree about where the same plant's tip is.
