// OLO_TEST_LAYER: L3

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Renderer/GPUScene/GPUScene.h"
#include "Rendering/ShaderHarness.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <spirv_cross/spirv_cross.hpp>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        struct MemberPin
        {
            const char* m_Name;
            u32 m_Offset;
        };

        // Every GPU-scene record's member table. A member added, renamed,
        // reordered or resized on either side fails the reflection check:
        // the C++ side through offsetof, the GLSL side through SPIRV-Cross.
        constexpr std::array<MemberPin, 10> kInstancePins{ {
            { "CurrentTransform", offsetof(GPUSceneInstance, CurrentTransform) },
            { "PreviousTransform", offsetof(GPUSceneInstance, PreviousTransform) },
            { "GeometryIndex", offsetof(GPUSceneInstance, GeometryIndex) },
            { "GeometryGeneration", offsetof(GPUSceneInstance, GeometryGeneration) },
            { "MaterialIndex", offsetof(GPUSceneInstance, MaterialIndex) },
            { "StableIndex", offsetof(GPUSceneInstance, StableIndex) },
            { "VisibilityMask", offsetof(GPUSceneInstance, VisibilityMask) },
            { "Flags", offsetof(GPUSceneInstance, Flags) },
            { "Generation", offsetof(GPUSceneInstance, Generation) },
            { "MaterialGeneration", offsetof(GPUSceneInstance, MaterialGeneration) },
        } };
        constexpr std::array<MemberPin, 14> kGeometryPins{ {
            { "VertexBufferIndex", offsetof(GPUSceneGeometry, VertexBufferIndex) },
            { "VertexBufferGeneration", offsetof(GPUSceneGeometry, VertexBufferGeneration) },
            { "IndexBufferIndex", offsetof(GPUSceneGeometry, IndexBufferIndex) },
            { "IndexBufferGeneration", offsetof(GPUSceneGeometry, IndexBufferGeneration) },
            { "VertexAddress", offsetof(GPUSceneGeometry, VertexAddress) },
            { "IndexAddress", offsetof(GPUSceneGeometry, IndexAddress) },
            { "VertexFormat", offsetof(GPUSceneGeometry, VertexFormat) },
            { "IndexFormat", offsetof(GPUSceneGeometry, IndexFormat) },
            { "FirstIndex", offsetof(GPUSceneGeometry, FirstIndex) },
            { "IndexCount", offsetof(GPUSceneGeometry, IndexCount) },
            { "BaseVertex", offsetof(GPUSceneGeometry, BaseVertex) },
            { "VertexCount", offsetof(GPUSceneGeometry, VertexCount) },
            { "Generation", offsetof(GPUSceneGeometry, Generation) },
            { "Flags", offsetof(GPUSceneGeometry, Flags) },
        } };
        constexpr std::array<MemberPin, 32> kMaterialPins{ {
            { "BaseColorFactor", offsetof(GPUSceneMaterial, BaseColorFactor) },
            { "EmissiveFactor", offsetof(GPUSceneMaterial, EmissiveFactor) },
            { "LegacyAmbient", offsetof(GPUSceneMaterial, LegacyAmbient) },
            { "LegacySpecular", offsetof(GPUSceneMaterial, LegacySpecular) },
            { "MetallicFactor", offsetof(GPUSceneMaterial, MetallicFactor) },
            { "RoughnessFactor", offsetof(GPUSceneMaterial, RoughnessFactor) },
            { "NormalScale", offsetof(GPUSceneMaterial, NormalScale) },
            { "OcclusionStrength", offsetof(GPUSceneMaterial, OcclusionStrength) },
            { "AlphaCutoff", offsetof(GPUSceneMaterial, AlphaCutoff) },
            { "AlphaMode", offsetof(GPUSceneMaterial, AlphaMode) },
            { "ClosureVersion", offsetof(GPUSceneMaterial, ClosureVersion) },
            { "Flags", offsetof(GPUSceneMaterial, Flags) },
            { "AlbedoTextureIndex", offsetof(GPUSceneMaterial, AlbedoTextureIndex) },
            { "AlbedoTextureGeneration", offsetof(GPUSceneMaterial, AlbedoTextureGeneration) },
            { "MetallicRoughnessTextureIndex", offsetof(GPUSceneMaterial, MetallicRoughnessTextureIndex) },
            { "MetallicRoughnessTextureGeneration", offsetof(GPUSceneMaterial, MetallicRoughnessTextureGeneration) },
            { "NormalTextureIndex", offsetof(GPUSceneMaterial, NormalTextureIndex) },
            { "NormalTextureGeneration", offsetof(GPUSceneMaterial, NormalTextureGeneration) },
            { "OcclusionTextureIndex", offsetof(GPUSceneMaterial, OcclusionTextureIndex) },
            { "OcclusionTextureGeneration", offsetof(GPUSceneMaterial, OcclusionTextureGeneration) },
            { "EmissiveTextureIndex", offsetof(GPUSceneMaterial, EmissiveTextureIndex) },
            { "EmissiveTextureGeneration", offsetof(GPUSceneMaterial, EmissiveTextureGeneration) },
            { "SpecularTextureIndex", offsetof(GPUSceneMaterial, SpecularTextureIndex) },
            { "SpecularTextureGeneration", offsetof(GPUSceneMaterial, SpecularTextureGeneration) },
            { "AlbedoHeapOffset", offsetof(GPUSceneMaterial, AlbedoHeapOffset) },
            { "MetallicRoughnessHeapOffset", offsetof(GPUSceneMaterial, MetallicRoughnessHeapOffset) },
            { "NormalHeapOffset", offsetof(GPUSceneMaterial, NormalHeapOffset) },
            { "OcclusionHeapOffset", offsetof(GPUSceneMaterial, OcclusionHeapOffset) },
            { "EmissiveHeapOffset", offsetof(GPUSceneMaterial, EmissiveHeapOffset) },
            { "SpecularHeapOffset", offsetof(GPUSceneMaterial, SpecularHeapOffset) },
            { "StableIndex", offsetof(GPUSceneMaterial, StableIndex) },
            { "Generation", offsetof(GPUSceneMaterial, Generation) },
        } };
        constexpr std::array<MemberPin, 8> kLightPins{ {
            { "PositionAndRange", offsetof(GPUSceneLight, PositionAndRange) },
            { "DirectionAndRadius", offsetof(GPUSceneLight, DirectionAndRadius) },
            { "ColorAndIntensity", offsetof(GPUSceneLight, ColorAndIntensity) },
            { "ShapeParams", offsetof(GPUSceneLight, ShapeParams) },
            { "Type", offsetof(GPUSceneLight, Type) },
            { "Flags", offsetof(GPUSceneLight, Flags) },
            { "StableIndex", offsetof(GPUSceneLight, StableIndex) },
            { "Generation", offsetof(GPUSceneLight, Generation) },
        } };
        constexpr std::array<MemberPin, 16> kEnvironmentPins{ {
            { "EnvironmentIndex", offsetof(GPUSceneEnvironment, EnvironmentIndex) },
            { "EnvironmentGeneration", offsetof(GPUSceneEnvironment, EnvironmentGeneration) },
            { "IrradianceIndex", offsetof(GPUSceneEnvironment, IrradianceIndex) },
            { "IrradianceGeneration", offsetof(GPUSceneEnvironment, IrradianceGeneration) },
            { "PrefilterIndex", offsetof(GPUSceneEnvironment, PrefilterIndex) },
            { "PrefilterGeneration", offsetof(GPUSceneEnvironment, PrefilterGeneration) },
            { "BRDFLutIndex", offsetof(GPUSceneEnvironment, BRDFLutIndex) },
            { "BRDFLutGeneration", offsetof(GPUSceneEnvironment, BRDFLutGeneration) },
            { "EnvironmentHeapOffset", offsetof(GPUSceneEnvironment, EnvironmentHeapOffset) },
            { "IrradianceHeapOffset", offsetof(GPUSceneEnvironment, IrradianceHeapOffset) },
            { "PrefilterHeapOffset", offsetof(GPUSceneEnvironment, PrefilterHeapOffset) },
            { "BRDFLutHeapOffset", offsetof(GPUSceneEnvironment, BRDFLutHeapOffset) },
            { "Intensity", offsetof(GPUSceneEnvironment, Intensity) },
            { "Flags", offsetof(GPUSceneEnvironment, Flags) },
            { "StableIndex", offsetof(GPUSceneEnvironment, StableIndex) },
            { "Generation", offsetof(GPUSceneEnvironment, Generation) },
        } };

        // The test kernel touches one member of every record so no block is
        // dead-stripped before reflection. Its own output takes a binding the
        // include does not alias (19 = SSBO_AUTO_EXPOSURE_HISTOGRAM, a leaf the
        // auto-exposure kernels rebind per dispatch).
        constexpr const char* kSource = R"glsl(
#version 450
#include "include/GPUScene.glsl"
layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;
layout(std430, binding = 19) writeonly buffer GPUSceneTestOutput
{
    uint Value;
} g_Output;
void main()
{
    g_Output.Value = g_GPUSceneInstances[0].Generation + g_GPUSceneGeometries[0].IndexCount +
                     g_GPUSceneMaterials[0].Generation + g_GPUSceneLights[0].Type +
                     g_GPUSceneEnvironments[0].Flags;
}
)glsl";
    } // namespace

    TEST(GPUScene, ShaderStorageLayoutsMatchCppRecords)
    {
        namespace SH = ShaderHarness;

        const auto root = SH::ResolveShaderRoot();
        ASSERT_FALSE(root.empty());

        shaderc::Compiler compiler;
        ASSERT_TRUE(compiler.IsValid());
        const auto result = SH::CompileStageToSpv(root / "tests/GPUSceneContract.comp", kSource,
                                                  shaderc_glsl_compute_shader, root, compiler,
                                                  /*generateDebugInfo=*/true);
        ASSERT_EQ(result.GetCompilationStatus(), shaderc_compilation_status_success)
            << result.GetErrorMessage();

        spirv_cross::Compiler reflection(std::vector<u32>(result.cbegin(), result.cend()));
        const auto resources = reflection.get_shader_resources();

        auto checkRecord = [&](u32 binding, const auto& pins, sizet cppStride)
        {
            const spirv_cross::Resource* resource = nullptr;
            for (const auto& candidate : resources.storage_buffers)
            {
                if (reflection.get_decoration(candidate.id, spv::DecorationBinding) == binding)
                {
                    resource = &candidate;
                    break;
                }
            }
            ASSERT_NE(resource, nullptr) << "missing GPU-scene storage buffer binding " << binding;

            const auto& blockType = reflection.get_type(resource->base_type_id);
            ASSERT_EQ(blockType.member_types.size(), 1u);
            const sizet stride = reflection.get_declared_struct_size_runtime_array(blockType, 1) -
                                 reflection.get_declared_struct_size_runtime_array(blockType, 0);
            EXPECT_EQ(stride, cppStride) << "std430 runtime-array stride mismatch at binding " << binding;

            const auto& arrayType = reflection.get_type(blockType.member_types[0]);
            ASSERT_NE(arrayType.parent_type, 0u);
            const auto recordTypeId = arrayType.parent_type;
            const auto& recordType = reflection.get_type(recordTypeId);
            const auto pinCount = pins.size();
            ASSERT_EQ(recordType.member_types.size(), pinCount)
                << "member-count mismatch at binding " << binding << ": one side of the mirror gained or lost a field";
            EXPECT_EQ(reflection.get_declared_struct_size(recordType), cppStride);

            for (u32 member = 0; member < pinCount; ++member)
            {
                EXPECT_EQ(reflection.get_member_name(recordTypeId, member), pins[member].m_Name)
                    << "member-order mismatch at binding " << binding << ", member " << member;
                EXPECT_EQ(reflection.get_member_decoration(recordTypeId, member, spv::DecorationOffset),
                          pins[member].m_Offset)
                    << pins[member].m_Name << " offset mismatch at binding " << binding;
            }
        };

        checkRecord(GPUSceneBindingLayout::Instances, kInstancePins, sizeof(GPUSceneInstance));
        checkRecord(GPUSceneBindingLayout::Geometries, kGeometryPins, sizeof(GPUSceneGeometry));
        checkRecord(GPUSceneBindingLayout::Materials, kMaterialPins, sizeof(GPUSceneMaterial));
        checkRecord(GPUSceneBindingLayout::Lights, kLightPins, sizeof(GPUSceneLight));
        checkRecord(GPUSceneBindingLayout::Environments, kEnvironmentPins, sizeof(GPUSceneEnvironment));
    }

    // The GPU-scene buffers alias the Forward+ per-type light slots (9/10) and
    // the instance-cull trio (15/16/17) because the portable SSBO namespace is
    // full. Inside one shader that aliasing is a real collision on both
    // backends. The check is on the NUMBERS, over each shader's whole include
    // closure: any shader that reaches GPUScene.glsl must not, itself or
    // through another include, declare a storage block at an aliased binding.
    TEST(GPUScene, NoShaderDeclaresAnAliasedStorageBindingTogetherWithGPUScene)
    {
        namespace fs = std::filesystem;
        namespace SH = ShaderHarness;

        const fs::path root = SH::ResolveShaderRoot();
        ASSERT_FALSE(root.empty());
        const fs::path gpuSceneInclude = (root / "include" / "GPUScene.glsl").lexically_normal();
        ASSERT_TRUE(fs::exists(gpuSceneInclude));

        constexpr std::array<u32, 5> kAliased{ GPUSceneBindingLayout::Instances, GPUSceneBindingLayout::Geometries,
                                               GPUSceneBindingLayout::Materials, GPUSceneBindingLayout::Lights,
                                               GPUSceneBindingLayout::Environments };
        const auto declaresAliasedBinding = [&](const fs::path& file)
        {
            for (const u32 binding : SH::DeclaredStorageBufferBindings(SH::ReadWholeFile(file)))
            {
                if (std::ranges::find(kAliased, binding) != kAliased.end())
                {
                    return true;
                }
            }
            return false;
        };

        // Anti-vacuity: the scanner must see the declarations it exists to catch,
        // in the include that owns them and in the families they alias.
        EXPECT_TRUE(declaresAliasedBinding(gpuSceneInclude));
        for (const char* fixture : { "ForwardPlusCommon.glsl", "InstanceBlock_Vertex.glsl" })
        {
            const fs::path path = root / "include" / fixture;
            ASSERT_TRUE(fs::exists(path)) << "anti-vacuity fixture moved: " << path.generic_string();
            EXPECT_TRUE(declaresAliasedBinding(path)) << fixture << " no longer declares an aliased binding";
        }

        u32 scanned = 0;
        std::string violations;
        for (const fs::path& path : SH::EnumerateShaderSources(root))
        {
            const fs::path shader = path.lexically_normal();
            if (shader == gpuSceneInclude)
            {
                continue;
            }
            ++scanned;
            const std::vector<fs::path> closure = SH::IncludeClosure(root, shader);
            if (std::ranges::find(closure, gpuSceneInclude) == closure.end())
            {
                continue;
            }
            bool collides = declaresAliasedBinding(shader);
            for (const fs::path& dependency : closure)
            {
                collides = collides || (dependency != gpuSceneInclude && declaresAliasedBinding(dependency));
            }
            if (collides)
            {
                violations += (violations.empty() ? "" : ", ") + shader.generic_string();
            }
        }

        EXPECT_GT(scanned, 0u);
        EXPECT_TRUE(violations.empty())
            << "a shader reaches GPUScene.glsl and also declares an aliased storage binding: " << violations;
    }
} // namespace OloEngine::Tests
