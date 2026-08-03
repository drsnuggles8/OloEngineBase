// OLO_TEST_LAYER: shaderpipe
// =============================================================================
// BindlessShaderPipelineTest.cpp
//
// Issue #691 Phase 3. Pins the single constraint that decides how heap-bindless
// can reach a shader on the OpenGL backend, because it is a property of the
// TOOLCHAIN rather than of our code and a version bump could silently change
// it in either direction.
//
// THE QUESTION. Every production shader takes this path
// (`OpenGLShader::CompileOrGetVulkanBinaries` -> `CompileOrGetOpenGLBinaries`):
//
//     GLSL  --shaderc(target=vulkan 1.2)-->  SPIR-V
//           --SPIRV-Cross-->                 GLSL 450
//           --shaderc(target=opengl 4.5)-->  OpenGL SPIR-V
//           --glShaderBinary/glSpecializeShader-->  program
//
// `GL_ARB_bindless_texture` is a GLSL-only extension that predates SPIR-V. If
// tier 1 rejects it, then NO production shader can be written in bindless GLSL
// without a compile path that bypasses SPIR-V entirely — which is a structural
// finding about the rehearsal, not an implementation detail, and it is exactly
// the kind of thing Phase 3 exists to discover before Phase 4 spends
// Vulkan-specific effort on the same assumption.
//
// WHY THIS IS A TEST AND NOT A COMMENT. A comment recording "shaderc rejects
// this" rots the first time the vendored toolchain moves. This runs the real
// compiler with the real target environments the engine uses, so the day the
// answer changes the test says so.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>
#include <shaderc/shaderc.hpp>

#include <string>

namespace OloEngine::Tests
{
    namespace
    {
        // Minimal, self-contained, and shaped exactly like the real thing: a
        // heap of handles in a buffer, indexed by a runtime offset, converted to
        // a sampler by the extension's constructor. Nothing here is engine
        // specific — if this compiles, bindless GLSL is expressible on the path.
        constexpr const char* kBindlessFragment = R"(#version 460 core
#extension GL_ARB_bindless_texture : require

layout(std430, binding = 45) readonly buffer ResourceHeapBlock
{
    uvec2 g_ResourceHeap[];
};

layout(std140, binding = 9) uniform HeapOffsets
{
    uint u_AlbedoOffset;
};

layout(location = 0) in vec2 v_TexCoord;
layout(location = 0) out vec4 o_Color;

void main()
{
    o_Color = texture(sampler2D(g_ResourceHeap[u_AlbedoOffset]), v_TexCoord);
}
)";

        // The same shader in the ordinary slot-based form, as a control. If the
        // control also fails, the harness is broken and the bindless result
        // means nothing — the same "floor guard" reasoning RHIBoundaryRatchetTest
        // uses to stop a broken scanner from passing forever.
        constexpr const char* kBindfulFragment = R"(#version 460 core

layout(binding = 0) uniform sampler2D u_Albedo;

layout(location = 0) in vec2 v_TexCoord;
layout(location = 0) out vec4 o_Color;

void main()
{
    o_Color = texture(u_Albedo, v_TexCoord);
}
)";

        struct Result
        {
            bool Succeeded = false;
            std::string Message;
        };

        [[nodiscard]] Result Compile(const char* source, shaderc_target_env targetEnv, u32 targetVersion)
        {
            shaderc::Compiler compiler;
            shaderc::CompileOptions options;
            options.SetTargetEnvironment(targetEnv, targetVersion);
            options.SetPreserveBindings(true);
            options.SetSuppressWarnings();

            const shaderc::SpvCompilationResult module = compiler.CompileGlslToSpv(
                source, shaderc_glsl_fragment_shader, "bindless_probe.glsl", options);

            return Result{ .Succeeded = module.GetCompilationStatus() == shaderc_compilation_status_success,
                           .Message = module.GetErrorMessage() };
        }
    } // namespace

    // The harness check. Without it, a shaderc that failed on everything would
    // make the bindless assertions below pass while measuring nothing.
    TEST(BindlessShaderPipeline, TheControlShaderCompilesOnBothTargetEnvironments)
    {
        const Result vulkan = Compile(kBindfulFragment, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2);
        EXPECT_TRUE(vulkan.Succeeded) << "Slot-based GLSL must compile for the Vulkan target: " << vulkan.Message;

        const Result opengl = Compile(kBindfulFragment, shaderc_target_env_opengl, shaderc_env_version_opengl_4_5);
        EXPECT_TRUE(opengl.Succeeded) << "Slot-based GLSL must compile for the OpenGL target: " << opengl.Message;
    }

    // -------------------------------------------------------------------------
    // THE FINDING.
    //
    // The engine's tier-1 compile targets Vulkan, and `GL_ARB_bindless_texture`
    // has no SPIR-V representation in that environment. So heap-bindless GLSL
    // cannot travel the production shader path at all, and the OpenGL rehearsal
    // needs a compile route that hands the ORIGINAL GLSL straight to
    // glShaderSource.
    //
    // If this ever starts passing — a toolchain bump adding the extension, or a
    // move to SPV_NV_bindless_texture — the raw-GLSL route becomes removable and
    // that is worth knowing immediately, which is why the assertion is written
    // in the direction that fails on GOOD news rather than being skipped.
    // -------------------------------------------------------------------------
    TEST(BindlessShaderPipeline, BindlessGlslCannotTravelTheProductionSpirvPath)
    {
        const Result vulkan = Compile(kBindlessFragment, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2);

        EXPECT_FALSE(vulkan.Succeeded)
            << "\n  GOOD NEWS, AND IT INVALIDATES A PHASE 3 DESIGN DECISION.\n"
            << "  shaderc now accepts GL_ARB_bindless_texture for the Vulkan target environment,\n"
            << "  so bindless shaders no longer need the raw-GLSL compile route\n"
            << "  (OpenGLShader's bindless path). Delete that route and this test, and update\n"
            << "  docs/agent-rules/rhi-abstraction-boundary.md's Phase 3 section.\n";

        if (!vulkan.Succeeded)
        {
            // Recorded rather than asserted on: the exact wording is a shaderc
            // implementation detail, and pinning it would make this test fail
            // for a reason nobody cares about.
            GTEST_LOG_(INFO) << "shaderc(vulkan) rejected bindless GLSL as expected:\n"
                             << vulkan.Message;
        }
    }

    // The OpenGL target environment is probed separately because it is the
    // plausible escape hatch — and it is not one. Even if tier 3 accepted the
    // extension, tier 1 is where the shader ENTERS the pipeline, so a tier-3
    // success would not help. Measured anyway so the record is complete rather
    // than inferred.
    TEST(BindlessShaderPipeline, TheOpenGLTargetEnvironmentIsNotAnEscapeHatch)
    {
        const Result opengl = Compile(kBindlessFragment, shaderc_target_env_opengl, shaderc_env_version_opengl_4_5);

        GTEST_LOG_(INFO) << "shaderc(opengl 4.5) on bindless GLSL: "
                         << (opengl.Succeeded ? "accepted" : "rejected — " + opengl.Message);

        // Deliberately no assertion on the outcome. What matters is the tier-1
        // result above; this exists so a future reader does not have to re-run
        // the experiment to know both halves.
        SUCCEED();
    }
} // namespace OloEngine::Tests
