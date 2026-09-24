#pragma once

#include "OloEngine/Containers/Array.h"
#include "OloEngine/Core/Base.h"

#include <assimp/mesh.h>

namespace OloEngine
{
    // Appends the three indices of every TRIANGLE face of `mesh` to `indices`, and returns
    // how many faces were skipped because they were not triangles.
    //
    // Every consumer of the result reads it as a flat triangle list, so a single two- or
    // one-index face in the middle shifts every triangle after it: the mesh stays within
    // its vertex range and draws as garbage, and meshoptimizer asserts on an index count
    // that is not a multiple of three (issue #1440). aiProcess_Triangulate leaves line and
    // point faces alone, and aiProcess_FindDegenerates CREATES them, so neither makes a
    // mesh safe to read three indices at a time. The caller decides what a skipped face
    // means; it must say so rather than drop them silently.
    [[nodiscard]] inline u32 AppendTriangleIndices(const aiMesh& mesh, TArray<u32>& indices)
    {
        u32 skippedFaces = 0;
        for (u32 i = 0; i < mesh.mNumFaces; ++i)
        {
            const aiFace& face = mesh.mFaces[i];
            if (face.mNumIndices != 3)
            {
                ++skippedFaces;
                continue;
            }
            indices.Add(face.mIndices[0]);
            indices.Add(face.mIndices[1]);
            indices.Add(face.mIndices[2]);
        }
        return skippedFaces;
    }
} // namespace OloEngine
