# Playing with the dogs

Each furred dog has a play scene: `Scenes/DogPlay.olo` (the golden), `BernesePlay.olo` and
`SamoyedPlay.olo` (#1559). Open one, press **Play**, and play with the dog:

| input | what it does |
|---|---|
| right-drag | orbit the camera around the play spot |
| wheel | zoom |
| W A S D (Shift to hurry) | walk the play spot across the lawn, to follow the dog; the camera comes along |
| hold left on the dog, move | pet it: the fur parts under the hand, and the dog reacts; a hand held still rests on the spot and moves with the dog |
| hold left on the ball or the stick, flick, let go | throw it: the flick's speed and direction set the throw |

A flick up the screen throws away from the camera with an arc; a sideways flick throws sideways. The
dog watches the toy in flight, runs to it once it comes down, picks it up in its mouth (the ball
held, the stick carried crosswise), brings it back, drops it at your feet, and play-bows for the
next throw. While it is being petted it ignores the toys. It leans into the hand, its eyes half
close and its tail wags faster; the ears and the chest are its favourite spots and earn a stronger
reaction.

Between games the dog is never a statue waiting for input:

- **It pays attention.** Its head follows the cursor once the hand comes near, a toy in your hand or
  in flight, or you, with quick glances about the lawn; when what it watches is out of the head's
  reach it turns its body. Walk off holding a toy and it follows you, trotting, or running when far
  behind, and stops a step short.

The head's look is kept as an angle off the body's forward (`HEAD_YAW_LIMIT` 1.1 rad either side,
`HEAD_PITCH_LIMIT` 0.9 rad up or down), not as a point in the world, and swings no faster than
`HEAD_TURN_SPEED`. A world point stays where it is while the body turns away from it. The head then
craned round behind the dog after a toy held behind it, or carried a fetched toy home looking
sideways. While a clip poses the head (pick-up, drop, sniff, shake…), the look drifts back to
straight ahead, so attention resumes from there.
- **It has a mood.** Energy, excitement, a wish for company and curiosity rise and fall with what
  happens: running tires it, a toy held up excites it, petting satisfies it, being ignored makes it
  want you, standing about makes it curious.
- **It takes the initiative.** Left alone long enough it comes over and nudges your hand; excited,
  it fetches a toy lying on the lawn on its own and brings it to you; with a toy held up it bounces
  and play-bows; very excited and rested it does zoomies; curious, it trots off to sniff the lawn; it
  sits and scratches, stretches, shakes off after a long fuss, and nudges for more after a short one.
  Tired, it lies down; stroke it then and it rolls over for a belly rub.

## How it is built

The behaviour is two breed-agnostic Lua scripts. The engine pieces they use are general, not
dog-only.

| piece | where | what |
|---|---|---|
| the player | `Scripts/LuaScripts/PlayHand.lua`, on the camera | orbit, zoom, walk, pet, grab and throw; a free cursor |
| the dog | `Scripts/LuaScripts/DogPlay.lua`, on the dog | attention (head aim), mood, the initiative it picks, petting, the fetch chain |
| pick volumes | `PickVolumesComponent`, `Scene::RaycastPickVolumes` | a capsule per bone fitted to the skin, carried by the current pose; named regions |
| coat colliders | `GroomColliderComponent` | a hand or a toy pushes every simulated coat it reaches out of it, to its full radius |
| carry and release | `Scene::AttachEntity` / `DetachEntity` | a body rides a bone (kinematic, following the drawn pose) and leaves at the speed it was carried |
| rest a hand on a spot | `Scene::AttachEntityInPlace` (Lua `attach_in_place`, C# `AttachInPlace`) | attach where the entity already is, under the parent's or bone's drawn pose |
| where a child is | Lua `get_world_translation`, C# `Entity.WorldTranslation` | the composed world position; `get_translation` is parent-relative |
| steering on root motion | `NavAgentComponent::m_UseRootMotion` | the clip moves the dog; navigation only turns it toward the next corner |
| script physics | `Physics3DScriptApi` (C# and Lua) | 3D impulse, velocity, body type, contacts |

The two scripts share a global table, `PlayBus` (`petting`, `cursor`, `held`, `thrown`, `inMouth`;
the dog publishes `dogState`, `dogAction`, `mood`), which `DogPlay.OnCreate` resets. A global
`PlayConfig` tunes the dog: `fetchDisabled = true` keeps it watching thrown toys (and leaving lying
ones) instead of fetching; `mood = { energy, excitement, affection, curiosity }` restarts it from that
mood; `seed` repeats its random choices.

The play scenes turn on groom representation streaming (`StreamingSettings` representation
budgets, set by `make_play_scene.py`). A dog that walks up to the camera and away again crosses its
coat's LOD steps all the time; without streaming each crossing rebuilt the strand stream and the
coat volume on the main thread, a one-second freeze. Streamed, the next step is prepared on workers
while the floor, the card tier, draws. Two budgets must hold a whole strand tier:
staging its preparation estimate (the Samoyed's 8121 MiB, the Bernese's 6914, the golden's 2140;
the scenes set 10240), and a frame's upload the prepared tier, which is built in one frame (2061,
1784 and 568 MiB; the scenes set 2560). A tier larger than either is never started or never built,
and the coat stays on cards however close the camera comes. `GroomRenderPass` warns once when
that happens ("never streamed in"), and `DogPlayEvidenceTest.CloseUpThePlaySceneDrawsTheCoatOnStrands`
checks each breed close up.

## What differs per breed

Only data, written by generators:

- **The clips.** The play clips (Run, PickUp, Carry, Drop, PlayBow, Petted) and the life clips
  (Trot, Sniff, Shake, LieDown, GetUp, BellyUp, RollOver, Excited, Scratch, Stretch, Nudge,
  TurnInPlace) are authored once, in `tools/dog-authoring/dog_clips.py`, relative to each rig's rest
  pose, and appended to every breed's model after its own six. A model without one of them logs a
  warning once and the dog skips that behaviour. `build_dog.py` calls it for the golden; the Bernese and Samoyed
  get them through their sources and `rebuild.py`. Each rig's `rig.json` records the clips' events
  (PickUp `grab` 0.62, Drop `release` 0.48), its run and carry speeds, its mouth sockets and where
  its mouth meets the ground at the grab (`fetch.pickUpBall`).
- **The scenes.** `tools/dog-authoring/make_play_scene.py` derives each play scene from the breed's
  showcase scene and its rig: the camera leaves the dog and orbits a play spot; the dog gets its
  pick volumes, nav agent, head aim and script; `MouthSocketBall` / `MouthSocketStick` ride the jaw at
  the rig's sockets; `PickUpReach` marks where the mouth meets the ground; a hand, a ball, a stick
  and a navmesh over the lawn are added. Rerun it after a rig changes.

## Known limits

- **The parting is as fine as the coat's guides.** Strands follow their guides' displacement, so a
  hand parts the coat in patches the size of the guide spacing, and the card tier parts the same way
  (cards follow their source guide). The coat's self-shadow is baked at rest and does not part.
- **The Samoyed picks up close to its forepaws** (0.10 m; the golden 0.28 m, the Bernese 0.26 m):
  its head sits back over its chest and the rig's short neck cannot reach further.
- **The run and carry cycles are short** (0.40 s and 0.47 s): at 3 m/s and 1.5 m/s these legs
  cannot reach longer strides with their paws planted.
- **Getting up from a belly rub crossfades.** RollOver goes from lying to belly-up; there is no
  clip back, so BellyUp blends into Lie.
- **Turning on the spot slides the paws a little.** TurnInPlace steps in place while the script
  turns the root, so a planted paw slips by the turn rate times 0.18 m.
- **Scratch reaches behind the ear, not into it**: the hind paw stops 1-20 mm short (fur covers
  the gap).

## Verification

`DogPlayEvidenceTest` (`Rendering/PropertyTests`), per breed, on runtime frames with the player
driven through synthetic input: a flicked ball is fetched and dropped at the player's feet; with
fetching off it stays where it landed; a stroke parts the coat where the hand passes and nowhere
else, and a stroke that misses leaves it untouched; a favourite spot earns a stronger reaction than
the back, and the dog keeps the spot under a resting hand. The stroke counts only coat that could
leave the hand: fur the hand presses flat against the skin, below its centre, has nowhere to go.
Through the fetch the head never flicks one way and back (0.2 rad out and back within 0.6 s) and
carries the toy home looking ahead. A toy swept round the dog turns its body, and a toy held up
behind it turns it round, with the head within 1.3 rad of the body throughout. A player walking off
with a toy is followed.

The life between games, each from a set mood against the opposite one: the head follows a cursor
beside it and not one across the lawn; a dog wanting company comes to the player and a content one
stays; a tired dog lies down and rolls over when petted, a rested one stands and leans in; a toy
held up gets an invitation to play, a toy lying there does not; an excited dog fetches a lying toy
on its own and a calm one leaves it.

`ScriptPhysics3DPlaySurfaceTest` (Functional) covers the script physics surface (impulse, velocity,
body type, contacts, carry and release, attach in place, world position) and
`GroomGuideSimulationTest` the coat colliders.
