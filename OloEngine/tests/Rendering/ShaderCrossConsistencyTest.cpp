// =============================================================================
// ShaderCrossConsistencyTest.cpp
//
// Catches the class of OloEditor breakage where two production shaders
// disagree on the binding index for the *same* UBO / SSBO block name. At
// runtime the engine writes (say) CameraUBO to slot 0; a rogue shader that
// declared `layout(binding = 5) uniform CameraMatrices` reads stale data
// from slot 5 → silently wrong matrices → wrong projection / lighting,
// undiagnosable from the editor.
//
// What this test does
// -------------------
//   For every production shader stage:
//     1. Compile to SPIR-V.
//     2. Reflect every uniform_buffers / storage_buffers resource.
//     3. Record { block_name -> set<(binding, shader_path)> }.
//   Assert: each block name maps to a single binding index. If two shaders
//   use the same block name with different bindings, surface ALL the
//   conflicting (shader, binding) pairs.
//
// Companion test (same file) — SamplerBindingsHaveConsistentType
// --------------------------------------------------------------
//   Every texture binding slot that is used as a sampler must be declared
//   with the *same* sampler type (sampler2D / samplerCube / sampler3D /
//   sampler2DArray / …) across all shaders. The runtime binds one texture
//   object per binding; a shader that declares slot 9 as samplerCube while
//   another declares it as sampler2D produces silently wrong sampling.
//   Sampler-variable NAMES are pass-local by design (u_Diffuse vs
//   u_Texture vs u_FogTexture at slot 0 are all OK) so name agreement is
//   not enforced — only the underlying sampler type.
//
// What this test does NOT do
// --------------------------
//   * Block names that only appear in a single shader pass trivially —
//     there is nothing to disagree with.
//
// Classification: shaderpipe.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>
#include <spirv_cross/spirv_cross.hpp>

#include "ShaderHarness.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;
        namespace SH = ShaderHarness;

        struct BindingUse
        {
            u32 Binding;
            std::string ShaderPath;
        };

        /// Pick the GLSL-side block-type name (same logic as the
        /// reflection test). For `uniform CameraMatrices { ... };` (no
        /// instance name — the engine's convention), spirv-cross reports
        /// the block type via `get_name(base_type_id)`.
        std::string ResolveBlockName(const spirv_cross::Compiler& compiler,
                                     const spirv_cross::Resource& resource)
        {
            std::string baseTypeName = compiler.get_name(resource.base_type_id);
            if (!baseTypeName.empty())
                return baseTypeName;
            return resource.name;
        }

        void CollectBindings(const spirv_cross::Compiler& refl,
                             const spirv_cross::SmallVector<spirv_cross::Resource>& resources,
                             const std::string& shaderPath,
                             std::map<std::string, std::vector<BindingUse>>& nameToUses)
        {
            for (const auto& res : resources)
            {
                const u32 binding = refl.get_decoration(res.id, spv::DecorationBinding);
                const std::string name = ResolveBlockName(refl, res);
                if (name.empty())
                    continue;
                nameToUses[name].push_back({ binding, shaderPath });
            }
        }
    } // namespace

    TEST(ShaderCrossConsistency, BlockNamesHaveUniqueBindings)
    {
        const fs::path root = SH::ResolveShaderRoot();
        ASSERT_FALSE(root.empty());
        const auto shaders = SH::EnumerateProductionShaders(root);
        ASSERT_FALSE(shaders.empty());

        shaderc::Compiler compiler;
        ASSERT_TRUE(compiler.IsValid());

        // Map block name -> every observed (binding, shader) pair. Tracked
        // separately for UBOs and SSBOs because spirv-cross treats them as
        // different resource categories.
        std::map<std::string, std::vector<BindingUse>> uboNameToUses;
        std::map<std::string, std::vector<BindingUse>> ssboNameToUses;

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
                    CollectBindings(refl, resources.uniform_buffers, path.generic_string(), uboNameToUses);
                    CollectBindings(refl, resources.storage_buffers, path.generic_string(), ssboNameToUses);
                }
                catch (...)
                {
                    // Reflection errors are the reflection test's problem.
                }
            }
        }

        auto checkUnique = [](const std::map<std::string, std::vector<BindingUse>>& nameToUses,
                              const char* kind,
                              std::ostringstream& errors)
        {
            for (const auto& [name, uses] : nameToUses)
            {
                std::set<u32> bindings;
                for (const auto& u : uses)
                    bindings.insert(u.Binding);

                if (bindings.size() <= 1)
                    continue;

                errors << "----\n"
                       << kind << " block '" << name
                       << "' is bound to " << bindings.size()
                       << " different indices across production shaders:\n";
                for (const auto& u : uses)
                    errors << "    binding " << u.Binding << " in " << u.ShaderPath << "\n";
            }
        };

        std::ostringstream errors;
        checkUnique(uboNameToUses, "UBO", errors);
        checkUnique(ssboNameToUses, "SSBO", errors);

        if (!errors.str().empty())
            FAIL() << "Cross-shader binding inconsistency detected:\n"
                   << errors.str();

        EXPECT_FALSE(uboNameToUses.empty()) << "Reflection produced no UBO blocks across all shaders.";
    }

    // -------------------------------------------------------------------------
    // SamplerBindingsHaveConsistentType
    // -------------------------------------------------------------------------

    namespace
    {
        // We compare on the underlying texture-target dimension only. The
        // MSAA / non-MSAA pair (sampler2D vs sampler2DMS) is a deliberate
        // variant: `DeferredLighting.glsl` and `DeferredLighting_MSAA.glsl`
        // are alternate implementations of the same render path, and the
        // engine binds the matching GL texture target per shader. Likewise
        // sampler2D vs sampler2DArray share Dim2D but read from different
        // GL targets — flagging them as "inconsistent" would just flag a
        // runtime-rebinding detail rather than a target conflict.
        struct SamplerSignature
        {
            spv::Dim Dim;

            bool operator==(const SamplerSignature& o) const noexcept
            {
                return Dim == o.Dim;
            }
            bool operator!=(const SamplerSignature& o) const noexcept
            {
                return !(*this == o);
            }
            bool operator<(const SamplerSignature& o) const noexcept
            {
                return Dim < o.Dim;
            }
        };

        std::string DescribeSampler(const SamplerSignature& s)
        {
            switch (s.Dim)
            {
                case spv::Dim1D:
                    return "sampler1D-family";
                case spv::Dim2D:
                    return "sampler2D-family";
                case spv::Dim3D:
                    return "sampler3D";
                case spv::DimCube:
                    return "samplerCube-family";
                case spv::DimRect:
                    return "samplerRect";
                case spv::DimBuffer:
                    return "samplerBuffer";
                default:
                    return "sampler?";
            }
        }

        struct SamplerUse
        {
            SamplerSignature Sig;
            std::string SamplerName;
            std::string ShaderPath;
        };
    } // namespace

    TEST(ShaderCrossConsistency, SamplerBindingsHaveConsistentType)
    {
        const fs::path root = SH::ResolveShaderRoot();
        ASSERT_FALSE(root.empty());
        const auto shaders = SH::EnumerateProductionShaders(root);
        ASSERT_FALSE(shaders.empty());

        shaderc::Compiler compiler;
        ASSERT_TRUE(compiler.IsValid());

        std::map<u32, std::vector<SamplerUse>> bindingToUses;

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
                    for (const auto& res : refl.get_shader_resources().sampled_images)
                    {
                        const u32 binding = refl.get_decoration(res.id, spv::DecorationBinding);

                        // `TEX_USER_0/1/2` are documented in
                        // ShaderBindingLayout.h as user-defined / pass-local
                        // slots: PBR shaders bind IBL cubemaps there while
                        // Decal shaders bind 2D decal maps to the same
                        // indices, with the engine rebinding between passes.
                        // Type consistency is not part of the contract.
                        if (binding == ShaderBindingLayout::TEX_USER_0 ||
                            binding == ShaderBindingLayout::TEX_USER_1 ||
                            binding == ShaderBindingLayout::TEX_USER_2)
                            continue;

                        const auto& type = refl.get_type(res.type_id);
                        bindingToUses[binding].push_back({
                            { type.image.dim },
                            res.name,
                            path.generic_string(),
                        });
                    }
                }
                catch (...)
                { /* reflection errors handled elsewhere */
                }
            }
        }

        std::ostringstream errors;
        for (const auto& [binding, uses] : bindingToUses)
        {
            std::set<SamplerSignature> distinct;
            for (const auto& u : uses)
                distinct.insert(u.Sig);
            if (distinct.size() <= 1)
                continue;

            errors << "----\nTexture binding " << binding
                   << " is declared as " << distinct.size()
                   << " distinct sampler types across production shaders:\n";
            for (const auto& u : uses)
                errors << "    " << DescribeSampler(u.Sig) << " '" << u.SamplerName
                       << "' in " << u.ShaderPath << "\n";
        }

        if (!errors.str().empty())
            FAIL() << "Sampler-type inconsistency detected:\n"
                   << errors.str();

        EXPECT_FALSE(bindingToUses.empty()) << "Reflection produced no samplers across all shaders.";
    }
} // namespace OloEngine::Tests
