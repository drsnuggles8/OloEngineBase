# Samoyed appearance checkpoint

The 9 October 2026 model preserves the selected face, full double coat, softened
ear furnishings, lower body silhouette, curled plume and expression rig. A local
brisket correction rounds the former shelf in front of the forelegs. The user
requested this checkpoint before continuing with the Bernese mountain dog.

Open `Assets/Scenes/Samoyed.olo` in the Sandbox project. The scene uses this glTF,
its texture maps and eye profile, and the matched native groom and binding in
`Assets/Grooms/Samoyed/`. The source Alembic archive retains the complete hair
curves and import metadata. The coat has 1,999,138 strands; no performance-driven
quality reduction is included.

The large source and groom files use Git LFS. After checkout, run `git lfs pull`
before opening the scene or running asset checks. CI checkouts of this checkpoint
also need LFS objects available.

The [native capture gallery](../../../../../docs/reviews/dog-breeds-1558/samoyed-20261009/index.html)
contains twelve front, three-quarter, body, side, chest, pant, tilt and blink views.
Its [manifest](../../../../../docs/reviews/dog-breeds-1558/samoyed-20261009/evidence.json)
records the exact asset hashes, capture provenance and limits.

This is an authored asset checkpoint. `build_samoyed.py` is an earlier editing
recipe requiring an explicit frozen input; it is not a one-command reconstruction
of this final asset. The exported glTF, complete Alembic and matched cooked files
are the retained source of truth. Generator consolidation remains future work.
There are 19 morph targets, including the final static `SamoyedRoundedBrisket`;
do not rerun the old editing recipe on the final asset and double-apply its edits.

Appearance was checked in the existing Release OpenGL Forward worktree editor.
Earlier shared groom/renderer work is still separate pending work on this branch.
The broader backend, packaged-runtime and import-parity matrix is unfinished.
Renderer performance improvements are explicitly deferred to another worktree.
