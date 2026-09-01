// OLO_TEST_LAYER: L3

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Renderer/GPUScene/GPUScene.h"
#include "Rendering/ShaderHarness.h"

#include <array>
#include <cstddef>
#include <spirv_cross/spirv_cross.hpp>

namespace OloEngine::Tests
{
    TEST(GPUScene, ShaderStorageLayoutsMatchCppRecords)
    {
        namespace SH = ShaderHarness;

        struct MemberPin
        {
            const char* m_Name;
            u32 m_Offset;
        };

        static constexpr std::array<MemberPin, 10> kInstancePins{ {
            { "CurrentTransform", offsetof(GPUSceneInstance, CurrentTransform) },
            { "PreviousTransform", offsetof(GPUSceneInstance, PreviousTransform) },
            { "GeometryIndex", offsetof(GPUSceneInstance, GeometryIndex) },
            { "GeometryGeneration", offsetof(GPUSceneInstance, GeometryGeneration) },
            { "MaterialIndex", offsetof(GPUSceneInstance, MaterialIndex) },
            { "StableIndex", offsetof(GPUSceneInstance, StableIndex) },
            { "VisibilityMask", offsetof(GPUSceneInstance, VisibilityMask) },
            { "Flags", offsetof(GPUSceneInstance, Flags) },
            { "Generation", offsetof(GPUSceneInstance, Generation) },
            { "Pad0", offsetof(GPUSceneInstance, Pad0) },
        } };
        static constexpr std::array<MemberPin, 14> kGeometryPins{ {
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

        const auto root = SH::ResolveShaderRoot();
        ASSERT_FALSE(root.empty());

        static constexpr const char* kSource = R"glsl(
#version 450
#include "include/GPUScene.glsl"
layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;
layout(std430, binding = 17) writeonly buffer GPUSceneTestOutput
{
    uint Value;
} g_Output;
void main()
{
    g_Output.Value = g_GPUSceneInstances[0].Generation + g_GPUSceneGeometries[0].IndexCount;
}
)glsl";

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
            ASSERT_EQ(recordType.member_types.size(), pinCount);
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
    }

} // namespace OloEngine::Tests
