#pragma once

#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"
#include "OloEngine/Renderer/Material.h"

#include <glm/glm.hpp>

namespace OloEngine
{
    // Helper function to compute depth from camera space for sort key.
    // Returns a quantized depth value in range [0, 0xFFFFFF] for 24-bit depth.
    // @param modelMatrix The model transformation matrix.
    // @param boundingSphereCenter Optional world-space bounding sphere center. If provided,
    //        uses this for more accurate depth sorting for off-center meshes. If nullptr,
    //        falls back to using modelMatrix[3] (the origin of the transformed object).
    inline auto ComputeDepthForSortKeyWithView(const glm::mat4& modelMatrix,
                                               const glm::mat4& viewMatrix,
                                               const glm::vec3* boundingSphereCenter = nullptr) -> u32
    {
        const glm::vec4 worldPos = boundingSphereCenter
                                       ? glm::vec4(*boundingSphereCenter, 1.0f)
                                       : modelMatrix[3];
        const glm::vec4 viewPos = viewMatrix * worldPos;

        // Use negative Z since camera looks down -Z axis.
        f32 depth = -viewPos.z;

        // Clamp depth to reasonable range [0, 1000] and quantize to 24 bits.
        constexpr f32 MIN_DEPTH = 0.1f;
        constexpr f32 MAX_DEPTH = 1000.0f;
        depth = glm::clamp(depth, MIN_DEPTH, MAX_DEPTH);
        const f32 normalizedDepth = (depth - MIN_DEPTH) / (MAX_DEPTH - MIN_DEPTH);
        return static_cast<u32>(normalizedDepth * 0xFFFFFF);
    }

    inline auto ComputeDepthForSortKey(const glm::mat4& modelMatrix,
                                       const glm::vec3* boundingSphereCenter = nullptr) -> u32
    {
        return ComputeDepthForSortKeyWithView(modelMatrix, CommandDispatch::GetViewMatrix(), boundingSphereCenter);
    }

    // Helper to generate material ID hash for sort key.
    inline auto ComputeMaterialID(const Material& material) -> u32
    {
        u64 hash = 0;

        if (material.GetType() == MaterialType::PBR)
        {
            const u64 albedoID = material.GetAlbedoMap() ? static_cast<u64>(material.GetAlbedoMap()->GetRendererID()) : 0ULL;
            const u64 metallicID = material.GetMetallicRoughnessMap() ? static_cast<u64>(material.GetMetallicRoughnessMap()->GetRendererID()) : 0ULL;
            const u64 normalID = material.GetNormalMap() ? static_cast<u64>(material.GetNormalMap()->GetRendererID()) : 0ULL;

            hash = albedoID;
            hash ^= metallicID + 0x9e3779b9ULL + (hash << 6) + (hash >> 2);
            hash ^= normalID + 0x9e3779b9ULL + (hash << 6) + (hash >> 2);
        }
        else
        {
            const u64 diffuseID = material.GetDiffuseMap() ? static_cast<u64>(material.GetDiffuseMap()->GetRendererID()) : 0ULL;
            const u64 specularID = material.GetSpecularMap() ? static_cast<u64>(material.GetSpecularMap()->GetRendererID()) : 0ULL;

            hash = diffuseID;
            hash ^= specularID + 0x9e3779b9ULL + (hash << 6) + (hash >> 2);
        }

        // Fold 64-bit hash to 16-bit material ID (as defined in DrawKey).
        return static_cast<u32>((hash ^ (hash >> 32)) & 0xFFFF);
    }

    // Helper to create default POD render state.
    inline auto CreateDefaultPODRenderState() -> PODRenderState
    {
        PODRenderState state{};
        // All fields are initialized to sensible defaults by the struct itself.
        return state;
    }

    // Helper to populate POD render state from material properties.
    // Maps MaterialFlag to PODRenderState for proper render state setup.
    inline auto CreatePODRenderStateForMaterial(const Material& material) -> PODRenderState
    {
        PODRenderState state{};

        // Depth test - most materials want this enabled.
        state.depthTestEnabled = material.GetFlag(MaterialFlag::DepthTest);
        state.depthWriteMask = true; // Write to depth buffer for opaque materials.
        state.depthFunction = GL_LESS;

        // Blend state - for transparent materials.
        if (material.GetFlag(MaterialFlag::Blend))
        {
            state.blendEnabled = true;
            state.blendSrcFactor = GL_SRC_ALPHA;
            state.blendDstFactor = GL_ONE_MINUS_SRC_ALPHA;
            state.blendEquation = GL_FUNC_ADD;
            // Transparent objects typically don't write to depth buffer.
            state.depthWriteMask = false;
        }
        else
        {
            state.blendEnabled = false;
        }

        // Culling - controlled by TwoSided flag.
        if (material.GetFlag(MaterialFlag::TwoSided))
        {
            // Double-sided materials don't cull any faces.
            state.cullingEnabled = false;
        }
        else
        {
            state.cullingEnabled = true;
            state.cullFace = GL_BACK;
        }

        return state;
    }
} // namespace OloEngine
