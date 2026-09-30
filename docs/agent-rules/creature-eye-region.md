# A furred creature's eye region (#1533)

Read before touching the socket, the lid shells or the orbital skin in
`Models/Dog/build_dog.py`, the `EyeRim` / `Lid` layers or `EyeRampAt` in
`DogShowcaseEvidenceTest.cpp`, or `DogEye.oloskin`. The same traps wait for any creature whose eyes
are separate globes behind separate lids.

## The rules

1. **The face's skin closes a few degrees outside the lids' margin, all the way round.** A socket
   carved as a sphere meets the head's surface wherever the face happens to be. On the showcase dog
   that was 59 degrees off the gaze above the eye and 105 at the temple, and everything between that
   rim and the lid margin was lid shell: a bald dome in a crater, an eyeball glued into a porthole.
   `f_orbit_skin` lays 3.5 mm of skin over the socket outside an opening that follows the lids' own
   almond (`lid_aperture`) `ORBIT_BAND` (5) degrees out, and the socket is carved again through it
   with a small fillet (`SOCKET_EDGE`; the first carve's fillet is wider than the skin is thick and
   removes it). A four-point outline (46 / 60 / 52 / 64 degrees) left 12 to 14 degrees of lid showing,
   a smooth pale band of short lid fur round every eye. Check below the eye too: there the cheek
   closes over the globe 17 to 23 degrees off the gaze, above the lower lid, so the lower lid rests at
   the cheek's edge (`LID_LOWER_OPEN` -22) and the first carve's fillet is 3 mm (`SOCKET_FILLET`); at
   -40 and 6 mm the eye had no lower lid line and a pale, rounded cheek lip under it.

2. **The opening is narrower than the globe, and centred on the iris as it is seen.** With the lid
   corners at 84 degrees the whole side of the globe showed, and every three-quarter view had a white
   triangle behind the iris. The corners sit at 58 degrees on the inner side, which the bridge of the
   nose partly covers, and 46 on the outer side, inside the iris's 53. At 58 on both sides the
   visible opening sat toward the temple and the irises looked crossed.

3. **The iris plane sits behind the limbus.** A wide animal iris behind a human chamber depth draws
   smaller than authored. `SkinProfile` now moves the plane back and reports it; see
   [eye-cornea-iris.md](../guides/eye-cornea-iris.md).

4. **Fur length changes continuously toward the eye, measured from the socket.** A ring region of
   short fur with its own lengths draws a ring where it meets the face. The dog's head layers share
   the face's lengths and shorten with `EyeRampAt`, keyed on the distance from the socket sphere's
   surface: 30% of the region's length at the opening's edge, whichever surface that edge is, full at
   22 mm. Keyed on the eye's centre the visible lid dome got one short length, a cushion inside a
   ring; keyed on the front of the eye, the cheek's edge below it (much nearer the front than the
   lids' margin) got the shortest fur on the face, a pale crescent.

5. **The lid's outer skin is pigmented and dry, and the margin is a thin dark line.** Only a sliver
   of lid shows between the face's skin and the margin, so it takes a golden retriever's dark lid
   colour: in the pelt's shade it read as a pale ring, and dark but moist (roughness 0.45) the dome
   reflected the sky and every eye sat behind a grey lens. The black, moist rim is `LID_RIM_BAND`
   (0.7 mm) plus the rolled margin (`LID_THICK`, 1.3 mm); at 1.2 and 1.75 mm the pair read as
   eyeliner. The socket walls (`EYE_RIM`, 0.2 mm) stay on the hidden side. Lid fur lies flat, short
   and fine, turned toward the back corner: combed straight away from the opening and gathered into
   tufts, it stood out like lashes all round the eye. Lid skin that no blink or brow raise brings
   into view is left bare, or its fur grows out through the face (`ORBIT_LID_MARGIN`).

6. **Close the lid shells to the globe's side poles.** At 84 degrees each pole kept a hole about
   3 mm across, and a three-quarter view looked through the outer one into the socket: a dark notch
   with white in it.

7. **A blink closes: check it in the engine, not by the sign.** `blink()` turns the upper lid by
   +`LID_CLOSE_UPPER` about `up x gaze` and the lower by -`LID_CLOSE_LOWER`. With the signs the
   other way round every blink opened the lids to the whole globe, twice every Idle loop, and no
   still frame at rest could show it. `OLO_DOG_LOOKDEV_CLIP=Idle OLO_DOG_LOOKDEV_FRAMES=67` renders
   the first blink closed.

## Diagnosing

- The Blender previews (`build_dog.py`'s second argument) show the bare head, where a crater, a dome
  or a notch cannot hide under fur. Look at `rest_face34.png` and `rest_front.png` first.
- The look-dev frames (`OLO_DOG_LOOKDEV=1`) include `EyeThreeQuarter`, a close-up of the left eye.
  `OLO_DOG_LOOKDEV_CLIP=Idle OLO_DOG_LOOKDEV_FRAMES=62` holds the first blink half closed.
- A white triangle that survives a narrower opening is the iris plane (rule 3), not the lids.
