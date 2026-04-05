#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"

#include <filesystem>
#include <vector>

namespace OloEngine
{
    class MeshSource;
    class AnimationClip;

    // Reads/writes .omesh binary mesh cache files.
    namespace MeshBinarySerializer
    {
        // Serialize a MeshSource to a binary .omesh file.
        // Includes geometry, submeshes, materials, skeleton, bone influences, morph targets.
        // Vertex/index/bone-influence buffers are encoded with meshoptimizer for smaller files.
        bool Write(const std::filesystem::path& path, const MeshSource& meshSource, u64 sourceTimestamp = 0);

        // Deserialize a MeshSource from a binary .omesh file.
        // Returns nullptr on failure (corrupt file, version mismatch, etc.).
        Ref<MeshSource> Read(const std::filesystem::path& path);

        // Read only the header to check version and source timestamp (lightweight).
        // Returns false if the file doesn't exist or has an invalid header.
        bool ReadTimestamp(const std::filesystem::path& path, u64& outSourceTimestamp);
    } // namespace MeshBinarySerializer

    // Reads/writes .oanim binary animation cache files.
    namespace AnimationBinarySerializer
    {
        // Serialize one or more AnimationClips to a binary .oanim file.
        bool Write(const std::filesystem::path& path, const std::vector<Ref<AnimationClip>>& clips, u64 sourceTimestamp = 0);

        // Deserialize all AnimationClips from a binary .oanim file.
        // Returns empty vector on failure.
        std::vector<Ref<AnimationClip>> Read(const std::filesystem::path& path);

        // Read only the header to check source timestamp.
        bool ReadTimestamp(const std::filesystem::path& path, u64& outSourceTimestamp);
    } // namespace AnimationBinarySerializer

} // namespace OloEngine
