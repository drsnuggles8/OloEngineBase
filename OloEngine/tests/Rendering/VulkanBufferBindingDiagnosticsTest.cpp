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
} // namespace OloEngine::Tests

#else

TEST(VulkanBufferBindingDiagnostics, SkipsWhenNotCompiledIn)
{
    GTEST_SKIP() << "OLO_WITH_VULKAN is off.";
}

#endif
