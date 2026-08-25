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
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Accessibility/AccessibilitySettings.h"

#include <array>
#include <cstddef> // offsetof, for the DDGI C++<->GLSL layout pins
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
        // 44 = the base 33, plus the five #691 compute blocks, plus
        // ColorBlindParams (#458), plus PrefixSumParams (#713),
        // TerrainCullParams (#714), the two DDGI blocks (#707) and
        // ShadingRateParams (#683).
        const std::array<KnownBlock, 44> kKnownBlocks = { {
            { "CameraMatrices", sizeof(UBOStructures::CameraUBO) },
            { "Camera", sizeof(UBOStructures::CameraUBO) },
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
            { "UnderwaterFogBlock", sizeof(UnderwaterFogUBOData) },
            { "FroxelFogData", sizeof(UBOStructures::FroxelFogUBO) },
            // The compute-shader params blocks introduced when issue #691
            // The sweep migrated bare default-block uniforms into std140 blocks
            // (SPIR-V cannot express a bare uniform). Listed here deliberately:
            // an unlisted block lands in `blocksSkippedUnknown` and is SKIPPED,
            // so without these entries the one test that compares a reflected
            // GLSL block size against its C++ twin silently covered none of
            // them — and six have no other GLSL<->C++ guard at all.
            { "AutoExposureParams", sizeof(UBOStructures::AutoExposureUBO) },
            { "HZBParams", sizeof(UBOStructures::HZBParamsUBO) },
            { "GTAODenoiseParams", sizeof(UBOStructures::GTAODenoiseUBO) },
            { "ParticleSimParams", sizeof(UBOStructures::GPUParticleParamsUBO) },
            { "WindGenerateParams", sizeof(UBOStructures::WindGenerateUBO) },
            { "SnowComputeParams", sizeof(UBOStructures::SnowComputeUBO) },
            { "TerrainErosionParams", sizeof(UBOStructures::TerrainErosionUBO) },
            { "LightCullingParams", sizeof(UBOStructures::LightCullingUBO) },
            { "VirtualClusterCullParams", sizeof(UBOStructures::VirtualClusterCullUBO) },
            { "VirtualRasterParams", sizeof(UBOStructures::VirtualRasterUBO) },
            { "InstanceCullParams", sizeof(UBOStructures::InstanceCullUBO) },
            // Issue #691 — the sweep's completion: the six compute
            // shaders whose passes never ran in the live log.
            // OceanFFTParams is one block declared verbatim in all three
            // Ocean_*.comp passes.
            { "OceanFFTParams", sizeof(UBOStructures::OceanFFTUBO) },
            { "CloudNoiseGenParams", sizeof(UBOStructures::CloudNoiseGenUBO) },
            { "CloudShadowGenParams", sizeof(UBOStructures::CloudShadowGenUBO) },
            { "PrecipitationFeedParams", sizeof(UBOStructures::PrecipitationFeedUBO) },
            { "ReflectionProbeCullParams", sizeof(UBOStructures::ReflectionProbeCullUBO) },
            // Colour-vision adaptation (issue #458). Listed for the reason the
            // comment above gives: an unlisted block is SKIPPED, not failed, so
            // leaving it out would mean this block has no GLSL<->C++ guard at all.
            { "ColorBlindParams", sizeof(ColorBlindUBOData) },
            // GPU prefix-sum / parallel scan (issue #713). One block declared
            // verbatim in PrefixSum_Scan.comp and PrefixSum_AddBlockOffsets.comp.
            // Listed for the same reason as everything above it: unlisted means
            // SKIPPED, so leaving it out is not a neutral omission — it is the
            // block's only GLSL<->C++ size guard silently not existing.
            { "PrefixSumParams", sizeof(UBOStructures::PrefixSumUBO) },
            // GPU terrain LOD quadtree descent params (issue #714), declared
            // once in include/TerrainCullParams.glsl and included by all four
            // Terrain*.comp kernels. Same reason again.
            { "TerrainCullParams", sizeof(UBOStructures::TerrainCullUBO) },
            // Realtime DDGI (issues #632 / #707). Neither block was listed
            // before #707 — so the one test that compares a reflected GLSL
            // block against its C++ twin covered neither, while DDGIVolume grew
            // from 112 to 512 bytes and DDGIPassData from 160 to 400. Both are
            // declared once in an include and read by five shaders each, which
            // is the drift shape this table exists for.
            { "DDGIVolume", sizeof(UBOStructures::DDGIVolumeUBO) },
            { "DDGIPassData", sizeof(UBOStructures::DDGIPassDataUBO) },
            // Variable Rate Compute Shading classification (issue #683),
            // declared in compute/VRCSClassify.comp. It shares UBO_USER_0 with
            // DDGIPassData, so the two are the same binding at different sizes
            // — the reason each dispatch refills the slot immediately before
            // using it, and the reason both are listed here rather than trusted
            // to agree by inspection.
            { "ShadingRateParams", sizeof(UBOStructures::ShadingRateUBO) },
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
            auto stages = SH::SplitStages(source);

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
    // `CameraMatrices` is in every PBR shader), every MEANINGFUL member
    // name must agree with every other shader's member at the same
    // std140 offset.
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
    //
    // Comparison is keyed by std140 OFFSET, not member index -- there is no
    // separate prefix-matching pass; a shorter block simply contributes no
    // observation at the higher offsets, so agreement there is implicit. A
    // purely positional (index-by-index) comparison misfires on two patterns
    // that are common and harmless in this codebase: a shader that only
    // needs a subset of a shared block's fields and stops declaring early
    // (handled for free by the offset key, per the previous sentence), and a
    // shader that merges several trailing scalar fields it doesn't use into
    // one padding field of a different arity — e.g. Particle_Billboard.glsl's
    // Camera block
    // packs the unused position+pad tail into a single `vec4` where
    // OcclusionProxy.glsl declares the real `vec3 u_CameraPosition; float
    // _padding0;` pair — same 16 bytes, different member count, so a
    // positional walk desyncs from that point on even though nothing is
    // actually wrong. Keying by offset sidesteps both: it only asks
    // whether more than one MEANINGFUL name claims the same byte.
    //
    // A leading underscore is this codebase's established convention for
    // "declared for std140 layout parity, deliberately never read" (see
    // `_padding0`, `_foliagePad1`, `_camera_pad_view`, `_vdPad0`, `_ddgiPad0`
    // throughout OloEditor/assets/shaders/). Since std140 reads happen by
    // BYTE OFFSET, not GLSL member name, a shader that names its copy of a
    // slot as a placeholder is provably not reading it — so a placeholder
    // name disagreeing with a real name at the same offset cannot cause
    // wrong rendering, only two DIFFERENT real names claiming the same
    // offset can (the actual bug class this test exists to catch).
    //
    // One further exception, `kMemberAliases` below: GLSL forbids two
    // uniform-block members with the same name at global (non-instanced)
    // scope, so a shader that already has e.g. `u_PrevViewProjection` from
    // its own CameraMatrices block must rename a second block's identically-
    // offset field to avoid a compile error — PostProcess_Cloudscape.glsl's
    // `MotionBlurUBO.u_MB_PrevViewProjection` versus PostProcess_Fog.glsl's
    // instance-named `u_MB.u_PrevViewProjection` are the two different
    // techniques shaders here use to dodge that collision (issue #429). Both
    // real names, both correct, same offset — an explicit, audited alias,
    // not a general escape hatch.
    // -------------------------------------------------------------------------
    TEST(ShaderUBOSizeConsistency, CrossShaderUBOMemberOffsetsAgree)
    {
        const fs::path root = SH::ResolveShaderRoot();
        ASSERT_FALSE(root.empty());
        const auto shaders = SH::EnumerateProductionShaders(root);
        ASSERT_FALSE(shaders.empty());

        shaderc::Compiler compiler;
        ASSERT_TRUE(compiler.IsValid());

        struct MemberAlias
        {
            std::string_view BlockName;
            std::string_view Name;
            std::string_view CanonicalName;
        };
        // Each entry is a claim that two real (non-placeholder) names are
        // the SAME field, verified by reading both declaring shaders --
        // never add one just to make a newly-real mismatch go away. See the
        // header comment above for how this one entry was confirmed.
        static constexpr std::array<MemberAlias, 1> kMemberAliases = { {
            { "MotionBlurUBO", "u_MB_PrevViewProjection", "u_PrevViewProjection" },
        } };
        auto canonicalize = [](std::string_view blockName, const std::string& name) -> std::string
        {
            for (const auto& alias : kMemberAliases)
                if (alias.BlockName == blockName && alias.Name == name)
                    return std::string(alias.CanonicalName);
            return name;
        };

        struct MemberObservation
        {
            std::string Name; // as declared, before alias canonicalization
            std::string ShaderPath;
        };
        // blockName -> offset -> every member observed declaring at that
        // std140 offset, across every shader that declares this block name.
        std::map<std::string, std::map<u32, std::vector<MemberObservation>>> blockOffsets;

        for (const auto& path : shaders)
        {
            const std::string source = SH::ReadWholeFile(path);
            auto stages = SH::SplitStages(source);

            for (const auto& [kind, stageSource] : stages)
            {
                // Debug info is needed here (unlike the two tests above) so
                // shaderc emits OpMemberName and member names actually
                // populate the map below -- see the base_type_id note just
                // below for why offsets need it too.
                auto result = SH::CompileStageToSpv(path, stageSource, kind, root, compiler,
                                                    /*generateDebugInfo=*/true);
                if (result.GetCompilationStatus() != shaderc_compilation_status_success)
                    continue;

                try
                {
                    spirv_cross::Compiler refl(std::vector<u32>(result.cbegin(), result.cend()));
                    for (const auto& res : refl.get_shader_resources().uniform_buffers)
                    {
                        const std::string blockName = ResolveBlockName(refl, res);
                        const auto& type = refl.get_type(res.type_id);

                        for (u32 m = 0; m < type.member_types.size(); ++m)
                        {
                            // spirv-cross keeps member names/decorations
                            // (including DecorationOffset) attached to the
                            // block's *base* type, not res.type_id -- using
                            // type_id here returns "" for the name and 0 for
                            // the offset unconditionally, which is why this
                            // test previously compared ("", 0) against
                            // ("", 0) for every member and could never fail
                            // (#847). get_type(res.type_id) above is left
                            // alone: the member *count* and declared struct
                            // size are correct off either id.
                            const std::string memberName =
                                refl.get_member_name(res.base_type_id, m);
                            const u32 offset =
                                refl.get_member_decoration(res.base_type_id, m, spv::DecorationOffset);
                            blockOffsets[blockName][offset].push_back({ memberName, path.generic_string() });
                        }
                    }
                }
                catch (...)
                {
                }
            }
        }

        // Report every offset where more than one distinct MEANINGFUL
        // (non-placeholder, alias-canonicalized) name is claimed.
        std::ostringstream errors;
        for (const auto& [blockName, offsets] : blockOffsets)
        {
            std::ostringstream blockErrors;
            for (const auto& [offset, observations] : offsets)
            {
                struct Meaningful
                {
                    const MemberObservation* Obs;
                    std::string CanonicalName;
                };
                std::vector<Meaningful> meaningful;
                for (const auto& obs : observations)
                {
                    if (obs.Name.empty() || obs.Name.starts_with('_'))
                        continue;
                    meaningful.push_back({ &obs, canonicalize(blockName, obs.Name) });
                }
                if (meaningful.size() <= 1)
                    continue;

                bool allAgree = true;
                for (const auto& m : meaningful)
                    if (m.CanonicalName != meaningful.front().CanonicalName)
                    {
                        allAgree = false;
                        break;
                    }
                if (allAgree)
                    continue;

                blockErrors << "    offset " << offset << " is claimed by " << meaningful.size()
                            << " different member names:\n";
                for (const auto& m : meaningful)
                    blockErrors << "        " << m.Obs->Name << " in " << m.Obs->ShaderPath << "\n";
            }

            if (!blockErrors.str().empty())
                errors << "----\nUBO block '" << blockName
                       << "' has conflicting member names at shared offsets:\n"
                       << blockErrors.str();
        }

        if (!errors.str().empty())
            FAIL() << "Cross-shader UBO member-layout disagreement:\n"
                   << errors.str();
    }

    // -------------------------------------------------------------------------
    // DDGIBlockMembersMatchTheCppStructOffsets
    //
    // The two tests above leave a hole for the DDGI blocks specifically, and
    // it is the hole that matters most for them:
    //
    //   * the size table only compares TOTAL size, so any two members swapped
    //     between C++ and GLSL keeps the size identical and passes;
    //   * CrossShaderUBOMemberOffsetsAgree only compares shader AGAINST shader,
    //     and both DDGI blocks are declared exactly once in a shared include
    //     (DDGICommon.glsl / DDGIPassData.glsl). Every shader therefore sees
    //     byte-identical text, so that test can never disagree with itself and
    //     provides no protection here at all.
    //
    // What is left unguarded is the only boundary that can actually drift: the
    // C++ struct the engine uploads versus the GLSL block the shaders read.
    // A swap there does not fail to compile, link, or dispatch. Reading
    // CascadeOrigin's bytes as CascadeSpacing gives every cascade a plausible
    // origin and a plausible spacing, so the field renders — lit, smooth, and
    // sampling the wrong world position at every probe.
    //
    // std140 offsets come from spirv-cross's reflected decorations, i.e. what
    // the compiler actually laid out, not a hand-computed prediction of it.
    TEST(ShaderUBOSizeConsistency, DDGIBlockMembersMatchTheCppStructOffsets)
    {
        using OloEngine::UBOStructures::DDGIPassDataUBO;
        using OloEngine::UBOStructures::DDGIVolumeUBO;

        struct MemberPin
        {
            const char* GlslName;
            u32 CppOffset;
            u32 ElementCount; // 0 = not an array
        };

        // std140 packs an array of vec4/ivec4 at 16 bytes per element. Rather
        // than trust a reflected stride decoration, this is DERIVED from the
        // gap to the next member (or to the end of the block for the last
        // one), which validates the actual packing and uses only the two
        // reflection calls the tests above already rely on.
        constexpr u32 kStd140Vec4Stride = 16;

        static constexpr std::array<MemberPin, 23> kVolumePins{ {
            { "u_DDGIBoundsMin", offsetof(DDGIVolumeUBO, BoundsMin), 0 },
            { "u_DDGIBoundsMax", offsetof(DDGIVolumeUBO, BoundsMax), 0 },
            { "u_DDGIGridDimensions", offsetof(DDGIVolumeUBO, GridDimensions), 0 },
            { "u_DDGIProbeSpacing", offsetof(DDGIVolumeUBO, ProbeSpacing), 0 },
            { "u_DDGIEnabled", offsetof(DDGIVolumeUBO, Enabled), 0 },
            { "u_DDGIIntensity", offsetof(DDGIVolumeUBO, Intensity), 0 },
            { "u_DDGIHysteresis", offsetof(DDGIVolumeUBO, Hysteresis), 0 },
            { "u_DDGISelfShadowBias", offsetof(DDGIVolumeUBO, SelfShadowBias), 0 },
            { "u_DDGIHitCacheTexels", offsetof(DDGIVolumeUBO, HitCacheTexels), 0 },
            { "u_DDGIFrameIndex", offsetof(DDGIVolumeUBO, FrameIndex), 0 },
            { "u_DDGIHybridBlend", offsetof(DDGIVolumeUBO, HybridBlend), 0 },
            { "u_DDGIEnergyConservation", offsetof(DDGIVolumeUBO, EnergyConservation), 0 },
            { "u_DDGIMaxRayDistance", offsetof(DDGIVolumeUBO, MaxRayDistance), 0 },
            { "u_DDGIBounceMarginScale", offsetof(DDGIVolumeUBO, BounceMarginScale), 0 },
            { "u_DDGICascadeCount", offsetof(DDGIVolumeUBO, CascadeCount), 0 },
            { "u_DDGICascadeBlendBand", offsetof(DDGIVolumeUBO, CascadeBlendBand), 0 },
            { "u_DDGIUpdateRateDivisor", offsetof(DDGIVolumeUBO, UpdateRateDivisor), 0 },
            { "u_DDGIRequestLifetime", offsetof(DDGIVolumeUBO, RequestLifetime), 0 },
            { "u_DDGISparsityEnabled", offsetof(DDGIVolumeUBO, SparsityEnabled), 0 },
            { "_ddgiPad0", offsetof(DDGIVolumeUBO, Pad0), 0 },
            // The three cascade arrays. Stride is asserted as well as offset:
            // a std140 array of vec4/ivec4 packs at 16 bytes, and a stride
            // disagreement would misread every element after the first while
            // leaving element 0 — the finest cascade, the one most likely to
            // be looked at — perfectly correct.
            { "u_DDGICascadeOrigin", offsetof(DDGIVolumeUBO, CascadeOrigin), DDGIVolumeUBO::MaxCascades },
            { "u_DDGICascadeSpacing", offsetof(DDGIVolumeUBO, CascadeSpacing), DDGIVolumeUBO::MaxCascades },
            { "u_DDGICascadeLattice", offsetof(DDGIVolumeUBO, CascadeLattice), DDGIVolumeUBO::MaxCascades },
        } };

        static constexpr std::array<MemberPin, 9> kPassDataPins{ {
            { "u_DDGIModel", offsetof(DDGIPassDataUBO, Model), 0 },
            { "u_DDGINormalMatrix", offsetof(DDGIPassDataUBO, NormalMatrix), 0 },
            { "u_DDGIBaseColor", offsetof(DDGIPassDataUBO, BaseColor), 0 },
            { "u_DDGIProbePosition", offsetof(DDGIPassDataUBO, ProbePosition), 0 },
            { "u_DDGIInvViewProjection", offsetof(DDGIPassDataUBO, InvViewProjection), 0 },
            { "u_DDGIRenderOrigin", offsetof(DDGIPassDataUBO, RenderOrigin), 0 },
            { "u_DDGICameraPosRel", offsetof(DDGIPassDataUBO, CameraPosRel), 0 },
            { "u_DDGIComputeParams", offsetof(DDGIPassDataUBO, ComputeParams), 0 },
            { "u_DDGIPrevLattice", offsetof(DDGIPassDataUBO, PrevLattice), DDGIVolumeUBO::MaxCascades },
        } };

        const fs::path root = SH::ResolveShaderRoot();
        ASSERT_FALSE(root.empty());
        const auto shaders = SH::EnumerateProductionShaders(root);
        ASSERT_FALSE(shaders.empty());

        shaderc::Compiler compiler;
        ASSERT_TRUE(compiler.IsValid());

        // Which blocks we managed to reflect at all. A DDGI block that stops
        // being declared anywhere must fail this test rather than vacuously
        // pass it -- that is how a pin quietly stops guarding anything.
        bool sawVolume = false;
        bool sawPassData = false;
        std::ostringstream errors;

        auto checkBlock = [&errors](const spirv_cross::Compiler& refl, const spirv_cross::Resource& res,
                                    const std::string& blockName, const MemberPin* pins, sizet pinCount,
                                    const std::string& shaderPath)
        {
            const auto& type = refl.get_type(res.base_type_id);
            if (type.member_types.size() != pinCount)
            {
                errors << blockName << " in " << shaderPath << " declares " << type.member_types.size()
                       << " members, the C++ pin table has " << pinCount
                       << ". A member was added or removed on one side only; update the table in this "
                          "test alongside the struct and the GLSL block.\n";
                return;
            }

            for (u32 m = 0; m < type.member_types.size(); ++m)
            {
                const MemberPin& pin = pins[m];
                // The compile call above now asks for debug info (like the
                // sibling test below), so shaderc emits OpMemberName and this
                // name check is live, not dead code -- still guarded on
                // `!name.empty()` in case that ever changes back, so this
                // stays a real check either way rather than asserting on
                // something the harness might stop producing.
                //
                // Worth knowing when reading the sibling test above:
                // CrossShaderUBOMemberOffsetsAgree used to compare member
                // names and offsets read off res.type_id, which for a UBO
                // resource is always "" / 0 -- both halves of its signature
                // were constant, so it compared ("", 0) to ("", 0) for every
                // member and could never fail (#847, fixed alongside this
                // comment: it now reads off res.base_type_id, same as here).
                // The reorder detection below does not depend on names at
                // all even so: the pins are in GLSL declared order carrying
                // C++ offsets, so two swapped members produce mismatched
                // offsets at both indices regardless of what shaderc names
                // them.
                if (const std::string name = refl.get_member_name(res.base_type_id, m);
                    !name.empty() && name != pin.GlslName)
                {
                    errors << blockName << " member " << m << " is named '" << name << "', expected '"
                           << pin.GlslName << "' -- the block was REORDERED relative to the C++ struct.\n";
                    continue;
                }
                const char* const name = pin.GlslName;

                const u32 glslOffset = refl.get_member_decoration(res.base_type_id, m, spv::DecorationOffset);
                if (glslOffset != pin.CppOffset)
                {
                    errors << blockName << "." << name << " is at std140 offset " << glslOffset
                           << " but the C++ struct puts it at " << pin.CppOffset
                           << ". The engine uploads one struct and the shader reads another layout; "
                              "this renders without erroring.\n";
                }

                if (pin.ElementCount != 0)
                {
                    // Derived, not read from a stride decoration: the span this
                    // array actually occupies is the gap to the next member, or
                    // to the end of the block when it is the last one.
                    const u32 spanEnd =
                        (m + 1 < type.member_types.size())
                            ? refl.get_member_decoration(res.base_type_id, m + 1, spv::DecorationOffset)
                            : static_cast<u32>(refl.get_declared_struct_size(type));
                    const u32 stride = (spanEnd - glslOffset) / pin.ElementCount;
                    if (stride != kStd140Vec4Stride)
                    {
                        errors << blockName << "." << name << " occupies " << (spanEnd - glslOffset)
                               << " bytes for " << pin.ElementCount << " elements (stride " << stride
                               << "), expected stride " << kStd140Vec4Stride
                               << " -- element 0 would still read correctly and every later cascade "
                                  "would not.\n";
                    }
                }
            }
        };

        for (const auto& path : shaders)
        {
            if (sawVolume && sawPassData)
                break;

            const std::string source = SH::ReadWholeFile(path);
            for (const auto& [kind, stageSource] : SH::SplitStages(source))
            {
                // Debug info on, same as CrossShaderUBOMemberOffsetsAgree
                // above -- see the comment on the name check below for why.
                auto result = SH::CompileStageToSpv(path, stageSource, kind, root, compiler,
                                                    /*generateDebugInfo=*/true);
                if (result.GetCompilationStatus() != shaderc_compilation_status_success)
                    continue;

                try
                {
                    spirv_cross::Compiler refl(std::vector<u32>(result.cbegin(), result.cend()));
                    for (const auto& res : refl.get_shader_resources().uniform_buffers)
                    {
                        const std::string blockName = ResolveBlockName(refl, res);
                        if (blockName == "DDGIVolume" && !sawVolume)
                        {
                            sawVolume = true;
                            checkBlock(refl, res, blockName, kVolumePins.data(), kVolumePins.size(),
                                       path.generic_string());
                        }
                        else if (blockName == "DDGIPassData" && !sawPassData)
                        {
                            sawPassData = true;
                            checkBlock(refl, res, blockName, kPassDataPins.data(), kPassDataPins.size(),
                                       path.generic_string());
                        }
                    }
                }
                catch (const std::exception&)
                {
                    // Reflection failures are ShaderCompilationTest's problem.
                }
            }
        }

        EXPECT_TRUE(sawVolume) << "no production shader declares the DDGIVolume block — the C++<->GLSL "
                                  "offset pins below are guarding nothing";
        EXPECT_TRUE(sawPassData) << "no production shader declares the DDGIPassData block — same problem";

        if (!errors.str().empty())
            FAIL() << "DDGI UBO layout disagrees with its C++ struct:\n"
                   << errors.str();
    }
} // namespace OloEngine::Tests
