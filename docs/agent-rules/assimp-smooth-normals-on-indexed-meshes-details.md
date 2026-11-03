# Assimp smooth normals on indexed meshes: details

## 1. What Assimp does on an indexed mesh

`GenVertexNormalsProcess::GenMeshVertexNormals` (Assimp 6.0.5) works in two passes. It first
computes each face's normal and writes it onto that face's vertices. It then gathers all vertices
at the same position and averages their normals, within `AI_CONFIG_PP_GSN_MAX_SMOOTHING_ANGLE`.

On a verbose mesh, one vertex per face corner, the first pass leaves one face normal per corner and
the second averages them. On an indexed mesh, each shared vertex is overwritten by every face around
it, so it keeps the last face's normal, and the second pass averages that one value with itself.
The step refuses a scene flagged `AI_SCENE_FLAGS_NON_VERBOSE_FORMAT`, but the glTF2 importer
produces indexed meshes without setting that flag. Nothing fails or logs.

Measured on a 16x10 UV sphere glTF without normals: every vertex's normal is one adjacent face's
normal, 9 to 12 degrees off radial. The shading is continuous but lopsided. The static route had
this since #653 moved it to `GenSmoothNormals`, and so did foliage.

`GenerateMissingSmoothNormals` works on the indexed mesh. Each face corner's normal is the sum of
the normals of the faces at its position within the angle limit, as in Assimp. A vertex whose
corners need different normals (a crease sharper than the limit) is copied once per extra normal.
The copy takes the vertex's colours, UVs, tangents, bone weights and morph-target data. A mesh
that already has normals is not touched, so `aiProcess_GenSmoothNormals` in the post-processing
flags then has nothing to do. `ReadSceneWithSmoothNormals` is `ReadFile(path, 0)`, this pass and
`ApplyPostProcessing(flags)`. The static route already parsed in two steps and calls the pass
between them.

Coincident vertices smooth together only when their positions are bit-identical. Assimp uses a
small epsilon instead. A glTF's split vertices are copies of the same floats, so this does not
matter for the sources here.

## 2. What catches it

The first version of `AnimatedModelNormalsImportTest` used a flat 3x3 grid. All eight face normals
are +Z, so last-writer-wins gives the right answer, and the vertex count stayed 9. It passed on
the broken path.

- `AnimatedModelImportSettingsTest.ANormallessRiggedSphereImportsWithRadialNormals` checks that
  each normal points along its vertex's position (dot > 0.99). The broken path measures
  0.978-0.987.
- `AssimpSmoothNormalsTest` covers the crease split on a cube with shared corners: 8 vertices
  become 24, and each copy keeps its bone weight and morph position.
- `SkinnedNormalsVisualEvidenceTest` renders the sphere on every path and counts brightness steps.
  It catches flat normals (2.4-2.6% of interior pixels against 0.13-0.19%). It does not catch
  last-writer-wins, because one interpolated normal per vertex still shades without steps.
