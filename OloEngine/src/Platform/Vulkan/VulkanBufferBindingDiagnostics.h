#pragma once

#include "OloEngine/Core/Base.h"

#if OLO_WITH_VULKAN

#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "Platform/Vulkan/VulkanShader.h"

#include <format>
#include <string>
#include <string_view>

namespace OloEngine
{
    enum class VulkanMissingBufferSeverity : u8
    {
        Trace,
        Warning,
        Error,
    };

    // Optional means a reviewed shader declaration with a gate before its read,
    // not a binding number: 17 also holds indirect arguments, 63 also holds
    // bones, foliage and particle data, and 79 also holds the VT bake and
    // indirection-update blocks. Those tenants require real buffers.
    // occupantAbsent must be false when a published buffer failed to resolve an
    // address (e.g. arena exhaustion); that failure is never an optional absence.
    [[nodiscard]] inline VulkanMissingBufferSeverity ClassifyMissingVulkanBuffer(
        std::string_view shaderName, const VulkanShaderBinding& binding, bool occupantAbsent)
    {
        if (binding.BindingKind != VulkanShaderBinding::Kind::StorageBuffer)
            return VulkanMissingBufferSeverity::Warning;

        if (occupantAbsent && binding.Set == 0 && binding.ArrayCount == 1)
        {
            // Both static PBR vertex stages check InstanceData::LightmapScaleOffset
            // before indexing UV2. Unbaked meshes legitimately omit VAO stream 1.
            const bool lightmapShader = shaderName == "PBR_MultiLight" || shaderName == "PBR_GBuffer";
            const bool optionalLightmap = lightmapShader && binding.Binding == ShaderBindingLayout::SSBO_BONE_PULL &&
                                          binding.Name == "OloLightmapUVPull" && binding.Stages == VK_SHADER_STAGE_VERTEX_BIT;

            // CommandDispatch::UploadModelInstance clears the material generation
            // unless BindGPUSceneMaterialsIfNeeded succeeded. oloGPUSceneMaterial
            // returns on generation zero BEFORE indexing the table; unlinked draws
            // use the per-draw material UBO instead (issue #994 / #1190).
            const bool optionalMaterials = shaderName == "PBR_GBuffer" &&
                                           binding.Binding == ShaderBindingLayout::SSBO_INSTANCE_DRAW_INDIRECT &&
                                           binding.Name == "OloGPUSceneMaterials" && binding.Stages == VK_SHADER_STAGE_FRAGMENT_BIT;

            // Terrain VT feedback (issue #1390). b_TerrainVTFeedback is written only
            // by oloVTWriteFeedback, called only from oloVTResolveSurface, which both
            // terrain shaders call only under `useSplatmap && vtParams.Enabled > 0.5`.
            // Enabled comes from TerrainVirtualTexture::FillShaderParams, filled in
            // the same branch that supplies the feedback buffer, and DrawTerrainPatch
            // rejects a partial VT binding set. The VT bake and indirection kernels
            // declare other blocks at this binding; those require real buffers.
            const bool terrainShader = shaderName == "Terrain_PBR" || shaderName == "Terrain_GBuffer";
            const bool optionalVTFeedback = terrainShader && binding.Binding == ShaderBindingLayout::SSBO_TERRAIN_VT &&
                                            binding.Name == "TerrainVTFeedback" && binding.Stages == VK_SHADER_STAGE_FRAGMENT_BIT;
            if (optionalLightmap || optionalMaterials || optionalVTFeedback)
                return VulkanMissingBufferSeverity::Trace;
        }
        return VulkanMissingBufferSeverity::Error;
    }

    // A trace for a designed absence must not consume the later ERROR for an
    // address-resolution failure. Namespaces and declarations also stay distinct.
    [[nodiscard]] inline std::string MissingVulkanBufferDiagnosticKey(
        std::string_view shaderName, const VulkanShaderBinding& binding, VulkanMissingBufferSeverity severity)
    {
        return std::format("{}:{}:{}:{}:{}:{}", shaderName, binding.Set, binding.Binding,
                           static_cast<u32>(binding.BindingKind), binding.Name.ToView(), static_cast<u32>(severity));
    }
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
