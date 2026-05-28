// =============================================================================
// ShaderUBOSizeConsistencyTest.cpp
//
// Catches the classic ABI-drift class of OloEditor breakage: a developer
// adds (or reorders) a field in a C++ `UBOStructures::*UBO` struct but
// forgets to update the matching GLSL `layout(std140) uniform` block in
// every shader that declares it. The runtime then uploads the larger
// C++ struct into a smaller GLSL block, and shaders read past the end of
// the GLSL declaration into garbage memory layout-wise — or, worse, the
// GLSL block is *larger* than the C++ side and shaders read undefined
// trailing bytes.
//
// Invariant
// ---------
//   For every shader that declares a UBO block known to map to a C++
//   `UBOStructures::*UBO` struct, the SPIR-V declared block size must be
//   <= sizeof(C++ struct).
//
// std140 explicitly allows shaders to declare a *prefix* of a buffer
// (e.g. legacy `CameraMatrices` blocks that stop at `_padding0` and don't
// include the trailing `u_PrevViewProjection` matrix). We accept that.
// What we reject is GLSL declaring more bytes than the C++ side actually
// uploads — that's a guaranteed out-of-bounds read.
//
// Some production block names are aliased (Camera / CameraMatrices both
// resolve to `UBOStructures::CameraUBO`; BoneMatrices / AnimationMatrices
// both resolve to `UBOStructures::AnimationUBO`). The lookup table below
// enumerates every known alias.
//
// Classification: shaderpipe.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>
#include <spirv_cross/spirv_cross.hpp>

#include "ShaderHarness.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <array>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;
        namespace SH = ShaderHarness;

        struct KnownBlock
        {
            std::string_view GlslName;
            u32 CppSize;
        };

        // Block names that production shaders use, mapped to their
        // canonical C++ struct size. Aliases are listed explicitly.
        const std::array<KnownBlock, 21> kKnownBlocks = { {
            { "CameraMatrices", sizeof(UBOStructures::CameraUBO) },
            { "Camera", sizeof(UBOStructures::CameraUBO) },
            { "LightProperties", sizeof(UBOStructures::LightUBO) },
            { "MultiLightBuffer", sizeof(UBOStructures::MultiLightUBO) },
            { "MultiLightData", sizeof(UBOStructures::MultiLightUBO) },
            { "MaterialProperties", sizeof(UBOStructures::MaterialUBO) },
            { "PBRMaterialProperties", sizeof(UBOStructures::PBRMaterialUBO) },
            { "ModelMatrices", sizeof(UBOStructures::ModelUBO) },
            { "MeshInstanceData", sizeof(UBOStructures::ModelUBO) },
            { "AnimationMatrices", sizeof(UBOStructures::AnimationUBO) },
            { "BoneMatrices", sizeof(UBOStructures::AnimationUBO) },
            { "ShadowData", sizeof(UBOStructures::ShadowUBO) },
            { "TerrainParams", sizeof(UBOStructures::TerrainUBO) },
            { "BrushPreview", sizeof(UBOStructures::BrushPreviewUBO) },
            { "FoliageParams", sizeof(UBOStructures::FoliageUBO) },
            { "DecalParams", sizeof(UBOStructures::DecalUBO) },
            { "WaterParams", sizeof(UBOStructures::WaterUBO) },
            { "ForwardPlusParams", sizeof(UBOStructures::ForwardPlusUBO) },
            { "SelectionOutlineUBO", sizeof(UBOStructures::SelectionOutlineUBO) },
            { "JumpFloodUBO", sizeof(UBOStructures::JumpFloodUBO) },
            { "IBLParameters", sizeof(UBOStructures::IBLParametersUBO) },
        } };

        const KnownBlock* FindKnownBlock(std::string_view glslName)
        {
            for (const auto& block : kKnownBlocks)
            {
                if (block.GlslName == glslName)
                    return &block;
            }
            return nullptr;
        }

        struct SizeFailure
        {
            std::string ShaderPath;
            std::string BlockName;
            u32 GlslSize;
            u32 CppSize;
        };

        std::string ResolveBlockName(const spirv_cross::Compiler& compiler,
                                     const spirv_cross::Resource& resource)
        {
            if (std::string baseTypeName = compiler.get_name(resource.base_type_id); !baseTypeName.empty())
                return baseTypeName;
            return resource.name;
        }
    } // namespace

    TEST(ShaderUBOSizeConsistency, GlslBlockSizeNeverExceedsCppStruct)
    {
        const fs::path root = SH::ResolveShaderRoot();
        ASSERT_FALSE(root.empty());
        const auto shaders = SH::EnumerateProductionShaders(root);
        ASSERT_FALSE(shaders.empty());

        shaderc::Compiler compiler;
        ASSERT_TRUE(compiler.IsValid());

        std::vector<SizeFailure> failures;
        u32 blocksChecked = 0;
        u32 blocksSkippedUnknown = 0;

        for (const auto& path : shaders)
        {
            const std::string source = SH::ReadWholeFile(path);
            auto stages = SH::SplitByType(source);
            if (stages.empty() && source.find("local_size_x") != std::string::npos)
                stages.emplace_back(shaderc_glsl_compute_shader, source);

            for (const auto& [kind, stageSource] : stages)
            {
                auto result = SH::CompileStageToSpv(path, stageSource, kind, root, compiler);
                if (result.GetCompilationStatus() != shaderc_compilation_status_success)
                    continue;

                const std::vector<u32> spirv(result.cbegin(), result.cend());
                try
                {
                    spirv_cross::Compiler refl(spirv);
                    const auto resources = refl.get_shader_resources();
                    for (const auto& res : resources.uniform_buffers)
                    {
                        const std::string name = ResolveBlockName(refl, res);
                        const KnownBlock* known = FindKnownBlock(name);
                        if (!known)
                        {
                            // Pass-local / project-specific UBOs (PostProcessUBO,
                            // MotionBlurUBO, DeferredLightingControls, …) live
                            // outside `UBOStructures::`; their sizes are owned
                            // by the pass that declares them. Out of scope.
                            ++blocksSkippedUnknown;
                            continue;
                        }
                        const auto& type = refl.get_type(res.type_id);
                        const u32 glslSize = static_cast<u32>(refl.get_declared_struct_size(type));
                        ++blocksChecked;
                        if (glslSize > known->CppSize)
                        {
                            failures.push_back({ path.generic_string(), name,
                                                 glslSize, known->CppSize });
                        }
                    }
                }
                catch (...)
                {
                    // Reflection errors are the reflection test's problem.
                }
            }
        }

        EXPECT_GT(blocksChecked, 0u);

        if (!failures.empty())
        {
            std::ostringstream oss;
            oss << failures.size()
                << " GLSL UBO block(s) exceed their corresponding C++ struct size:\n";
            for (const auto& f : failures)
            {
                oss << "----\n"
                    << f.ShaderPath << "\n"
                    << "    block '" << f.BlockName << "': GLSL declares "
                    << f.GlslSize << " B, C++ struct sizeof = " << f.CppSize << " B.\n"
                    << "    Shader reads past the end of the uploaded buffer.\n";
            }
            FAIL() << oss.str();
        }
    }

    // -------------------------------------------------------------------------
    // CrossStageUBOLayoutAgreesWithinShader
    //
    // Catches OloEditor breakage where a single shader's vertex and
    // fragment stages declare the *same* UBO block name (e.g.
    // `CameraMatrices` at binding 0) with different member sets — for
    // instance, vertex includes a trailing `mat4 u_PrevViewProjection`
    // while fragment stops at `_padding0`. std140 lets the C++-side buffer
    // carry extra trailing bytes, BUT only versus the GLSL block; within a
    // single shader program both stages must agree on the block layout or
    // glLinkProgram fails ("struct type mismatch between shaders for
    // uniform ...") and the material silently renders as the fallback.
    // -------------------------------------------------------------------------
    TEST(ShaderUBOSizeConsistency, CrossStageUBOLayoutAgreesWithinShader)
    {
        const fs::path root = SH::ResolveShaderRoot();
        ASSERT_FALSE(root.empty());
        const auto shaders = SH::EnumerateProductionShaders(root);
        ASSERT_FALSE(shaders.empty());

        shaderc::Compiler compiler;
        ASSERT_TRUE(compiler.IsValid());

        struct StageSize
        {
            std::string Stage;
            u32 Size;
        };

        struct Mismatch
        {
            std::string ShaderPath;
            std::string BlockName;
            std::vector<StageSize> Sizes;
        };
        std::vector<Mismatch> mismatches;

        for (const auto& path : shaders)
        {
            const std::string source = SH::ReadWholeFile(path);
            auto stages = SH::SplitByType(source);
            if (stages.size() < 2)
                continue; // Only multi-stage shaders can mismatch.

            // For each UBO block name in this shader, collect the
            // (stage, declared-size) pairs across all stages.
            std::map<std::string, std::vector<StageSize>> blockSizesByName;

            for (const auto& [kind, stageSource] : stages)
            {
                auto result = SH::CompileStageToSpv(path, stageSource, kind, root, compiler);
                if (result.GetCompilationStatus() != shaderc_compilation_status_success)
                    continue;

                const char* stageName = nullptr;
                switch (kind)
                {
                    case shaderc_glsl_vertex_shader:
                        stageName = "vertex";
                        break;
                    case shaderc_glsl_fragment_shader:
                        stageName = "fragment";
                        break;
                    case shaderc_glsl_geometry_shader:
                        stageName = "geometry";
                        break;
                    case shaderc_glsl_tess_control_shader:
                        stageName = "tess_control";
                        break;
                    case shaderc_glsl_tess_evaluation_shader:
                        stageName = "tess_eval";
                        break;
                    case shaderc_glsl_compute_shader:
                        stageName = "compute";
                        break;
                    default:
                        stageName = "?";
                        break;
                }

                try
                {
                    spirv_cross::Compiler refl(std::vector<u32>(result.cbegin(), result.cend()));
                    for (const auto& res : refl.get_shader_resources().uniform_buffers)
                    {
                        const std::string name = ResolveBlockName(refl, res);
                        const auto& type = refl.get_type(res.type_id);
                        const u32 size = static_cast<u32>(refl.get_declared_struct_size(type));
                        blockSizesByName[name].push_back({ stageName, size });
                    }
                }
                catch (...)
                {
                }
            }

            for (const auto& [name, sizes] : blockSizesByName)
            {
                if (sizes.size() < 2)
                    continue;
                // All recorded sizes must agree.
                const u32 first = sizes.front().Size;
                bool agree = true;
                for (const auto& s : sizes)
                    if (s.Size != first)
                    {
                        agree = false;
                        break;
                    }
                if (!agree)
                    mismatches.push_back({ path.generic_string(), name, sizes });
            }
        }

        if (!mismatches.empty())
        {
            std::ostringstream oss;
            oss << mismatches.size()
                << " shader(s) declare a UBO block with inconsistent layout between stages:\n";
            for (const auto& m : mismatches)
            {
                oss << "----\n"
                    << m.ShaderPath << "\n"
                    << "    block '" << m.BlockName << "' size by stage:\n";
                for (const auto& s : m.Sizes)
                    oss << "        " << s.Stage << ": " << s.Size << " B\n";
                oss << "    glLinkProgram() rejects this — material renders as fallback.\n";
            }
            FAIL() << oss.str();
        }
    }

    // -------------------------------------------------------------------------
    // CrossShaderUBOMemberOffsetsAgree
    //
    // Extension of the per-shader UBO size check: for each UBO block
    // name that appears in *multiple* production shaders (e.g.
    // `CameraMatrices` is in every PBR shader), every member's name AND
    // declared offset must agree across every shader that declares it.
    //
    // This catches a subtle bug class: shader A and shader B both
    // declare a `CameraMatrices` block at binding 0, both 272 bytes,
    // but shader A has `mat4 u_View; mat4 u_Projection;` while shader
    // B has `mat4 u_Projection; mat4 u_View;` (swapped). Same total
    // size, identical bindings, glLinkProgram() doesn't catch it —
    // because each shader is a separate program. The runtime uploads
    // a single C++ CameraUBO struct; one shader reads `u_View` from
    // the correct offset and the other reads garbage from
    // u_Projection's slot. Surface in OloEditor: "this material's
    // matrix is wrong but the others look fine."
    // -------------------------------------------------------------------------
    TEST(ShaderUBOSizeConsistency, CrossShaderUBOMemberOffsetsAgree)
    {
        const fs::path root = SH::ResolveShaderRoot();
        ASSERT_FALSE(root.empty());
        const auto shaders = SH::EnumerateProductionShaders(root);
        ASSERT_FALSE(shaders.empty());

        shaderc::Compiler compiler;
        ASSERT_TRUE(compiler.IsValid());

        // Per-block-name layout key: a stable signature of the block's
        // member layout — list of (member_name, member_offset) pairs in
        // declared order. Different signatures for the same block name
        // across shaders = mismatch.
        struct MemberInfo
        {
            std::string Name;
            u32 Offset;
        };
        struct ObservedLayout
        {
            std::vector<MemberInfo> Members;
            std::string FirstShaderPath; // the shader we saw this layout in first
        };
        std::map<std::string, std::vector<ObservedLayout>> blockLayouts;

        for (const auto& path : shaders)
        {
            const std::string source = SH::ReadWholeFile(path);
            auto stages = SH::SplitByType(source);
            if (stages.empty() && source.find("local_size_x") != std::string::npos)
                stages.emplace_back(shaderc_glsl_compute_shader, source);

            for (const auto& [kind, stageSource] : stages)
            {
                auto result = SH::CompileStageToSpv(path, stageSource, kind, root, compiler);
                if (result.GetCompilationStatus() != shaderc_compilation_status_success)
                    continue;

                try
                {
                    spirv_cross::Compiler refl(std::vector<u32>(result.cbegin(), result.cend()));
                    for (const auto& res : refl.get_shader_resources().uniform_buffers)
                    {
                        const std::string blockName = ResolveBlockName(refl, res);
                        const auto& type = refl.get_type(res.type_id);

                        ObservedLayout obs;
                        obs.FirstShaderPath = path.generic_string();
                        for (u32 m = 0; m < type.member_types.size(); ++m)
                        {
                            const std::string memberName =
                                refl.get_member_name(res.type_id, m);
                            const u32 offset =
                                refl.get_member_decoration(res.type_id, m, spv::DecorationOffset);
                            obs.Members.push_back({ memberName, offset });
                        }

                        auto& layouts = blockLayouts[blockName];
                        // Check if this matches any existing layout. If
                        // not, record as a new variant.
                        bool matched = false;
                        for (const auto& existing : layouts)
                        {
                            if (existing.Members.size() != obs.Members.size())
                                continue;
                            bool sameLayout = true;
                            for (sizet k = 0; k < existing.Members.size(); ++k)
                            {
                                if (existing.Members[k].Name != obs.Members[k].Name ||
                                    existing.Members[k].Offset != obs.Members[k].Offset)
                                {
                                    sameLayout = false;
                                    break;
                                }
                            }
                            if (sameLayout)
                            {
                                matched = true;
                                break;
                            }
                        }
                        if (!matched)
                            layouts.push_back(std::move(obs));
                    }
                }
                catch (...)
                {
                }
            }
        }

        // Report any block name with >1 distinct observed layout.
        std::ostringstream errors;
        for (const auto& [blockName, layouts] : blockLayouts)
        {
            if (layouts.size() <= 1)
                continue;

            // std140 allows shaders to declare a prefix of a buffer
            // (one shader sees a shorter CameraMatrices than another).
            // Detect the prefix-of-prefix case: if every shorter layout
            // is a strict prefix of the longest one (same members, same
            // offsets, just truncated), accept it. Disagreement on a
            // shared prefix's offsets IS the bug.
            const ObservedLayout* longest = &layouts.front();
            for (const auto& l : layouts)
                if (l.Members.size() > longest->Members.size())
                    longest = &l;

            bool allPrefixesAgree = true;
            for (const auto& l : layouts)
            {
                const sizet n = l.Members.size();
                if (n > longest->Members.size())
                {
                    allPrefixesAgree = false;
                    break;
                }
                for (sizet k = 0; k < n; ++k)
                {
                    if (l.Members[k].Name != longest->Members[k].Name ||
                        l.Members[k].Offset != longest->Members[k].Offset)
                    {
                        allPrefixesAgree = false;
                        break;
                    }
                }
                if (!allPrefixesAgree)
                    break;
            }

            if (allPrefixesAgree)
                continue;

            errors << "----\nUBO block '" << blockName
                   << "' has " << layouts.size()
                   << " incompatible layouts across shaders:\n";
            for (const auto& l : layouts)
            {
                errors << "    in " << l.FirstShaderPath << ":\n";
                for (const auto& m : l.Members)
                    errors << "        offset=" << m.Offset << " name=" << m.Name << "\n";
            }
        }

        if (!errors.str().empty())
            FAIL() << "Cross-shader UBO member-layout disagreement:\n"
                   << errors.str();
    }
} // namespace OloEngine::Tests
