#pragma once

// Editor/cook-only OpenVDB (.vdb) importer (#724). This header — and every
// header it drags in — is compiled ONLY into the OloEngine-VolumeCook static
// lib, which OloRuntime and OloServer never link (see the OLO_WITH_OPENVDB
// option comment in the root CMakeLists.txt and
// docs/agent-rules/asset-import-usd-alembic.md). Do not #include this from
// anything in OloEngine itself.

#include "OloEngine/Core/Base.h"

#include <filesystem>
#include <glm/glm.hpp>
#include <string>
#include <vector>

namespace OloEngine::VolumeCook
{
    // A dense-resampled OpenVDB grid, ready to hand to
    // VolumeSerializer::SerializeToFile/EncodeToBytes. Pure data — no
    // GPU/RHI, no OpenVDB types — so it can cross the OloEngine-VolumeCook /
    // OloEngine boundary freely.
    struct VolumeGridDense
    {
        glm::uvec3 Dimensions{ 0u, 0u, 0u };
        glm::vec3 VoxelSize{ 1.0f, 1.0f, 1.0f };
        // Dense-index CORNER space [0, Dimensions] -> world space, derived
        // directly from the source grid's own index-to-world transform (see
        // .cpp) — preserves translation/rotation/non-uniform scale exactly
        // as authored, which is the metadata issue #724 names as the reason
        // not to just bake slices in a DCC.
        glm::mat4 GridTransform{ 1.0f };
        f32 BackgroundValue = 0.0f;
        std::vector<f32> Density; // Dimensions.x * Dimensions.y * Dimensions.z, row-major (x fastest, then y, then z)
        std::string SourceGridName;
    };

    struct VolumeCookOptions
    {
        std::string GridName;        // empty = first scalar (float) grid found in the file
        u32 MaxAxisResolution = 256; // dense-resample cap per axis; trilinear-downsamples a larger active region rather than refusing it
    };

    struct VolumeCookResult
    {
        bool Success = false;
        std::string ErrorMessage;
        VolumeGridDense Grid;
    };

    // Reads a .vdb file and dense-resamples the named (or first scalar) grid's
    // ACTIVE region into VolumeGridDense. Trilinear-interpolated when the
    // active region exceeds MaxAxisResolution per axis; a straight sample
    // otherwise (no upsampling — a small source stays small).
    [[nodiscard]] VolumeCookResult ImportOpenVDBVolume(const std::filesystem::path& vdbPath,
                                                       const VolumeCookOptions& options = {});

    // Convenience: import + cook straight to a .olovol file (the engine-
    // native format VolumeSerializer reads) — the single call the editor's
    // content-browser import step needs.
    [[nodiscard]] bool CookOpenVDBToNativeFile(const std::filesystem::path& vdbPath,
                                               const std::filesystem::path& outputOlovolPath,
                                               const VolumeCookOptions& options, std::string* outError = nullptr);

    // Lists every scalar (float) grid's name in a .vdb, for an import-time
    // grid picker — reads only the file's grid metadata, not the tree data.
    [[nodiscard]] std::vector<std::string> ListScalarGridNames(const std::filesystem::path& vdbPath);
} // namespace OloEngine::VolumeCook
