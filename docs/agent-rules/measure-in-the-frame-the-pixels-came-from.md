# Unproject each readback through its own frame's camera, and classify in the subject's own frame

**The rule:**

- **Readback.** A world point read back from a depth buffer comes from the matrix that frame was
  drawn with, taken after that frame. In the dog fixture that matrix is
  `RenderedViewProjection()`, which is `Scene::GetCameraViewProjection()` checked against the
  stored pose.
- **Classifier.** A region classifier written in a subject's own frame is fed points carried into
  that frame by the inverse of the subject's world matrix for the captured state. In the dog
  fixture that is `WorldToDog` then `MaskCoat`.
- **Required regions.** A measurement that needs a region to be seen states which regions each view
  frames. If a required region falls under its floor, the test fails. It never skips the region.

## What happened (#1533 review)

Both bugs went unnoticed while the dog stood at the origin. They surfaced when the walk began to
travel by root motion, carrying the runtime camera parented to the dog.

- **B6 used a stale matrix.** B6's `RecordRuntime` took its matrix once, after the warm-up. The
  60 measured frames then moved the dog and its camera 5.2 mm each, so frame 60 was unprojected
  31 cm from where its depth said. That is as far as the classifier searches for the body.
  - A pixel with no body vertex within 30 cm left every region. The per-frame lost count was
    computed and then discarded.
  - A region under its pixel floor was skipped.
  - So the measurement could quietly shrink and still pass.
- **The rest bake classified world points as dog-local.** Its coarse partition (face, tail, torso
  boxes in the dog's own frame) was handed world points. Its editor camera also stayed at the
  origin while the `WalkLegs` case walked the dog 0.23–0.44 m forward.
  - The case's tail was never measured: it read 0.000 and was printed as below its floor.
  - Fixed, the tail reads 0.215 on the coarse partition and 0.259 by the body's own parts. The
    whole coat reads 0.082, up from 0.066.
  - Only Walk moves the root bone. Idle, Sit and the Idle lighting record stand at the origin, and
    their labels did not change.
- **What the stale B6 matrix did to the counts.** It moved pixels between parts. The front view's
  ears read 6,155 px a frame where they hold 14,828. The rear view's tail read 39,181 where it
  holds 28,247. The ratios moved by at most 0.02.

## The shape that replaced them

- **The readback.** Every runtime readback takes `RenderedViewProjection()` after its frame.
  `RecordRuntime` keeps each frame's placement counts, and B6 checks two things on every measured
  frame (`ExpectPlacedEveryFrame`):
  - Its coat is placed on the body: at most 0.1% of the coat pixels have no body vertex within
    30 cm.
  - Each region its view frames has its floor (`RequiredShimmerRegions`).
- **The partition.**
  - `MaskCoat(frame, worldToDog)` classifies in the dog's frame.
  - `CompareRestBake` places its view relative to the dog (`DogRelative`) and checks that the four
    arms found the dog in one place.
  - It also classifies each coat pixel by its nearest skinned body vertex (`Anatomical`). That
    segmentation follows a sit or a stride, and the acceptance check on the face reads it. The
    coarse boxes are a record, not a segmentation.

## The tests that hold it

- **`TheRuntimeReadbackFollowsTheTravellingDog`** reads each walk frame through its own camera and
  through the first frame's. It runs twice: once with the dog as shipped, and once with the dog
  turned 57° and moved 2.7 m off the origin.
  - Through each frame's own camera, the eyes' pixels lie within 0.16 mm of the eye spheres. The
    eyes are exact anchors. No coat pixel is lost.
  - Through the first frame's camera, the last frame's eye pixels land 441.5 mm from the eye
    centre, which is the camera's travel. 9.4% of the coat is lost there.
- **`TheCoatRegionsAreTheDogsOwnWhereverItStands`** checks face, eye-socket and tail samples under a
  walk, a turn, a slope and a 1.25 scale. Classified without the dog's frame, 4 to 6 of the 7
  samples change part.
