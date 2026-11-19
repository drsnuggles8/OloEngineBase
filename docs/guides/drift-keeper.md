# The keeper of Stacks Light

Sail past Stacks Light in daylight and the keeper turns towards the boat.
Within 62 m he waves once. Sail beyond 78 m and he can greet the next approach.
He watches out to 115 m. From 18:30 until 06:00 he raises his brass lantern.
He stands on the granite tread with his feet planted. His boots, ochre oilskin
and the light in his left hand give him a silhouette you can read from the water.

## Drawing the figure

Three photographs guided different parts of the design. The keeper's head,
body, skin pigmentation, clothes and fibres were drawn in code. The photographs
were used to compare landmarks and silhouettes.

![An 1888 keeper portrait, three standing Newhaven men, and Whitby fishermen at the harbour](../images/drift-keeper/reference.jpg)

- In the 1888 portrait of Cape Elizabeth keeper Marcus Hanna, the near eye is
  halfway from crown to chin. On a 600-pixel-tall copy the three landmarks fall
  at about 39, 159 and 278 px. The moustache clears the mouth at its centre.
  The raised collar follows the jaw and the shoulder seam slopes away from it.
  [US Lighthouse Society archive](https://archives.uslhs.org/people/marcus-hanna) · [Public-domain portrait record](https://commons.wikimedia.org/wiki/File:Marcus_Hanna,_Lighthouse_keeper_at_Cape_Elizabeth_ME.jpg)
- In Hill and Adamson's Newhaven group (1843–47), the middle man's fingertips
  hang 54% of the way from cap to sole and his jacket ends at 46%. The keeper
  has a longer storm coat, with a separate cape to shed spray from the shoulders.
  David Octavius Hill and Robert Adamson, *Alexander Rutherford, William Ramsay
  and John Linton*. National Gallery of Art, Paul Mellon Fund. Public domain.
  [Photograph](https://www.nga.gov/artworks/137713-alexander-rutherford-william-ramsay-and-john-linton)
- Frank Meadow Sutcliffe's *Whitby Fishermen* (c. 1885) shows how much boot
  height varies from man to man. The third standing figure from the left wears
  boots reaching about 41% of his dressed height. Look at the folds above his
  ankles and the way the shaft opens around the knee. The keeper wears a
  shorter boot with his trouser cuffs tucked inside. National Gallery of Art,
  Robert B. Menschel Fund. Public domain.
  [Photograph](https://www.nga.gov/artworks/108746-whitby-fishermen)

Peter Gregory Gallop's late-1800s keeper portrait supplied the high crowned
cap, short peak, badge and long divided beard. On the 340 × 415 px portrait
the pupils are about 63 px apart and the nose base is about 41 px below their
midpoint. The generated face has 64 mm between the pupils and a 232 mm head.
Its standing height is 7.54 heads, close to the seven to seven and a half
used in the Seattle Art Museum's figure-drawing exercise.
[Gallop portrait, National Park Service](https://www.nps.gov/people/petergregorygallop.htm) · [Seattle Art Museum, appendix C](https://www1.seattleartmuseum.org/EuropeanMasters/imgs/KenwoodHouseERG.pdf)

## What changed in the studies

![Four successive three-quarter views showing the bare construction and three revisions of the haired face](../images/drift-keeper/face-development.jpg)

The first pass exposed the sockets and mouth in Workbench. Flat planes ran
across the lower orbits, and the visible teeth looked like a row fixed to the
front of the mouth. The first strand pass established the beard silhouette but
produced an almost white sheet with a straight part down the centre. Mixing
silver and brown by follicle broke up that sheet. The cap gained a cord and
peak, the lashes acquired fitted roots, and the cut of the shoulder cape became
visible at full length. The fifth study rounded the ear bowl and lip volume and
narrowed the cloth where the trousers enter the boots. The sixth recessed the eyes, added colour to the wind-exposed cheeks, shortened guard hairs at the beard line and advanced one boot on the tread.

![Four views of the keeper at face distance](../images/drift-keeper/four-views.jpg)

Read the head from the side as well as from the front. The peak leaves room for
the eyes to turn. The nose projects sharply, the neck stands upright and the
hair ends in a nearly level fringe across the back of the head. From above,
the strong line under each eye and the sharply drawn cheek line of the beard
still read as graphic shapes. These are fixed-camera Blender studies. In
OloEngine, the eye and oral profiles, hair rendering, lighting and LOD changes
also affect the result.

## Hands, expression and motion

![The keeper at rest, waving, raising the lantern, blinking, opening his mouth and forming a rounded vowel](../images/drift-keeper/movement.jpg)

The wave shows the right palm and the left fingers curl round the lantern bail.
The fingers have separate profiles, pads and nails. Each hand has one joint,
so the fingers keep their authored curl during the gesture. The wave is driven
by the shoulder, elbow and wrist. Clothing folds and the flare at the cuff
travel with those joints. The elbows remain sharply defined at the top of the
wave. The raised sleeve meets the cape in a hard ridge. The torso shifts
little compared with the reach of the arm. In the earlier lantern pose the
right arm stayed up from the wave and the lamp tilted with the left forearm.
Rest keys now release the right shoulder, and a joint at the bail keeps
the lantern upright as the hand rises.

The upper and lower lids wrap each globe. A full blink closes the aperture.
The lower lip and dental arch descend for the open vowel. The cheeks move only
a little on a smile, and the thick moustache obscures much of the mouth. The
teeth are individually modelled, but their regular spacing and similar shapes
are apparent on inspection. The neck, shoulders and jaw lack the secondary
flesh movement of a speaking or gesturing person. The beard carries direction
and length across the jaw, yet the cut edge above each cheek is unusually crisp.

## Comparing the face

![The generated keeper and a textured render of the Infinite Scan bust from the same three-quarter direction](../images/drift-keeper/keeper-and-scan.jpg)

Both busts use the same viewing direction. The scan has a different crop,
closed eyes and exposed cheeks. Compare where light rolls from the temple
into the socket and from the nostril into the upper lip. The scan carries
small changes of curvature and colour through those transitions. The keeper's
nose, brow and cheek still meet in distinct planes. The keeper's separable eyes
and mouth serve his Drift rig, but the close-up falls short of the scanned
reference. `DigitalHuman.olo` continues to use the scan.
[Human-head acceptance issue #1402](https://github.com/drsnuggles8/OloEngineBase/issues/1402)

The right panel is a Blender render, reframed for this comparison, of *Infinite,
3D Head Scan* by Lee Perry-Smith (Triplegangers). Morgan McGuire and Guedis
Cardenas converted the files. Keijiro Takahashi prepared the FBX distribution.
The scan and its render carry CC BY 3.0. [Attribution and source chain](../../OloEditor/assets/models/InfiniteScanHead/README.md) · [Licence](https://creativecommons.org/licenses/by/3.0/)

## In the scene

`Stacks Light` parents the keeper, so moving the tower moves him with the
steps. A triangle-intersection check at thirteen poses per clip found clearance
from the door, treads and rail through idle, wave and lantern raise. The initial
position put the heels through the raised door sill. On the final tread the two
heel samples are within 3 mm of the granite. The body supplies the pose and nine expression targets. Each eye has
its own bone attachment. A second head attachment carries the pigmented skin
under the groom. Its triangle indices stay fixed as the body changes LOD, so
each beard, brow or lash root keeps its binding. The cook contains 23,000 beard
curves, 3,800 moustache curves, 13,500 scalp hairs, 2,100 brow hairs and 270
lashes. The lantern's light follows a socket at its wick.

The three body meshes have 95,088, 26,124 and 11,088 triangles. At the same
three-quarter pose and orthographic scale, their silhouettes cover 76,940,
76,624 and 75,788 pixels in 520 × 680 frames. Thresholding alpha at one
half gives 99.52% overlap between tiers 0 and 1 and 98.49% between tiers 0
and 2 (intersection divided by union). This measures the outline, including
the shared head attachments. Use the model README to regenerate the body and
paired groom assets. To check a change in the
game, inspect the eyes, mouth, shoulder and lantern separately in close-up,
then look back at the tower from 30 and 150 metres. The face detail disappears
first. The coat hem, cap peak and bent arm carry the gesture farther.
[Asset contract and regeneration commands](../../OloEditor/SandboxProject/Assets/Models/DriftKeeper/README.md)
