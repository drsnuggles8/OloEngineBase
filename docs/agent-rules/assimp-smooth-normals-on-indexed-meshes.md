# Assimp smooth normals on indexed meshes

Read every Assimp import that generates normals through `ReadSceneWithSmoothNormals`
(`Renderer/AssimpSmoothNormals.h`). Do not rely on `aiProcess_GenSmoothNormals` alone.

1. **Generate missing normals with `GenerateMissingSmoothNormals` on the raw scene, before any post-processing.** `aiProcess_GenSmoothNormals` assumes one vertex per face corner. glTF2 delivers indexed meshes without saying so, and the step then gives each shared vertex the normal of the last face that wrote it. [Details](assimp-smooth-normals-on-indexed-meshes-details.md#1-what-assimp-does-on-an-indexed-mesh)
2. **Pin normal generation with a curved fixture and the normal direction, not with a flat one or a vertex count.** On a plane every face normal is equal, and the broken path keeps the vertex count, so both pass. [Details](assimp-smooth-normals-on-indexed-meshes-details.md#2-what-catches-it)
