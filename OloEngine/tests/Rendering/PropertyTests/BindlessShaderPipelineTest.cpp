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

#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <gtest/gtest.h>
#include <shaderc/shaderc.hpp>

#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifndef OLO_TEST_EDITOR_ROOT
#error "OLO_TEST_EDITOR_ROOT must be defined by the test target's CMake — see OloEngine/tests/CMakeLists.txt"
#endif

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

    // =========================================================================
    // THE §5c GUARD (issue #691 Phase 3).
    //
    // docs/agent-rules/glsl-shaders.md §5c states the rule in prose:
    //
    //     The unit of conversion is a C++ bind AND its declaration, together.
    //
    // `WantsBindlessVariant()` is a property of the whole PROGRAM, not of one
    // input. The moment a shader converts any input it builds as the bindless
    // variant, so `Shader::IsBoundProgramBindless()` is true for every bind
    // issued while it is in flight, and `HeapBinding::BindTextureOrOffset`
    // records an offset and issues NO bind. A sampler whose DECLARATION is
    // still `layout(binding = N)` is therefore left unbound and reads black.
    //
    // WHY THIS IS A TEST AND NOT (JUST) THE PROSE RULE. The rule was already
    // written down, with a worked example, when Terrain_Depth.glsl was converted
    // one line at a time: `u_TerrainHeightmap` got a proper #ifdef/#else pair and
    // `u_SnowDepthMap`, declared on the very next line, did not. Under
    // OLO_RHI_BINDLESS=1 the snow-deformation fetch silently returned zero. The
    // whole suite stayed green in BOTH configurations, because the failure is
    // invisible to anything that does not compare the two.
    //
    // THE ONE LEGITIMATE EXCEPTION is a slot published through
    // `HeapBinding::PublishTextureOffsetAndBind`, which stages an offset AND
    // always binds, precisely so a slot-based consumer of the same slot keeps
    // working. Those slots are named below rather than hard-coded, so the
    // exception list cannot drift from the binding layout.
    // =========================================================================
    namespace
    {
        // Slots bound through PublishTextureOffsetAndBind: staged as an offset
        // for bindless readers AND bound for slot-based ones, so a surviving
        // slot-based declaration of these is correct, not a hazard.
        [[nodiscard]] bool SlotIsPublishedAndBound(u32 binding)
        {
            return binding == ShaderBindingLayout::TEX_DDGI_IRRADIANCE ||
                   binding == ShaderBindingLayout::TEX_DDGI_VISIBILITY ||
                   binding == ShaderBindingLayout::TEX_DDGI_PROBE_DATA;
        }

        [[nodiscard]] std::string ReadWholeFile(const std::filesystem::path& path)
        {
            std::ifstream in(path, std::ios::binary);
            std::ostringstream buf;
            buf << in.rdbuf();
            return buf.str();
        }

        // Blank comment bodies, preserving newlines so line-oriented scanning
        // still works. Same discipline as RHIBoundaryRatchetTest: a rule that
        // matches inside a comment measures the prose, not the code.
        [[nodiscard]] std::string BlankComments(const std::string& text)
        {
            std::string out = text;
            const sizet n = out.size();
            for (sizet i = 0; i + 1 < n;)
            {
                if (out[i] == '/' && out[i + 1] == '/')
                {
                    while (i < n && out[i] != '\n')
                    {
                        out[i++] = ' ';
                    }
                    continue;
                }
                if (out[i] == '/' && out[i + 1] == '*')
                {
                    const sizet end = out.find("*/", i + 2);
                    const sizet stop = (end == std::string::npos) ? n : end + 2;
                    for (; i < stop; ++i)
                    {
                        if (out[i] != '\n')
                        {
                            out[i] = ' ';
                        }
                    }
                    continue;
                }
                ++i;
            }
            return out;
        }

        // Inline #include the way OpenGLShader does before handing source to the
        // compiler, so a declaration living in a shared header is still seen.
        [[nodiscard]] std::string ResolveIncludes(const std::filesystem::path& path,
                                                  const std::filesystem::path& shaderRoot,
                                                  std::set<std::string>& seen)
        {
            const std::string key = path.lexically_normal().string();
            if (seen.contains(key))
            {
                return {};
            }
            seen.insert(key);

            const std::string src = ReadWholeFile(path);
            static const std::regex kInclude(R"(#include\s+\"([^\"]+)\")");

            std::string out;
            auto begin = std::sregex_iterator(src.begin(), src.end(), kInclude);
            const auto end = std::sregex_iterator();
            sizet last = 0;
            for (auto it = begin; it != end; ++it)
            {
                const std::smatch& m = *it;
                out.append(src, last, static_cast<sizet>(m.position()) - last);
                out.append(ResolveIncludes(shaderRoot / m[1].str(), shaderRoot, seen));
                last = static_cast<sizet>(m.position() + m.length());
            }
            out.append(src, last, std::string::npos);
            return out;
        }

        struct SamplerDecl
        {
            u32 Binding{};
            std::string Name;
            u32 Line{};
        };

        // The declarations that survive with OLO_BINDLESS defined. Frames opened
        // by a condition that does NOT mention OLO_BINDLESS are "pass-through":
        // both of their branches stay active, so a hazard hiding in the #else of
        // an unrelated conditional is still caught.
        [[nodiscard]] std::vector<SamplerDecl> ActiveSamplerDeclarations(const std::string& source)
        {
            struct Frame
            {
                bool Active{ true };
                bool IsBindlessFrame{ false };
                bool ParentActive{ true };
            };

            static const std::regex kIfDir(R"(^\s*#\s*(ifdef|ifndef|if)\b(.*)$)");
            static const std::regex kSampler(
                R"(layout\s*\([^)]*binding\s*=\s*(\d+)[^)]*\)\s*uniform\s+\w*sampler\w*\s+(\w+))");

            std::vector<Frame> stack{ Frame{} };
            std::vector<SamplerDecl> found;

            std::istringstream in(source);
            std::string line;
            u32 lineNo = 0;
            while (std::getline(in, line))
            {
                ++lineNo;

                // STRIP THE CR, and it is load-bearing rather than tidiness. The
                // shader files are CRLF; std::getline splits on '\n' and leaves
                // the '\r'. ECMAScript — which is std::regex's default grammar —
                // counts '\r' as a LineTerminator, so `.` does not match it and
                // an anchored `(.*)$` fails on every single #ifdef line. Every
                // frame then goes unpushed, every declaration looks active, and
                // the test reports the whole shader set as broken. (Python's `.`
                // does match '\r', which is why a prototype of this scan in
                // Python agreed with reality and the first C++ port did not.)
                if (!line.empty() && line.back() == '\r')
                {
                    line.pop_back();
                }

                if (std::smatch m; std::regex_match(line, m, kIfDir))
                {
                    const std::string directive = m[1].str();
                    const bool mentions = m[2].str().find("OLO_BINDLESS") != std::string::npos;
                    const bool parentActive = stack.back().Active;

                    Frame frame;
                    frame.IsBindlessFrame = mentions;
                    frame.ParentActive = parentActive;
                    frame.Active = parentActive && (!mentions || directive != "ifndef");
                    stack.push_back(frame);
                    continue;
                }
                if (line.find("#else") != std::string::npos && stack.size() > 1)
                {
                    Frame& top = stack.back();
                    // Only a genuine OLO_BINDLESS fork has a branch that is
                    // definitely NOT taken; anything else keeps both halves.
                    top.Active = top.IsBindlessFrame ? (top.ParentActive && !top.Active) : top.ParentActive;
                    continue;
                }
                if (line.find("#endif") != std::string::npos && stack.size() > 1)
                {
                    stack.pop_back();
                    continue;
                }

                if (!stack.back().Active)
                {
                    continue;
                }
                if (std::smatch m; std::regex_search(line, m, kSampler))
                {
                    found.push_back(SamplerDecl{ static_cast<u32>(std::stoul(m[1].str())), m[2].str(), lineNo });
                }
            }
            return found;
        }
    } // namespace

    TEST(BindlessShaderPipeline, NoBindlessRouteShaderKeepsASlotBasedSamplerDeclaration)
    {
        namespace fs = std::filesystem;

        const fs::path shaderRoot = fs::path{ OLO_TEST_EDITOR_ROOT } / "assets" / "shaders";
        ASSERT_TRUE(fs::exists(shaderRoot)) << "shader root not found: " << shaderRoot.string();

        std::vector<std::string> offenders;
        u32 scanned = 0;
        u32 onBindlessRoute = 0;

        for (const auto& entry : fs::recursive_directory_iterator(shaderRoot))
        {
            if (!entry.is_regular_file())
            {
                continue;
            }
            const fs::path& p = entry.path();
            const std::string ext = p.extension().string();
            if (ext != ".glsl" && ext != ".comp")
            {
                continue;
            }
            // Shared headers are not compiled on their own; they are measured
            // through the shaders that include them.
            if (p.parent_path().filename() == "include")
            {
                continue;
            }
            ++scanned;

            std::set<std::string> seen;
            const std::string resolved = BlankComments(ResolveIncludes(p, shaderRoot, seen));
            if (resolved.find("OLO_BINDLESS") == std::string::npos)
            {
                continue;
            }
            ++onBindlessRoute;

            for (const SamplerDecl& decl : ActiveSamplerDeclarations(resolved))
            {
                if (SlotIsPublishedAndBound(decl.Binding))
                {
                    continue;
                }
                offenders.push_back(p.filename().string() + ": binding " + std::to_string(decl.Binding) + " '" +
                                    decl.Name + "' survives with OLO_BINDLESS defined");
            }
        }

        // Floor guard, same reasoning as RHIBoundaryRatchetTest: a scan that
        // silently found nothing to scan would pass this test vacuously.
        EXPECT_GT(scanned, 100u) << "the shader scan did not actually run";
        EXPECT_GT(onBindlessRoute, 40u) << "no shaders detected on the bindless route — scan is broken";

        std::string report;
        for (const std::string& o : offenders)
        {
            report += "\n    " + o;
        }
        EXPECT_TRUE(offenders.empty())
            << "These shaders build as the bindless variant but still declare a slot-based sampler.\n"
               "HeapBinding::BindTextureOrOffset stages an offset and issues NO bind for them, so each\n"
               "one reads BLACK under OLO_RHI_BINDLESS=1 — silently, and identically in both configs\n"
               "unless something compares them. Convert the declaration (see §5c), or bind the slot\n"
               "through PublishTextureOffsetAndBind if a slot-based consumer of it also exists."
            << report;
    }
} // namespace OloEngine::Tests
