// =============================================================================
// ShaderReflectionBindingTest.cpp
//
// Catches the silent class of OloEditor breakage where a production shader
// compiles fine but binds a uniform block / sampler / SSBO to the wrong
// binding index — at runtime the editor renders garbage (wrong material
// data, missing textures, dark scenes) because the engine wrote, say,
// CameraUBO into slot 5 while the shader read slot 0.
//
// What this test does
// -------------------
//   1. Walk every production `.glsl` (same set as ShaderCompilationTest).
//   2. Compile each stage to SPIR-V (Vulkan 1.2 target, preserve_bindings).
//   3. Reflect with spirv-cross.
//   4. For each discovered UBO  → ShaderBindingLayout::IsKnownUBOBinding(slot, name).
//      For each discovered sampler → ShaderBindingLayout::IsKnownTextureBinding(slot, name).
//      For each discovered SSBO → must fall in the declared SSBO_* range
//        (no per-name validator exists for SSBOs — slot uniqueness is
//        enforced by ShaderBindingLayout::SSBOSlotUniqueness in the
//        sibling ShaderBindingLayoutTest).
//
// Failures are aggregated across all shaders so a single bad refactor
// produces a complete list of affected files instead of one at a time.
//
// Classification: shaderpipe.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>
#include <spirv_cross/spirv_cross.hpp>

#include "ShaderHarness.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <algorithm>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;
        namespace SH = ShaderHarness;

        /// Highest defined SSBO binding in `ShaderBindingLayout`. Any
        /// production shader that declares a storage buffer at a slot
        /// above this is using an undeclared binding — exactly the kind
        /// of drift this test catches.
        constexpr u32 kHighestKnownSSBOBinding = ShaderBindingLayout::SSBO_AUTO_EXPOSURE_STATE;

        struct BindingFailure
        {
            std::string ShaderPath;
            std::string Detail;
        };

        /// Pick the "GLSL-side" name of a uniform block / storage buffer. For
        /// `layout(...) uniform CameraMatrices { ... };` (the engine's
        /// convention — no instance name), spirv-cross typically reports
        /// the block type name. When the resource also has an instance
        /// name (`... } camera;`), `resource.name` holds the instance name
        /// and `get_name(base_type_id)` holds the block type. The
        /// validators match against block-type-style names (Camera*,
        /// Light*, …), so prefer the base type when both are present.
        std::string ResolveBlockName(const spirv_cross::Compiler& compiler,
                                     const spirv_cross::Resource& resource)
        {
            if (std::string baseTypeName = compiler.get_name(resource.base_type_id); !baseTypeName.empty())
                return baseTypeName;
            return resource.name;
        }
    } // namespace

    TEST(ShaderReflectionBinding, AllProductionShaderBindingsMatchCppLayout)
    {
        const fs::path root = SH::ResolveShaderRoot();
        ASSERT_FALSE(root.empty())
            << "Could not resolve OloEditor/assets/shaders root (cwd = "
            << fs::current_path().generic_string() << ")";

        const auto shaders = SH::EnumerateProductionShaders(root);
        ASSERT_FALSE(shaders.empty()) << "No .glsl/.comp files found under " << root;

        shaderc::Compiler compiler;
        ASSERT_TRUE(compiler.IsValid());

        u32 bindingsChecked = 0;
        std::vector<BindingFailure> failures;

        for (const auto& path : shaders)
        {
            const std::string source = SH::ReadWholeFile(path);
            auto stages = SH::SplitStages(source);

            for (const auto& [kind, stageSource] : stages)
            {
                auto result = SH::CompileStageToSpv(path, stageSource, kind, root, compiler);

                if (result.GetCompilationStatus() != shaderc_compilation_status_success)
                {
                    // Compilation failures are ShaderCompilationTest's
                    // responsibility — don't double-report. Skip silently.
                    continue;
                }

                const std::vector<u32> spirv(result.cbegin(), result.cend());

                try
                {
                    spirv_cross::Compiler refl(spirv);
                    const spirv_cross::ShaderResources resources = refl.get_shader_resources();

                    // --- UBOs -------------------------------------------------
                    for (const auto& res : resources.uniform_buffers)
                    {
                        const u32 binding = refl.get_decoration(res.id, spv::DecorationBinding);
                        const std::string name = ResolveBlockName(refl, res);
                        ++bindingsChecked;

                        if (!ShaderBindingLayout::IsKnownUBOBinding(binding, name))
                        {
                            std::ostringstream oss;
                            oss << "UBO binding " << binding << " (name='" << name
                                << "') is not recognised by ShaderBindingLayout::IsKnownUBOBinding. "
                                << "Either the binding index drifted from the C++ constant or "
                                << "the block name no longer matches the validator's pattern.";
                            failures.push_back({ path.generic_string(), oss.str() });
                        }
                    }

                    // --- Sampled images (textures) ----------------------------
                    for (const auto& res : resources.sampled_images)
                    {
                        const u32 binding = refl.get_decoration(res.id, spv::DecorationBinding);
                        const std::string name = res.name;
                        ++bindingsChecked;

                        if (!ShaderBindingLayout::IsKnownTextureBinding(binding, name))
                        {
                            std::ostringstream oss;
                            oss << "Texture binding " << binding << " (name='" << name
                                << "') is not recognised by ShaderBindingLayout::IsKnownTextureBinding. "
                                << "Add the slot to TEX_* constants or update the validator's "
                                << "name pattern for that slot.";
                            failures.push_back({ path.generic_string(), oss.str() });
                        }
                    }

                    // --- SSBOs ------------------------------------------------
                    // No per-name validator exists for SSBOs; instead we
                    // assert the slot falls within the declared SSBO_*
                    // range. A higher slot means GLSL is using a binding
                    // the C++ side never declared.
                    for (const auto& res : resources.storage_buffers)
                    {
                        const u32 binding = refl.get_decoration(res.id, spv::DecorationBinding);
                        const std::string name = ResolveBlockName(refl, res);
                        ++bindingsChecked;

                        if (binding > kHighestKnownSSBOBinding)
                        {
                            std::ostringstream oss;
                            oss << "SSBO binding " << binding << " (name='" << name
                                << "') exceeds the highest declared SSBO_* constant ("
                                << kHighestKnownSSBOBinding << "). Add a new SSBO_* slot in "
                                << "ShaderBindingLayout.h or correct the GLSL binding.";
                            failures.push_back({ path.generic_string(), oss.str() });
                        }
                    }
                }
                catch (const std::exception& e)
                {
                    std::ostringstream oss;
                    oss << "spirv-cross reflection threw: " << e.what();
                    failures.push_back({ path.generic_string(), oss.str() });
                }
            }
        }

        EXPECT_GT(bindingsChecked, 0u);

        if (!failures.empty())
        {
            std::ostringstream oss;
            oss << failures.size() << " shader binding(s) do not match the C++ ShaderBindingLayout:\n";
            for (const auto& f : failures)
                oss << "----\n"
                    << f.ShaderPath << ":\n  " << f.Detail << "\n";
            FAIL() << oss.str();
        }
    }
} // namespace OloEngine::Tests
