# Groom: external colliders, morph continuity, and a moving body

Rules from making a hand part a dog's coat, and the dog walk and run without going bald (#1559).

## 1. A hand pushes to its full radius; the body proxy only to the groom's depth

**Rule:** a collider that is not the body the coat grows on — a hand, a toy — carries
`GroomColliderFlags::External` and pushes every particle out to its full radius. The fitted body
proxy keeps its own rule: a particle is held out only as far as its *target* is, because a proxy
capsule is fatter than a slim body part and the groom's own short fur lies inside it (#1533).

**Why:** the first hand was an ordinary `GroomCollider`. Pressed into the coat, it moved nothing:
every guide under it had its target inside the sphere, so the rest-depth rule let each particle stay
exactly where the groom put it. The solver test `AnExternalColliderPartsTheFurUnderItAndNowhereElse`
pins both halves: the same sphere parts the fur as an external collider and leaves it as a proxy.

Related facts:

- External colliders come from `GroomColliderComponent` (a sphere, or a capsule along local Y),
  composed in world space, nearest first. The body proxy is fitted to at most
  `GroomSimulationLimits::MaxBodyColliders` (56), so 8 slots are always free for them.
- **More than `MaxColliders` (64) used to switch collision off for the whole coat**, silently. The
  solver now solves against the first 64 and counts the rest in `GroomSimulationStats::CollidersDropped`.
- The parting is as fine as the coat's guides: strands follow their guides' displacement, cards
  follow their source guide, so both tiers part, in guide-sized patches.

## 2. A morph change rejects motion vectors, not the simulation's history

**Rule:** `SimulateGroomGuides` re-seeds on every binding reset cause *except*
`MorphSurfaceChanged`. The binding refuses history on a morph change because one morphed vertex
array cannot give last frame's root positions. That is a fact about velocities, not about where the
coat is. The targets are this frame's, and a blinking or panting face moves them continuously.

**Why:** with a plain `bindingHasHistory` bool, the solver re-seeded the whole coat to its groomed
shape on every frame a face morph moved. The Bernese and Samoyed blink and pant with morphs (the
golden uses lid bones), so their coats never swung and never parted under a hand while their faces
moved. Every guide read exactly 0.0 mm from its target in `DogPlayEvidenceTest`'s stroke, including
guides on the far side of the dog. That zero, everywhere, is the tell: a coat that is simulating
never sits exactly on its targets.

**How to apply:** when a consumer of a producer's reset verdict decides what to reset, pass the
cause, not a bool, and decide per cause which state it invalidates.

## 3. Damp, spring and rub the coat relative to the body, or it trails a moving animal

**Rule:** the guide solver damps a particle's velocity *relative to its target's* motion, measures
the groom spring from where the body has *carried* the particle this step, and applies contact
friction to the velocity *relative to the body*. The body's own travel is never resisted.

**Why:** all three read world space. Damping the world velocity is drag from air the animal carries
along: a coat moving at v settled `Damping * v / Stiffness` behind its groom, 0.2 m at a 3 m/s run.
The spring measured from last step's position read the target's own move, v·dt, as an offset:
another 5 cm at a run. Friction on the world velocity held fur lying on a capsule still in space
while the body moved on under it. The golden's crown, neck and hind legs went bald from behind in
Run (68 % of the visible face skin bare), Carry and Trot, in proportion to each clip's speed. A
strand straight along the motion hides the spring term, because the length projection absorbs it.
Only a strand hanging across the motion shows it. `ACoatCarriedAtASteadySpeedKeepsItsDrape` (31 mm of
trail with only the damping fixed, 0.03 mm with both) and `FurLyingOnAMovingBodyCapsuleMovesWithIt`
(1.9-2.5 mm with world friction, 0.001 mm relative) pin them. That test's fur drapes *down the side*
of its limb: fur balanced on top of a capsule is a metastable rest any motion tips off, which reads
as a 12 mm "lag" and is not one.

**How to apply:** any per-particle force, damping or friction in a solver whose particles live in
world space must ask whether it should see the body's motion. A spring toward an animated target, a
settling damper and a rub against the body should not.

## 4. A short hair holds its groom; a stiffer spring cannot do it

**Rule:** a guide shorter than `GroomSimulationLimits::ShortHairReferenceLength` (10 cm) is pulled
back toward its groom every step by `1 - (length / 10 cm)^2` of its offset, after the prediction and
before the colliders. Longer guides keep their authored stiffness, and so does every guide of a
`GroomCoatRole::LongHair` group however short (`GroomSimulationInputs::LongHairGuides`).

**Why:** a hair is a cantilever: its natural frequency goes as 1/L², so short fur barely moves while
ears and plumes swing. With one stiffness for the coat, a gallop's 7 m/s² swung 1-2 cm body fur off
the skin by centimetres. Raising the spring for short guides to the step's stability ceiling
(`MaxStiffnessTimesStepSquared`) was tried first and failed: at the ceiling the explicit spring
rings with almost no damping, and a 1.5 cm hair still moved 3.5 mm. The positional hold is stable at
any weight: 0.16 mm (`AShortHairRidesABouncingBodyWhileALongOneSwings`, with a 15 cm control that
still swings 16 mm). A hand still parts short fur, because the colliders act after the hold. The
reference was 5 cm first: that left a dog's 5-8 cm neck guard hair free, and once friction no
longer pinned it in world space (rule 3) a gallop flipped it, 64 % of the neck bare from behind.
At 10 cm it reads 15 %. But 10 cm also reached the long-hair groups shorter than it -- the golden's
ear fringe (10 cm), leg feathering (7.5 cm), tail plume -- and their walk swing fell to a fifth
(the Bernese's long coat 10.7 mm to 2.0 mm), under `kLongHairSwingFloor`. A role authored to swing
is exempt (`ALongHairGuideIsNotHeldHoweverShortItIs`); the neck's guard hair stays held.

## 5. The body proxy lifts the coat; it does not sweep it

**Rule:** a body (non-external) collider pushes a particle along its *target's* normal off the
proxy, back up to the height the groom put it at. Only a particle that has slid round beside the
proxy, high enough along that normal yet still inside, is pushed out radially. The lift is
positional: it moves `Prev` with `Curr`, so it adds no velocity.

**Why:** a fitted capsule rides its bone while the skin over it blends between bones, so a swinging
limb's capsule slides under the coat. Pushed out radially from wherever the lagging particle sat,
the coat was wiped aside like a hand had parted it: pale patches of undercoat on a walking dog's
shoulders, on master since the simulation shipped. A lift that also left its displacement in the
velocity became a bounce that the friction step kept as "separating". The A/B that found it:
simulation off and capsules at 0.6 radius both cleared the patch, a pose-baked coat shadow did not.
`ABodyCapsuleSlidingUnderTheCoatLiftsItAndDoesNotWipeIt` pins it (body 0.00 mm, a hand doing the
same sweep 1.54 mm). The cost is a little rest tremble where two capsules cross at right angles
(0.013 mm), which `StrandsAcrossTwoCollidersComeToRestAtAnyFriction` now bounds at 0.02 mm.
