// OLO_TEST_LAYER: plumbing
#include "OloEngine/Core/Base.h"

#include <gtest/gtest.h>

#if OLO_WITH_VULKAN

#include "Platform/Vulkan/VulkanBufferBindingDiagnostics.h"
#include "Platform/Vulkan/VulkanWarnOnce.h"
#include "ShaderHarness.h"

#include <spirv_cross/spirv_cross.hpp>

#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        using Severity = VulkanMissingBufferSeverity;
        using Kind = VulkanShaderBinding::Kind;

        VulkanShaderBinding LightmapBinding()
        {
            return { .Binding = ShaderBindingLayout::SSBO_BONE_PULL,
                     .BindingKind = Kind::StorageBuffer,
                     .Stages = VK_SHADER_STAGE_VERTEX_BIT,
                     .Name = "OloLightmapUVPull" };
        }

        VulkanShaderBinding MaterialBinding()
        {
            return { .Binding = ShaderBindingLayout::SSBO_INSTANCE_DRAW_INDIRECT,
                     .BindingKind = Kind::StorageBuffer,
                     .Stages = VK_SHADER_STAGE_FRAGMENT_BIT,
                     .Name = "OloGPUSceneMaterials" };
        }

        VulkanShaderBinding TerrainVTFeedbackBinding()
        {
            return { .Binding = ShaderBindingLayout::SSBO_TERRAIN_VT,
                     .BindingKind = Kind::StorageBuffer,
                     .Stages = VK_SHADER_STAGE_FRAGMENT_BIT,
                     .Name = "TerrainVTFeedback" };
        }

        VkShaderStageFlags StageBit(shaderc_shader_kind kind)
        {
            switch (kind)
            {
                case shaderc_glsl_vertex_shader:
                    return VK_SHADER_STAGE_VERTEX_BIT;
                case shaderc_glsl_tess_control_shader:
                    return VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
                case shaderc_glsl_tess_evaluation_shader:
                    return VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
                case shaderc_glsl_fragment_shader:
                    return VK_SHADER_STAGE_FRAGMENT_BIT;
                case shaderc_glsl_compute_shader:
                    return VK_SHADER_STAGE_COMPUTE_BIT;
                default:
                    return 0;
            }
        }

        // Every storage block a production shader file declares, reflected
        // per stage from the Vulkan-backend SPIR-V.
        std::vector<VulkanShaderBinding> ReflectStorageBlocks(const std::string& relativePath)
        {
            namespace SH = ShaderHarness;
            std::vector<VulkanShaderBinding> out;
            const auto root = SH::ResolveShaderRoot();
            EXPECT_FALSE(root.empty());
            if (root.empty())
                return out;
            shaderc::Compiler compiler;
            const auto path = root / relativePath;
            for (const auto& [kind, source] : SH::SplitStages(SH::ReadWholeFile(path)))
            {
                const auto result = SH::CompileVulkanBackendStageToSpv(path, source, kind, root, compiler);
                EXPECT_EQ(result.GetCompilationStatus(), shaderc_compilation_status_success)
                    << relativePath << ": " << result.GetErrorMessage();
                if (result.GetCompilationStatus() != shaderc_compilation_status_success)
                    continue;
                const spirv_cross::Compiler reflected(std::vector<u32>{ result.cbegin(), result.cend() });
                for (const auto& resource : reflected.get_shader_resources().storage_buffers)
                {
                    out.push_back({ .Set = reflected.get_decoration(resource.id, spv::DecorationDescriptorSet),
                                    .Binding = reflected.get_decoration(resource.id, spv::DecorationBinding),
                                    .BindingKind = Kind::StorageBuffer,
                                    .Stages = StageBit(kind),
                                    .Name = resource.name });
                }
            }
            return out;
        }
    } // namespace

    TEST(VulkanBufferBindingDiagnostics, OnlyReviewedDeclarationsAreOptional)
    {
        EXPECT_EQ(ClassifyMissingVulkanBuffer("PBR_GBuffer", LightmapBinding(), true), Severity::Trace);
        EXPECT_EQ(ClassifyMissingVulkanBuffer("PBR_MultiLight", LightmapBinding(), true), Severity::Trace);
        EXPECT_EQ(ClassifyMissingVulkanBuffer("PBR_GBuffer", MaterialBinding(), true), Severity::Trace);
        EXPECT_EQ(ClassifyMissingVulkanBuffer("PBR_MultiLight", MaterialBinding(), true), Severity::Error);
        EXPECT_EQ(ClassifyMissingVulkanBuffer("UnknownShader", LightmapBinding(), true), Severity::Error);
        EXPECT_EQ(ClassifyMissingVulkanBuffer("PBR_GBuffer_Skinned", LightmapBinding(), true), Severity::Error);
    }

    TEST(VulkanBufferBindingDiagnostics, AliasedSlotsAndOtherNamespacesStayRequired)
    {
        auto binding = LightmapBinding();
        binding.Name = "OloBonePull";
        EXPECT_EQ(ClassifyMissingVulkanBuffer("PBR_GBuffer", binding, true), Severity::Error);
        binding = LightmapBinding();
        binding.Binding = ShaderBindingLayout::SSBO_VERTEX_PULL;
        binding.Name = "OloVertexPull";
        EXPECT_EQ(ClassifyMissingVulkanBuffer("PBR_GBuffer", binding, true), Severity::Error);
        binding = MaterialBinding();
        binding.Name = "DrawIndirectArgs";
        EXPECT_EQ(ClassifyMissingVulkanBuffer("PBR_GBuffer", binding, true), Severity::Error);
        binding = MaterialBinding();
        binding.BindingKind = Kind::UniformBuffer;
        EXPECT_EQ(ClassifyMissingVulkanBuffer("PBR_GBuffer", binding, true), Severity::Warning);
        binding.Binding = ShaderBindingLayout::UBO_VIRTUAL_SHADOW_DRAW;
        binding.Name = "VirtualShadowPass";
        EXPECT_EQ(ClassifyMissingVulkanBuffer("PBR_GBuffer", binding, true), Severity::Warning);
    }

    TEST(VulkanBufferBindingDiagnostics, DifferentSetsStagesAndArraysAreNotReviewed)
    {
        auto binding = MaterialBinding();
        binding.Set = 1;
        EXPECT_EQ(ClassifyMissingVulkanBuffer("PBR_GBuffer", binding, true), Severity::Error);
        binding = MaterialBinding();
        binding.Stages |= VK_SHADER_STAGE_COMPUTE_BIT;
        EXPECT_EQ(ClassifyMissingVulkanBuffer("PBR_GBuffer", binding, true), Severity::Error);
        binding = LightmapBinding();
        binding.ArrayCount = 2;
        EXPECT_EQ(ClassifyMissingVulkanBuffer("PBR_GBuffer", binding, true), Severity::Error);
    }

    TEST(VulkanBufferBindingDiagnostics, PublishedBufferAddressFailuresRemainErrors)
    {
        EXPECT_EQ(ClassifyMissingVulkanBuffer("PBR_GBuffer", LightmapBinding(), false), Severity::Error);
        EXPECT_EQ(ClassifyMissingVulkanBuffer("PBR_GBuffer", MaterialBinding(), false), Severity::Error);
    }

    TEST(VulkanBufferBindingDiagnostics, OptionalTraceCannotConsumeLaterErrorOrUniformWarning)
    {
        VulkanWarnOnceSet warned;
        auto binding = MaterialBinding();
        const auto traceKey = MissingVulkanBufferDiagnosticKey("PBR_GBuffer", binding, Severity::Trace);
        EXPECT_TRUE(warned.Insert(traceKey));
        EXPECT_FALSE(warned.Insert(traceKey));
        const auto errorKey = MissingVulkanBufferDiagnosticKey("PBR_GBuffer", binding, Severity::Error);
        EXPECT_TRUE(warned.Insert(errorKey));
        EXPECT_FALSE(warned.Insert(errorKey));
        binding.BindingKind = Kind::UniformBuffer;
        EXPECT_TRUE(warned.Insert(MissingVulkanBufferDiagnosticKey("PBR_GBuffer", binding, Severity::Warning)));
    }

    // Reflect production SPIR-V rather than assuming its retained block names or
    // stages. This exercises the real shader declarations without needing a GPU;
    // the live-editor pixel control separately covers publication and execution.
    TEST(VulkanBufferBindingDiagnostics, ProductionReflectionMatchesTheThreeOptionalDeclarations)
    {
        namespace SH = ShaderHarness;
        const auto root = SH::ResolveShaderRoot();
        ASSERT_FALSE(root.empty());
        shaderc::Compiler compiler;
        u32 optionalCount = 0;
        for (const std::string shaderName : { "PBR_MultiLight", "PBR_GBuffer" })
        {
            const auto path = root / (shaderName + ".glsl");
            const auto stages = SH::SplitStages(SH::ReadWholeFile(path));
            ASSERT_EQ(stages.size(), 2u);
            for (const auto& [kind, source] : stages)
            {
                SCOPED_TRACE(shaderName + ":" + std::to_string(kind));
                const auto result = SH::CompileVulkanBackendStageToSpv(path, source, kind, root, compiler);
                ASSERT_EQ(result.GetCompilationStatus(), shaderc_compilation_status_success) << result.GetErrorMessage();
                const spirv_cross::Compiler reflected(std::vector<u32>{ result.cbegin(), result.cend() });
                for (const auto& resource : reflected.get_shader_resources().storage_buffers)
                {
                    const VulkanShaderBinding binding{
                        .Set = reflected.get_decoration(resource.id, spv::DecorationDescriptorSet),
                        .Binding = reflected.get_decoration(resource.id, spv::DecorationBinding),
                        .BindingKind = Kind::StorageBuffer,
                        .Stages = static_cast<VkShaderStageFlags>(kind == shaderc_glsl_vertex_shader ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT),
                        .Name = resource.name
                    };
                    if (ClassifyMissingVulkanBuffer(shaderName, binding, true) == Severity::Trace)
                        ++optionalCount;
                }
            }
        }
        EXPECT_EQ(optionalCount, 3u);
    }

    // Issue #1390: with the virtual texture off nothing publishes the terrain
    // feedback buffer, and the only write to it sits behind the VT gate.
    TEST(VulkanBufferBindingDiagnostics, TerrainVTFeedbackIsOptionalOnlyInTheTerrainFragmentStages)
    {
        EXPECT_EQ(ClassifyMissingVulkanBuffer("Terrain_PBR", TerrainVTFeedbackBinding(), true), Severity::Trace);
        EXPECT_EQ(ClassifyMissingVulkanBuffer("Terrain_GBuffer", TerrainVTFeedbackBinding(), true), Severity::Trace);
        EXPECT_EQ(ClassifyMissingVulkanBuffer("Terrain_PBR", TerrainVTFeedbackBinding(), false), Severity::Error)
            << "a published feedback buffer whose address failed to resolve is never an optional absence";
        EXPECT_EQ(ClassifyMissingVulkanBuffer("PBR_GBuffer", TerrainVTFeedbackBinding(), true), Severity::Error);
        EXPECT_EQ(ClassifyMissingVulkanBuffer("TerrainVTTileBake", TerrainVTFeedbackBinding(), true), Severity::Error);

        // The VT kernels' own blocks at binding 79 need real buffers, even under
        // a terrain shader name.
        for (const char* blockName : { "TerrainVTBake", "TerrainVTIndirectionUpdates" })
        {
            auto binding = TerrainVTFeedbackBinding();
            binding.Name = blockName;
            EXPECT_EQ(ClassifyMissingVulkanBuffer("Terrain_PBR", binding, true), Severity::Error) << blockName;
        }
        auto binding = TerrainVTFeedbackBinding();
        binding.Stages = VK_SHADER_STAGE_COMPUTE_BIT;
        EXPECT_EQ(ClassifyMissingVulkanBuffer("Terrain_PBR", binding, true), Severity::Error);
        binding = TerrainVTFeedbackBinding();
        binding.Binding = ShaderBindingLayout::SSBO_TERRAIN_VISIBLE_NODES;
        EXPECT_EQ(ClassifyMissingVulkanBuffer("Terrain_PBR", binding, true), Severity::Error);
        binding = TerrainVTFeedbackBinding();
        binding.Set = 1;
        EXPECT_EQ(ClassifyMissingVulkanBuffer("Terrain_GBuffer", binding, true), Severity::Error);
        binding = TerrainVTFeedbackBinding();
        binding.BindingKind = Kind::UniformBuffer;
        EXPECT_EQ(ClassifyMissingVulkanBuffer("Terrain_GBuffer", binding, true), Severity::Warning);
    }

    // The real terrain SPIR-V: exactly one optional block per shader, and it is
    // the feedback buffer in the fragment stage. Everything else the four stages
    // declare (vertex pull, visible nodes, ...) stays required.
    TEST(VulkanBufferBindingDiagnostics, ProductionTerrainReflectionHasExactlyTheFeedbackBlockOptional)
    {
        for (const std::string shaderName : { "Terrain_PBR", "Terrain_GBuffer" })
        {
            SCOPED_TRACE(shaderName);
            const auto blocks = ReflectStorageBlocks(shaderName + ".glsl");
            u32 optionalCount = 0;
            bool sawFeedback = false;
            for (const auto& binding : blocks)
            {
                const bool isFeedback = binding.Name == "TerrainVTFeedback";
                sawFeedback |= isFeedback;
                const auto severity = ClassifyMissingVulkanBuffer(shaderName, binding, true);
                if (severity == Severity::Trace)
                {
                    ++optionalCount;
                    EXPECT_TRUE(isFeedback) << "unexpected optional block " << binding.Name.ToView();
                    EXPECT_EQ(binding.Binding, ShaderBindingLayout::SSBO_TERRAIN_VT);
                    EXPECT_EQ(binding.Stages, static_cast<VkShaderStageFlags>(VK_SHADER_STAGE_FRAGMENT_BIT));
                }
            }
            EXPECT_TRUE(sawFeedback) << "the fragment stage no longer declares TerrainVTFeedback";
            EXPECT_EQ(optionalCount, 1u);
        }
    }

    // Negative control: the VT bake and indirection kernels declare their own
    // blocks at the same binding and read them unconditionally.
    TEST(VulkanBufferBindingDiagnostics, ProductionVTComputeKernelsKeepBinding79Required)
    {
        u32 binding79Blocks = 0;
        for (const std::string kernel :
             { "TerrainVTTileBake", "TerrainVTCompressBC7", "TerrainVTIndirectionFill", "TerrainVTIndirectionWrite" })
        {
            SCOPED_TRACE(kernel);
            for (const auto& binding : ReflectStorageBlocks("compute/" + kernel + ".comp"))
            {
                if (binding.Binding != ShaderBindingLayout::SSBO_TERRAIN_VT)
                    continue;
                ++binding79Blocks;
                EXPECT_EQ(ClassifyMissingVulkanBuffer(kernel, binding, true), Severity::Error) << binding.Name.ToView();
                EXPECT_EQ(ClassifyMissingVulkanBuffer("Terrain_PBR", binding, true), Severity::Error)
                    << binding.Name.ToView();
            }
        }
        EXPECT_EQ(binding79Blocks, 4u);
    }
} // namespace OloEngine::Tests

#else

TEST(VulkanBufferBindingDiagnostics, SkipsWhenNotCompiledIn)
{
    GTEST_SKIP() << "OLO_WITH_VULKAN is off.";
}

#endif
