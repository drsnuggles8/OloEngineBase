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
        ///
        /// Bump this when you add an SSBO_* constant ABOVE the current top.
        /// It is deliberately a named constant rather than a literal, but it
        /// still has to be re-pointed by hand: deriving "the maximum" would
        /// need the constants enumerated somewhere, and the reason this test
        /// exists is that they are not.
        /// Re-pointed from SSBO_PREFIX_SUM_TOTAL (56) to SSBO_VSM_STATS (68) by
        /// issue #702: the Virtual Shadow Map allocator declares ten storage
        /// buffers in 58..68, and its kernels are production shaders that this
        /// test reflects.
        ///
        /// Re-pointed to SSBO_VSM_STATS (77) by issue #702: the Virtual Shadow
        /// Map allocator's ten storage buffers moved to the contiguous 68..77
        /// range, above #714's terrain-cull block (58..67) — the third VSM
        /// renumber in one PR, each because a parallel branch landed first.
        ///
        /// NOTE it is NOT simply "the numerically largest SSBO_* constant":
        /// SSBO_VERTEX_PULL (57) and SSBO_BONE_PULL (63) are declared only
        /// inside `#ifdef OLO_VULKAN` branches this harness does not compile,
        /// so they never reach reflection. This constant tracks the highest
        /// slot a production shader actually declares.
        // Bumped from SSBO_VSM_STATS (77) by issue #703, which added the one new
        // SSBO the local-light work needed. This is the guard that makes such an
        // addition deliberate rather than incidental -- see the note at
        // SSBO_VSM_LOCAL_LIGHTS for why that binding could not ride an existing
        // buffer, and how little space is left in this namespace.
        //
        // Bumped from SSBO_VSM_LOCAL_LIGHTS (78) by issue #715, whose terrain
        // virtual texture takes 79-81: a feedback buffer the fragment stage
        // writes, and one parameters-plus-payload buffer for each of its two
        // compute stages. It took no UBO slot at all -- the last free one (83)
        // was deliberately left alone, which is why the per-dispatch parameters
        // live in an SSBO header here instead.
        //
        // Bumped to SSBO_DDGI_STATS (83) by issue #707, which added TWO:
        // SSBO_DDGI_PROBE_AUX (82) is one record per probe across all cascades
        // (request timestamps, GPU classification, the #751 bounce accumulator)
        // and SSBO_DDGI_STATS (83) is the per-frame counter block. Neither could
        // ride an existing buffer: both are declared once in
        // include/DDGIProbeBuffers.glsl and used by the six shaders that include
        // it, the aux record sized by the probe field and the stats block
        // written by atomics from three of them (ProbeMaintain, Relight,
        // BlendIrradiance).
        //
        // #707 originally took 79/80 and had to renumber to 82/83 when #715
        // landed on master first -- the FOURTH such renumber recorded in this
        // comment, after #702's three. Note how it surfaced: the duplicate
        // constants merged CLEANLY into ShaderBindingLayout.h (the two blocks
        // sit in different parts of that file), so the only signals were this
        // conflict and SSBOSlotUniqueness. Diff the slot NUMBERS when rebasing,
        // not just the files.
        //
        // The guard did its job for #707: the bindings were added without
        // bumping this, and CI caught it rather than the mismatch reaching a
        // runtime where an out-of-range binding is silent. If you are bumping it
        // again, note the UBO namespace is the tighter one (UBO_BINDING_LIMIT is
        // 83 against a GL 4.6 ceiling of 84) -- both #715 and #707 deliberately
        // spent none of it.
        //
        // Re-pointed DOWN to SSBO_TERRAIN_VT (79) by issue #1015, the first
        // move in this history that was not a collision: 80..83 are above what
        // Mesa drivers expose (GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS = 80 on
        // radeonsi, the AMD CI box), so DDGI's two bindings folded into one at
        // 6 and the terrain VT's three buffers now share 79. The ceiling is a
        // constant now -- ShaderBindingLayout::SSBO_BINDING_LIMIT, pinned by a
        // static_assert over every SSBO_* and by
        // ShaderBindingLayout.SSBOSlotsFitTheMesaCeiling -- so the number here
        // can only ever move within 0..79. Nothing above 79 is legal any more.
        // Derived, not hand-pointed: the header's own max over every SSBO_*,
        // so a family that moves the top slot cannot leave this stale.
        constexpr u32 kHighestKnownSSBOBinding = ShaderBindingLayout::SSBO_HIGHEST_BINDING;

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
