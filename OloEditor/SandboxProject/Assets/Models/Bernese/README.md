# Bernese appearance checkpoint

The user accepted the 9 October 2026 model, including its rounded lower muzzle,
furred blinking eyelids, tricolour legs and complete coat. The coat contains
1,615,603 strands; performance optimisation is deferred.

Open `Assets/Scenes/Bernese.olo` in the Sandbox project. It uses this glTF and its
maps, eye profile, and matched groom and binding in `Assets/Grooms/Bernese/`.
Fetch Git LFS objects before opening the scene or running asset checks.

The [native comparison gallery](../../../../../docs/reviews/dog-breeds-1558/bernese-mouth-20261009/index.html)
contains nine matching before/current views and real photographic references.
Its [manifest](../../../../../docs/reviews/dog-breeds-1558/bernese-mouth-20261009/evidence.json)
records asset hashes and capture validation. These captures use the Release
OpenGL Forward editor with the complete coat.

The eight morph targets preserve the nasal bridge, jaw/pastern sculpt, eye and
muzzle proportions, lid aperture, three blink closure shapes and rounded lip.
All six animation clips retain their complete morph key timelines.

The `refine_bernese*.py` scripts are sequential editing recipes with explicit
source directories and new output directories. Do not apply them to this final
model a second time. Consolidation of portable authoring and the broader import,
backend and packaged-runtime verification follow this accepted checkpoint.
