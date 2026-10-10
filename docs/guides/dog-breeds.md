# Samoyed and Bernese source assets

The approved Samoyed and Bernese mountain dog are shipped with their complete strand coats, facial morphs, animations and dedicated eye profiles. Visual quality is the acceptance criterion; renderer performance work is separate. The Golden model and its generator are unchanged by this delivery.

## Get the assets

Install Git LFS, then run `git lfs pull` after cloning. The repository's `.gitattributes` identifies the model buffers, Alembic sources, native grooms, bindings and intermediate authoring buffers. CI shares one object download with each build's test shards. Hosted jobs restore the default-branch cache; self-hosted jobs keep objects on local disk.

Open `SandboxProject/Assets/Scenes/Samoyed.olo` or `Bernese.olo` in OloEditor. Keep each model together with its maps, eye profile, groom and binding. Copying a glTF without its external buffer produces an incomplete model.

| Model | Facial targets | Strands | Rendered segments |
|---|---:|---:|---:|
| Samoyed | 19 | 1,999,138 | 7,091,971 |
| Bernese | 8 | 1,615,603 | 6,134,310 |

## Rebuild from editable sources

The user selected versioned editable sources and reproducible rebuilds for this PR. The production workflow reconstructs the approved models from intermediate glTF meshes and deterministic refinement scripts. Maps and the metadata-complete Alembic groom are authored source assets. This workflow does not replay the historical procedural breed-generator experiments.

Install Python 3.10+ and the pinned dependencies:

```powershell
python -m pip install -r tools/dog-authoring/requirements.txt
```

Build `OloEngine-Tests` in Release through the repository's build lock, then run one command per breed from the repository root. Each output must be a new directory:

```powershell
python tools/dog-authoring/rebuild.py --breed Samoyed --output C:/temp/samoyed-rebuild --verify-shipped
python tools/dog-authoring/rebuild.py --breed Bernese --output C:/temp/bernese-rebuild --verify-shipped
```

Use `--test-exe PATH` for another native test executable. `--mesh-only` runs the geometry/map stages without native cooking and records that limitation. The native cook requires an Alembic-enabled engine build and a graphics context; a skipped fixture is treated as an incomplete rebuild.

The output is an asset overlay for this checkout: `Assets/Models/<breed>`, the authored scene, shared skin profiles, and the freshly cooked groom/binding pair in `Assets/Grooms/<breed>`. It is not a standalone project; the scene retains the Sandbox project's registered handles and environment references. `rebuild.json` records input/output hashes, whether native cooking completed, and verification results. Intermediate stages remain editable under `stages/`. The command never installs over the approved assets.

`--verify-shipped` requires exact topology, base positions, skin weights, animation, rig metadata and native groom/binding. The authored 8-bit PNG maps require identical dimensions, mode, palette, decoded texels and metadata, including color-space data; lossless compression bytes may differ between Pillow versions and are listed in `pngEncodingDifferences`. Morph position/normal deltas and their accessor bounds permit at most 1e-9 absolute roundoff between numerical-library implementations; the measured Samoyed rebuild differs by at most 2.33e-10, and Bernese is exact. This tolerance does not cover base geometry or animation.

## Authoring layout

- `tools/dog-authoring/sources/Samoyed`: the intermediate eleven-target head/rig before the final eye, cheek, mouth and brisket refinements.
- `tools/dog-authoring/sources/SamoyedLidSource`: the original bind surface whose lid influences distinguish rigid lids from surrounding facial skin.
- `tools/dog-authoring/sources/Bernese`: the unmorphed model, original coat map, eye profile and scene used by the refinement stages.
- `Assets/Models/Samoyed/refine_samoyed.py`: eight ordered stages producing the final nineteen targets.
- `Assets/Models/Bernese/build_bernese.py` and `refine_bernese*.py`: six stages producing the final eight targets and coat/iris maps. `authoring_maps.py` keeps this work independent of the Golden generator.
- `Assets/Models/<breed>/<breed>.abc`: the full groom source. Preserve its `groom_` metadata; a generic Alembic re-export can lose strand groups, widths, pigment and card-authoring settings.

Edit a source or recipe, rebuild into a new directory, inspect front/side/three-quarter and blinking/panting views with the full coat, and only then install a new accepted version. A topology or skin-binding edit also requires a new native binding.

## Approved visual evidence

- [Samoyed: twelve native full-fur views](../reviews/dog-breeds-1558/samoyed-20261009/index.html)
- [Bernese: nine native full-fur views and real-dog comparisons](../reviews/dog-breeds-1558/bernese-mouth-20261009/index.html)

The approval galleries were captured in Release OpenGL Forward. They establish visual acceptance; backend, import, animation and packaged-runtime verification are separate checks recorded with the delivery evidence.
