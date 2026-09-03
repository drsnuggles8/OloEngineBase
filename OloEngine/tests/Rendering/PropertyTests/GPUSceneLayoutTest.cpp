// OLO_TEST_LAYER: L3

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Renderer/GPUScene/GPUScene.h"
#include "Rendering/ShaderHarness.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <map>
#include <sstream>
#include <string_view>
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
#include "include/GPUSceneInstances.glsl"
#include "include/GPUSceneGeometries.glsl"
#include "include/GPUSceneMaterials.glsl"
#include "include/GPUSceneLights.glsl"
#include "include/GPUSceneEnvironments.glsl"
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
    // full. Since #994 GPUScene.glsl declares no block itself and each kind has
    // its own declaration file, so the rule is the honest one rather than a
    // blanket ban: if a shader takes a GPU Scene table, nothing else in that
    // shader may declare a block at the slot it took.
    //
    // Everything hangs on the block NAME. A binding legitimately carries the
    // same block twice — InstanceBlock.glsl and InstanceBlock_Vertex.glsl both
    // declare `InstanceBuffer` at 15, one per stage — and a shader may declare
    // an unrelated block at an aliased number as long as it does not also take
    // the GPU Scene table there. That is what lets PBR_GBuffer.glsl read the
    // canonical materials at 17 while it still reads per-draw InstanceData at
    // 15, and it is why the check is scoped to the five aliased numbers rather
    // than to every binding: the scan is text-level and cannot tell two stages
    // of one file apart, so a wider rule would flag legal per-stage pairs.
    TEST(GPUScene, NoShaderDeclaresAnotherBlockAtASlotItTakesForGPUScene)
    {
        namespace fs = std::filesystem;
        namespace SH = ShaderHarness;

        const fs::path root = SH::ResolveShaderRoot();
        ASSERT_FALSE(root.empty());

        constexpr std::array<u32, 5> kAliased{ GPUSceneBindingLayout::Instances, GPUSceneBindingLayout::Geometries,
                                               GPUSceneBindingLayout::Materials, GPUSceneBindingLayout::Lights,
                                               GPUSceneBindingLayout::Environments };
        // The canonical block name per aliased slot, taken from the per-kind
        // declaration files themselves so a rename there cannot silently take
        // this check out of the loop.
        const auto blocksIn = [&](const std::string& relative)
        {
            const fs::path path = root / "include" / relative;
            EXPECT_TRUE(fs::exists(path)) << "declaration file moved: " << path.generic_string();
            return SH::DeclaredStorageBufferBlocks(SH::ReadWholeFile(path));
        };
        std::map<u32, std::string> canonical;
        for (const char* kind : { "Instances", "Geometries", "Materials", "Lights", "Environments" })
        {
            const std::vector<SH::DeclaredStorageBlock> blocks = blocksIn(std::string("GPUScene") + kind + ".glsl");
            ASSERT_EQ(blocks.size(), 1u) << "GPUScene" << kind << ".glsl must declare exactly one storage block";
            EXPECT_TRUE(std::ranges::find(kAliased, blocks.front().m_Binding) != kAliased.end())
                << "GPUScene" << kind << ".glsl declares an unexpected binding";
            canonical.emplace(blocks.front().m_Binding, blocks.front().m_Name);
        }
        ASSERT_EQ(canonical.size(), kAliased.size()) << "two GPU Scene kinds landed on one slot";

        // GPUScene.glsl is the record contract only: it must declare nothing,
        // or a consumer could not take one kind without taking all five.
        EXPECT_TRUE(SH::DeclaredStorageBufferBlocks(SH::ReadWholeFile(root / "include" / "GPUScene.glsl")).empty())
            << "include/GPUScene.glsl declares a storage block again; the per-kind split (#994) is what lets a "
               "raster shader take materials without also taking the instance slot";

        // The violation this exists to catch, constructed: the canonical
        // instance table and InstanceBlock_Vertex.glsl both want slot 15.
        const std::vector<SH::DeclaredStorageBlock> instanceBlock = blocksIn("InstanceBlock_Vertex.glsl");
        EXPECT_TRUE(std::ranges::any_of(instanceBlock,
                                        [&](const SH::DeclaredStorageBlock& block)
                                        {
                                            return block.m_Binding == GPUSceneBindingLayout::Instances &&
                                                   block.m_Name != canonical.at(GPUSceneBindingLayout::Instances);
                                        }))
            << "anti-vacuity: InstanceBlock_Vertex.glsl no longer contends for the instance slot, so a shader that "
               "took the canonical instances alongside it would no longer be caught here";

        u32 scanned = 0;
        u32 tookATable = 0;
        std::string violations;
        for (const fs::path& path : SH::EnumerateShaderSources(root))
        {
            const fs::path shader = path.lexically_normal();
            ++scanned;

            std::vector<SH::DeclaredStorageBlock> blocks = SH::DeclaredStorageBufferBlocks(SH::ReadWholeFile(shader));
            for (const fs::path& dependency : SH::IncludeClosure(root, shader))
            {
                if (dependency.lexically_normal() == shader)
                {
                    continue;
                }
                const std::vector<SH::DeclaredStorageBlock> fromInclude =
                    SH::DeclaredStorageBufferBlocks(SH::ReadWholeFile(dependency));
                blocks.insert(blocks.end(), fromInclude.begin(), fromInclude.end());
            }

            bool takesATable = false;
            for (const auto& [binding, canonicalName] : canonical)
            {
                const bool takesThisTable = std::ranges::any_of(
                    blocks, [&, binding = binding](const SH::DeclaredStorageBlock& block)
                    { return block.m_Binding == binding && block.m_Name == canonicalName; });
                if (!takesThisTable)
                {
                    continue;
                }
                takesATable = true;
                for (const SH::DeclaredStorageBlock& block : blocks)
                {
                    if (block.m_Binding == binding && block.m_Name != canonicalName)
                    {
                        violations += (violations.empty() ? "" : ", ") + shader.generic_string() + " binding " +
                                      std::to_string(binding) + " (" + canonicalName + " vs " + block.m_Name + ")";
                    }
                }
            }
            tookATable += takesATable ? 1u : 0u;
        }

        EXPECT_GT(scanned, 0u);
        EXPECT_GT(tookATable, 0u) << "no shader takes a GPU Scene table at all — the raster migration (#994) is the "
                                     "first consumer, and PBR_GBuffer.glsl should be one";
        EXPECT_TRUE(violations.empty())
            << "a shader takes a GPU Scene table and declares another block at the same slot: " << violations;
    }

    // A GPU Scene consumer may not ask a storage buffer for its length.
    //
    // `.length()` compiles to OpArrayLength, and the Vulkan RHI maps these
    // buffers through VK_DESCRIPTOR_MAPPING_SOURCE_INDIRECT_ADDRESS_EXT, where
    // a buffer's length is not knowable. vkCreateGraphicsPipelines rejects the
    // whole pipeline, and the editor then dereferences the null pipeline and
    // crashes — on Vulkan only, at pipeline-creation time, long after the SPIR-V
    // compiled cleanly. Nothing on the OpenGL side notices, which is why this is
    // a test and not a review habit.
    //
    // The generation is the bound instead: a non-zero generation only comes from
    // a link the registry resolved this frame, and Upload grows the buffer to
    // hold every committed record before any draw runs.
    TEST(GPUScene, NoGPUSceneIncludeAsksAStorageBufferForItsLength)
    {
        namespace fs = std::filesystem;
        namespace SH = ShaderHarness;

        const fs::path root = SH::ResolveShaderRoot();
        ASSERT_FALSE(root.empty());
        const fs::path includes = root / "include";
        ASSERT_TRUE(fs::exists(includes));

        u32 scanned = 0;
        std::string violations;
        for (const fs::directory_entry& entry : fs::directory_iterator(includes))
        {
            const std::string name = entry.path().filename().string();
            if (!entry.is_regular_file() || !name.starts_with("GPUScene"))
            {
                continue;
            }
            ++scanned;
            std::string source = SH::ReadWholeFile(entry.path());
            // Strip line comments so the explanation of this rule does not trip it.
            std::string code;
            code.reserve(source.size());
            std::istringstream lines(source);
            for (std::string line; std::getline(lines, line);)
            {
                const auto comment = line.find("//");
                code.append(comment == std::string::npos ? line : line.substr(0, comment));
                code.push_back('\n');
            }
            if (code.find(".length()") != std::string::npos)
            {
                violations += (violations.empty() ? "" : ", ") + name;
            }
        }

        EXPECT_GT(scanned, 0u) << "no GPUScene*.glsl includes found — the scan is looking in the wrong place";
        EXPECT_TRUE(violations.empty())
            << "a GPU Scene include calls .length() on a storage buffer; that is OpArrayLength, which the Vulkan "
               "indirect-address descriptor mapping rejects at pipeline creation. Validate the record's generation "
               "instead. Offending: "
            << violations;
    }

} // namespace OloEngine::Tests
