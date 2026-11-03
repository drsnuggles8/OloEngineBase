#pragma once

#include "OloEngine/Core/Base.h"

#include <string>

struct aiMesh;
struct aiScene;

namespace Assimp
{
    class Importer;
}

namespace OloEngine
{
    // Shared smooth normals for meshes that arrive without any, generated the way
    // aiProcess_GenSmoothNormals means to: each face corner gets the sum of the
    // normals of the faces at its position that lie within `maxSmoothingAngleDegrees`
    // of its own face, normalised. aiProcess_GenSmoothNormals itself only gets that
    // right on a "verbose" mesh, one vertex per face corner. It writes each face's
    // normal onto that face's vertices and then averages the vertices at a position,
    // so on an indexed mesh, which glTF2 delivers without marking it as such, every
    // shared vertex keeps the normal of whichever face wrote it last.
    //
    // This works on the indexed mesh directly. A vertex keeps its index unless its
    // corners need different normals (a crease sharper than the limit); the extra
    // normals go to copies of the vertex, with its bone weights and morph-target data.
    // A mesh that already has normals, or has no triangles, is left alone.
    void GenerateMissingSmoothNormals(aiMesh& mesh, f32 maxSmoothingAngleDegrees);

    // ReadFile(path, flags) with GenerateMissingSmoothNormals run on every mesh of the
    // raw scene before any post-processing, at the importer's
    // AI_CONFIG_PP_GSN_MAX_SMOOTHING_ANGLE. aiProcess_GenSmoothNormals in `flags` then
    // finds the normals present and leaves them be. Returns what ReadFile would:
    // null on failure, with the reason in importer.GetErrorString().
    [[nodiscard]] const aiScene* ReadSceneWithSmoothNormals(Assimp::Importer& importer, const std::string& path,
                                                            u32 flags);

    // The middle step on its own, for an importer that already holds the raw scene
    // (ReadFile(path, 0)) and calls ApplyPostProcessing itself.
    void GenerateMissingSmoothNormals(Assimp::Importer& importer);
} // namespace OloEngine
