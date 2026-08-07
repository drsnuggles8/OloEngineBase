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

#include "OloEngine/Renderer/RHI/RHIDescriptorHeap.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <gtest/gtest.h>
#include <shaderc/shaderc.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
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
        // Slots that ALWAYS receive a real GL bind, whatever program is in flight.
        // A surviving slot-based declaration of one of these is correct rather
        // than a hazard, because the thing §5c warns about — the seam withdrawing
        // the bind for a bindless program — does not happen for them.
        //
        // Two mechanisms qualify, and both are deliberate:
        //   * HeapBinding::PublishTextureOffsetAndBind — stages the offset AND
        //     binds, for state read by converted and unconverted shaders alike;
        //   * a direct Texture::Bind() that never consults the seam at all.
        //
        // EVERY ENTRY HERE IS A SAMPLER DECLARED IN A SHARED include/ HEADER, and
        // that is not a coincidence — it is the rule. A header's own
        // `#ifdef OLO_BINDLESS` IS the route opt-in token, so converting a
        // declaration there drags every includer onto the raw-GLSL route,
        // unbinding all of THEIR slot-based samplers. A shared declaration
        // therefore cannot be converted per-shader; its slot has to be bound
        // unconditionally instead. Adding an entry without one of the two
        // mechanisms above turns this allowlist into a way to silence the test.
        [[nodiscard]] bool SlotAlwaysReceivesARealBind(u32 binding)
        {
            return
                // DDGI atlases — published for the DDGI compute shaders, bound for
                // Skybox_GBuffer and the forward PBR readers.
                binding == ShaderBindingLayout::TEX_DDGI_IRRADIANCE ||
                binding == ShaderBindingLayout::TEX_DDGI_VISIBILITY ||
                binding == ShaderBindingLayout::TEX_DDGI_PROBE_DATA ||
                // include/AtmosphereShading.glsl, read by PBR_MultiLight (bindless)
                // and Terrain_PBR / DeferredLightingShared (slot-based).
                binding == ShaderBindingLayout::TEX_CLOUD_SHADOW ||
                // include/CloudscapeCommon.glsl; published in RenderPipeline.cpp.
                binding == ShaderBindingLayout::TEX_CLOUD_BASE_NOISE ||
                binding == ShaderBindingLayout::TEX_CLOUD_DETAIL_NOISE ||
                binding == ShaderBindingLayout::TEX_CLOUD_WEATHER_MAP ||
                // include/WindSampling.glsl; WindSystem::BindWindTexture binds it
                // directly every frame, bypassing the seam entirely.
                binding == ShaderBindingLayout::TEX_WIND_FIELD;
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
        //
        // RESOLVED RELATIVE TO THE INCLUDING FILE FIRST, then to the shader root.
        // Shaders under compute/ spell their includes "../include/Foo.glsl";
        // joining that to the ROOT yields assets/include/Foo.glsl, which does not
        // exist. An earlier version did exactly that and returned an empty string,
        // so every declaration reaching those shaders through a shared header was
        // invisible and the scan under-reported in silence — the same shape of
        // bug this whole test exists to catch. `unresolved` makes that impossible
        // to repeat: the caller asserts on it.
        [[nodiscard]] std::string ResolveIncludes(const std::filesystem::path& path,
                                                  const std::filesystem::path& shaderRoot,
                                                  std::set<std::string>& seen,
                                                  std::vector<std::string>& unresolved)
        {
            namespace fs = std::filesystem;

            const std::string key = path.lexically_normal().string();
            if (seen.contains(key))
            {
                return {};
            }
            seen.insert(key);

            // BLANKED BEFORE THE INCLUDE MATCH, not after. A commented-out
            // `// #include "Foo.glsl"` would otherwise be inlined, dragging in
            // declarations the compiler never sees and reporting them as live —
            // and the callers' later BlankComments could not undo it, because by
            // then the included text is indistinguishable from real source.
            const std::string src = BlankComments(ReadWholeFile(path));
            static const std::regex kInclude(R"(#include\s+\"([^\"]+)\")");

            std::string out;
            auto begin = std::sregex_iterator(src.begin(), src.end(), kInclude);
            const auto end = std::sregex_iterator();
            sizet last = 0;
            for (auto it = begin; it != end; ++it)
            {
                const std::smatch& m = *it;
                out.append(src, last, static_cast<sizet>(m.position()) - last);

                const std::string spelling = m[1].str();
                const fs::path relative = (path.parent_path() / spelling).lexically_normal();
                const fs::path fromRoot = (shaderRoot / spelling).lexically_normal();

                if (fs::exists(relative))
                {
                    out.append(ResolveIncludes(relative, shaderRoot, seen, unresolved));
                }
                else if (fs::exists(fromRoot))
                {
                    out.append(ResolveIncludes(fromRoot, shaderRoot, seen, unresolved));
                }
                else if (!seen.contains(relative.string()))
                {
                    unresolved.push_back(path.filename().string() + " -> " + spelling);
                }
                last = static_cast<sizet>(m.position() + m.length());
            }
            out.append(src, last, std::string::npos);
            return out;
        }

        // MIRRORS OpenGLShader::WantsBindlessVariant, and must keep mirroring it.
        // Whole-identifier, not substring: the engine matches identifiers, so a
        // token that merely CONTAINS one of these (a `MY_OLO_BINDLESS_HACK`) does
        // not put a shader on the route and must not make this scan think it did.
        // Both tokens, because a shader with no samplers of its own opts in with
        // OLO_BINDLESS_ROUTE_PARITY alone (glsl-shaders §7a-bis).
        [[nodiscard]] bool MentionsIdentifier(const std::string& text, std::string_view identifier)
        {
            for (sizet pos = text.find(identifier); pos != std::string::npos;
                 pos = text.find(identifier, pos + identifier.size()))
            {
                const sizet after = pos + identifier.size();
                const bool leftOk =
                    (pos == 0) || !(std::isalnum(static_cast<unsigned char>(text[pos - 1])) || text[pos - 1] == '_');
                const bool rightOk =
                    (after >= text.size()) ||
                    !(std::isalnum(static_cast<unsigned char>(text[after])) || text[after] == '_');
                if (leftOk && rightOk)
                {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] bool WantsBindlessVariant(const std::string& source)
        {
            return MentionsIdentifier(source, "OLO_BINDLESS") ||
                   MentionsIdentifier(source, "OLO_BINDLESS_ROUTE_PARITY");
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
                    const bool mentions = WantsBindlessVariant(m[2].str());
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

    // The reserved null offsets are declared TWICE — once in C++ as
    // RHI::kNull*HeapOffset, once in GLSL as OLO_HEAP_NULL_* — because the shader
    // is the only side that knows which sampler TYPE a declaration wants, so it is
    // the side that substitutes the right null. Two declarations of one constant
    // drift, and drift here is silent: the shader would build a samplerCube from
    // whatever slot the stale number names, which is undefined rather than wrong
    // in any way a test would notice (issue #691 Phase 3).
    TEST(BindlessShaderPipeline, GlslNullOffsetsMatchTheReservedHeapSlots)
    {
        namespace fs = std::filesystem;

        const fs::path header = fs::path{ OLO_TEST_EDITOR_ROOT } / "assets" / "shaders" / "include" /
                                "BindlessHeap.glsl";
        ASSERT_TRUE(fs::exists(header)) << header.string();
        const std::string src = ReadWholeFile(header);

        const auto glslValue = [&src](std::string_view name) -> std::optional<u32>
        {
            const std::regex re(std::string{ "#define\\s+" } + std::string{ name } + R"(\s+(\d+)u)");
            if (std::smatch m; std::regex_search(src, m, re))
            {
                return static_cast<u32>(std::stoul(m[1].str()));
            }
            return std::nullopt;
        };

        struct Pin
        {
            std::string_view Glsl;
            u32 Cpp;
        };
        const std::array<Pin, 4> kPins{ {
            { "OLO_HEAP_NULL_2D", RHI::kNullHeapOffset },
            { "OLO_HEAP_NULL_CUBE", RHI::kNullCubeHeapOffset },
            { "OLO_HEAP_NULL_ARRAY", RHI::kNullArrayHeapOffset },
            { "OLO_HEAP_NULL_ARRAY_SHADOW", RHI::kNullArrayShadowHeapOffset },
        } };

        for (const Pin& pin : kPins)
        {
            const std::optional<u32> value = glslValue(pin.Glsl);
            ASSERT_TRUE(value.has_value()) << pin.Glsl << " is not defined in BindlessHeap.glsl";
            EXPECT_EQ(*value, pin.Cpp)
                << pin.Glsl << " = " << *value << " but the C++ reserves slot " << pin.Cpp
                << ". The shader would construct its sampler from the wrong reserved slot.";
        }

        // And no null may collide with a slot the allocator hands out, or a real
        // descriptor would land on top of one.
        // ALL FIVE, and pairwise DISTINCT. Checking only two left the other three
        // free to collide with an allocatable slot or with each other, and a
        // collision is silent: two kinds sharing one slot means the second one
        // written wins and the first samples a descriptor of the wrong type — the
        // exact undefined read the typed nulls exist to remove.
        const std::array<std::pair<std::string_view, u32>, 10> kReserved{ {
            { "kNullHeapOffset", RHI::kNullHeapOffset },
            { "kNullStorageHeapOffset", RHI::kNullStorageHeapOffset },
            { "kNullCubeHeapOffset", RHI::kNullCubeHeapOffset },
            { "kNullArrayHeapOffset", RHI::kNullArrayHeapOffset },
            { "kNullArrayShadowHeapOffset", RHI::kNullArrayShadowHeapOffset },
            { "kNullStorageRGBA32FHeapOffset", RHI::kNullStorageRGBA32FHeapOffset },
            { "kNullStorageR8HeapOffset", RHI::kNullStorageR8HeapOffset },
            { "kNullStorageRGBA16FHeapOffset", RHI::kNullStorageRGBA16FHeapOffset },
            { "kNullStorageRGBA8HeapOffset", RHI::kNullStorageRGBA8HeapOffset },
            { "kNullStorageR32UIHeapOffset", RHI::kNullStorageR32UIHeapOffset },
        } };
        for (const auto& [name, value] : kReserved)
        {
            EXPECT_LT(value, RHI::kFirstAllocatableHeapSlot)
                << name << " is not inside the reserved region, so the allocator can hand it out";
        }
        for (sizet i = 0; i < kReserved.size(); ++i)
        {
            for (sizet j = i + 1; j < kReserved.size(); ++j)
            {
                EXPECT_NE(kReserved[i].second, kReserved[j].second)
                    << kReserved[i].first << " and " << kReserved[j].first << " share a slot";
            }
        }
    }

    TEST(BindlessShaderPipeline, NoBindlessRouteShaderKeepsASlotBasedSamplerDeclaration)
    {
        namespace fs = std::filesystem;

        const fs::path shaderRoot = fs::path{ OLO_TEST_EDITOR_ROOT } / "assets" / "shaders";
        ASSERT_TRUE(fs::exists(shaderRoot)) << "shader root not found: " << shaderRoot.string();

        std::vector<std::string> offenders;
        std::vector<std::string> unresolved;
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
            // Already comment-free: ResolveIncludes blanks each file as it reads it.
            const std::string resolved = ResolveIncludes(p, shaderRoot, seen, unresolved);
            if (!WantsBindlessVariant(resolved))
            {
                continue;
            }
            ++onBindlessRoute;

            for (const SamplerDecl& decl : ActiveSamplerDeclarations(resolved))
            {
                if (SlotAlwaysReceivesARealBind(decl.Binding))
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

        // An include this scan cannot open is a hole in its coverage, not a
        // detail: every declaration behind it becomes invisible and the shader
        // silently looks clean. Fail rather than under-report.
        std::string missing;
        for (const std::string& u : unresolved)
        {
            missing += "\n    " + u;
        }
        EXPECT_TRUE(unresolved.empty())
            << "unresolvable #include(s) — the scan cannot see what they declare:" << missing;

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

    // =========================================================================
    // THE COMPLETENESS GUARD (issue #691 Phase 3, closing bucket 1).
    //
    // The test above asks "is a converted shader converted WHOLE". This one asks
    // the other half: "is every shader that COULD be converted either converted
    // or a decision". Without it the phase has no end condition — a shader left
    // slot-based is indistinguishable from one nobody got to, and "39 remaining"
    // stays true forever because nothing forces the number to mean anything.
    //
    // A slot-based shader is not a bug (the seam forks per program, so an
    // unconverted one gets a real bind and renders correctly). What it is, is an
    // unrecorded judgement — and the four reasons below are genuinely different
    // from each other, which is why a single "not yet" bucket would have been
    // worse than none.
    //
    // THE TABLE IS CHECKED IN BOTH DIRECTIONS. An entry naming a shader that is
    // now converted, or that declares no samplers at all, fails too — otherwise
    // the list rots into a permanent silencer, which is exactly what the
    // §5c allowlist above says an exception list must not become.
    // =========================================================================
    namespace
    {
        struct SlotBasedByDesign
        {
            std::string_view File;
            std::string_view Reason;
        };

        // Paths are relative to assets/shaders, with '/' separators.
        //
        // `to_array` rather than a sized `std::array`: a hand-written count that
        // drifts leaves DEFAULT-CONSTRUCTED entries at the end, and an empty File
        // matches no shader — so the list would quietly start reporting a phantom
        // " is listed but does not exist". (It did, on the first run of this test,
        // which is a small vote of confidence in checking the table both ways.)
        constexpr auto kSlotBasedByDesign = std::to_array<SlotBasedByDesign>({
            // ---- 1. A SHARED HEADER CONVERTS ALL-OR-NOTHING -----------------
            // Its own `#ifdef OLO_BINDLESS` IS the route opt-in token, so a
            // converted declaration here drags EVERY includer onto the raw-GLSL
            // route — present and future, whether or not that shader wanted to
            // give up SPIR-V reflection and the cache tiers. The slot is bound
            // unconditionally instead: every binding named here appears in
            // SlotAlwaysReceivesARealBind above, and that pairing is what makes
            // this a mechanism rather than an excuse.
            //
            // WORTH KNOWING BEFORE ANYONE "FIXES" THIS: four of the five now have
            // an all-converted includer set, so converting them would work today.
            // The reason not to is that it would make the compile route a property
            // of a HEADER rather than of a shader, after which `#include` becomes
            // the one edit that silently changes where a shader compiles.
            // WindSampling still has a slot-based includer
            // (compute/Particle_Simulate.comp), so for that one the original
            // argument also still applies literally.
            { "include/AtmosphereShading.glsl",
              "shared header; TEX_CLOUD_SHADOW is published+bound in BindShadowTextures" },
            { "include/CloudscapeCommon.glsl",
              "shared header; the three cloud-noise slots are published+bound in RenderPipeline" },
            { "include/DDGICommon.glsl",
              "shared header; the three DDGI atlases are published+bound in DDGIProbeUpdatePass" },
            { "include/WindSampling.glsl",
              "shared header; TEX_WIND_FIELD is bound directly by WindSystem::BindWindTexture" },
            { "include/VirtualDebugViz.glsl",
              "shared header, STORAGE IMAGES: bound directly by RenderCommand::BindImageTexture in "
              "VirtualGeometryPass, which never consults the seam" },

            // ---- 2. A SAMPLER TARGET WITH NO RESERVED NULL -------------------
            // A heap handle is typed by TARGET, and every unset input lands on
            // the reserved null for its target (BindlessHeap.glsl's
            // OLO_HEAP_TYPED_NULL). There is no 1x1 MULTISAMPLE texture in that
            // set and no OLO_HEAP_TEX_2D_MS to reach it with, so converting this
            // shader would reintroduce by hand the exact wrongly-typed-null
            // defect that cost four wrong diagnoses on the rebase pop. Adding the
            // target is a bounded change; doing it for one shader on the MSAA
            // deferred path is not what closes this bucket.
            { "DeferredLighting_MSAA.glsl",
              "sampler2DMS: no reserved multisample null and no OLO_HEAP_TEX_* form to reach one" },

            // ---- 3. THE HEAP REPLACES THE MECHANISM, NOT THE DECLARATION -----
            // `sampler2D u_Textures[32]` selected by a 32-case switch on a
            // per-vertex index is a workaround for exactly the limit bindless
            // removes. The right conversion carries a heap OFFSET per quad and
            // indexes g_OloResourceHeap directly, deleting the array AND the
            // switch — a vertex-format change, not a declaration wrap, and one
            // that belongs with the Vulkan shader path (Phase 6) where the payoff
            // is real.
            //
            // Its sibling Renderer2D_Text.glsl IS converted: it declares two
            // ordinary samplers, and Renderer2D's bind loop now runs through the
            // seam, so the quad shader takes the fallback in the same loop that
            // stages offsets for the text one. That pairing is why this entry is
            // about the ARRAY and not about the 2D path.
            { "Renderer2D_Quad.glsl",
              "2D batcher's 32-slot sampler array: bindless replaces the mechanism, so the conversion "
              "is a vertex-format change (Phase 6)" },
        });

        // ---- 4. A HARNESS FIXTURE HAS NO SEAM TO STAGE THROUGH ---------------
        // A PREFIX rule rather than nine entries, because it is a property of the
        // directory: everything under tests/ is driven by a shader-MATH property
        // test that binds its own textures outside the render graph, inside
        // ScopedSlotBasedShaders. See its comment in RenderPropertyTest.h for why
        // those harnesses opt out — the heap's lifetimes assume a frame boundary
        // they never reach, and every attempt to make them participate produced a
        // different order-dependent failure.
        //
        // Deliberately NOT staleness-checked the way the table above is: a new
        // fixture should inherit the reason rather than need an entry, and a
        // fixture that grows or loses a sampler is not a decision anyone owes.
        [[nodiscard]] bool IsHarnessFixture(const std::string& relative)
        {
            return relative.starts_with("tests/");
        }
    } // namespace

    TEST(BindlessShaderPipeline, EveryShaderIsOnTheRouteOrExplicitlyExcluded)
    {
        namespace fs = std::filesystem;

        const fs::path shaderRoot = fs::path{ OLO_TEST_EDITOR_ROOT } / "assets" / "shaders";
        ASSERT_TRUE(fs::exists(shaderRoot)) << "shader root not found: " << shaderRoot.string();

        static const std::regex kAnySampler(
            R"(uniform\s+(?:writeonly\s+|readonly\s+|coherent\s+|restrict\s+)*(?:i|u)?(?:sampler|image)\w*\s+\w+)");

        std::map<std::string, std::string_view> excluded;
        for (const SlotBasedByDesign& e : kSlotBasedByDesign)
        {
            excluded.emplace(std::string{ e.File }, e.Reason);
        }

        std::vector<std::string> undecided;    // declares samplers, not converted, no reason
        std::vector<std::string> staleEntries; // has a reason it no longer needs
        std::set<std::string> matchedEntries;
        u32 converted = 0;

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

            const std::string relative = fs::relative(p, shaderRoot).generic_string();

            // The shader's OWN text, not the include-resolved one: a file is on
            // the route because IT opts in, and resolving includes first would
            // let a converted header make every includer look converted.
            const std::string own = BlankComments(ReadWholeFile(p));
            const bool onRoute = WantsBindlessVariant(own);
            const bool declaresSampler = std::regex_search(own, kAnySampler);

            const auto it = excluded.find(relative);
            const bool listed = (it != excluded.end());
            if (listed)
            {
                matchedEntries.insert(relative);
            }

            if (onRoute)
            {
                ++converted;
                if (listed)
                {
                    staleEntries.push_back(relative + " is ON the bindless route but still listed as "
                                                      "slot-based-by-design");
                }
                continue;
            }

            if (!declaresSampler)
            {
                if (listed)
                {
                    staleEntries.push_back(relative + " declares no sampler or image, so it has nothing "
                                                      "to exclude");
                }
                continue;
            }

            if (!listed && !IsHarnessFixture(relative))
            {
                undecided.push_back(relative);
            }
        }

        for (const SlotBasedByDesign& e : kSlotBasedByDesign)
        {
            if (!matchedEntries.contains(std::string{ e.File }))
            {
                staleEntries.push_back(std::string{ e.File } + " is listed but does not exist");
            }
        }

        // Floor guard: a scan that found nothing would satisfy every assertion below.
        EXPECT_GT(converted, 80u) << "only " << converted
                                  << " shaders detected on the bindless route — the scan is broken, not the tree";

        // Echo the standing decisions on every run. The reasons are the point of
        // the table, and a reason nobody ever reads is a comment pretending to be
        // data — this is what gives them a consumer.
        std::string decisions;
        for (const auto& [file, reason] : excluded)
        {
            decisions += std::string{ "\n    " } + file + " — " + std::string{ reason };
        }
        GTEST_LOG_(INFO) << converted << " shaders on the bindless route; slot-based by design:" << decisions;

        std::string report;
        for (const std::string& u : undecided)
        {
            report += "\n    " + u;
        }
        EXPECT_TRUE(undecided.empty())
            << "These shaders declare a sampler or image, are NOT on the bindless route, and carry no\n"
               "recorded reason. That is not a bug — an unconverted shader gets a real bind and renders\n"
               "correctly — it is an unmade decision, and it is what keeps Phase 3 from having an end.\n"
               "Either convert it (glsl-shaders.md §5a, and move its C++ bind in the SAME change per §5c),\n"
               "or add it to kSlotBasedByDesign with the reason it stays."
            << report;

        std::string stale;
        for (const std::string& s : staleEntries)
        {
            stale += "\n    " + s;
        }
        EXPECT_TRUE(staleEntries.empty())
            << "kSlotBasedByDesign has drifted from the tree. An exception list that is not checked in\n"
               "BOTH directions decays into a way to silence this test."
            << stale;
    }

    // §7a-bis — THE COMPILE ROUTE IS OBSERVABLE THROUGH THE DEPTH BUFFER, not only
    // through bindings. `invariant gl_Position` is a promise between two
    // PROGRAMS: a depth prepass writes depth, a colour pass re-tests it at
    // GL_LEQUAL. `invariant` constrains the optimizations of ONE compiler; it
    // cannot make shaderc -> SPIR-V -> SPIRV-Cross and the driver's own GLSL
    // front-end round the same expression identically. So every shader carrying
    // that declaration must be on the SAME route as the others, all or none.
    //
    // This is a DIFFERENT rule from §5c and it is not implied by it. A depth
    // prepass declares no samplers at all, so §5c has nothing to say about it
    // and it would never mention OLO_BINDLESS on its own — it is exactly the
    // file a sampler-driven sweep leaves behind. That is what happened:
    // converting the material bucket moved PBR_MultiLight to the raw route and
    // left DepthPrepass.glsl on the SPIR-V one, and the colour pass then failed
    // LEQUAL against depth its own prepass had written.
    //
    // The failure is worth describing because it does NOT look like a depth bug:
    // 23340 dropout pixels on a sphere and ZERO on the ground plane in the same
    // frame. A curved surface's steep per-pixel depth gradient turns a last-bit
    // disagreement into a visible hole; a flat one absorbs it. It reads as "that
    // mesh is broken", which is why it survived a shading-side hunt.
    TEST(BindlessShaderPipeline, DepthInvariantShadersAgreeOnTheCompileRoute)
    {
        namespace fs = std::filesystem;

        const fs::path shaderRoot = fs::path{ OLO_TEST_EDITOR_ROOT } / "assets" / "shaders";
        ASSERT_TRUE(fs::exists(shaderRoot)) << "shader root not found: " << shaderRoot.string();

        std::vector<std::string> onRoute;
        std::vector<std::string> offRoute;
        std::vector<std::string> unresolved;

        for (const auto& entry : fs::recursive_directory_iterator(shaderRoot))
        {
            if (!entry.is_regular_file())
            {
                continue;
            }
            const fs::path& p = entry.path();
            const std::string ext = p.extension().string();
            if ((ext != ".glsl" && ext != ".comp") || p.parent_path().filename() == "include")
            {
                continue;
            }

            std::set<std::string> seen;
            // Already comment-free: ResolveIncludes blanks each file as it reads it.
            const std::string resolved = ResolveIncludes(p, shaderRoot, seen, unresolved);
            // The declaration must be measured AFTER comments are blanked, for
            // the same reason WantsBindlessVariant scans outside comments: the
            // natural way to write "this pass deliberately has no invariant
            // contract" is to say so in prose next to the words.
            if (resolved.find("invariant gl_Position") == std::string::npos)
            {
                continue;
            }

            // Mirrors OpenGLShader::WantsBindlessVariant exactly. If that
            // predicate ever grows a third token this must grow it too, or the
            // guard silently stops matching the thing it guards.
            const bool takesRoute = WantsBindlessVariant(resolved);
            (takesRoute ? onRoute : offRoute).push_back(p.filename().string());
        }

        EXPECT_TRUE(unresolved.empty()) << "unresolvable #include(s) — the scan cannot see the declaration";
        // Floor guard: a scan that found no contract at all would pass vacuously.
        EXPECT_GE(onRoute.size() + offRoute.size(), 5u)
            << "no depth-invariant shaders found — the scan is broken, not the codebase";

        std::string split;
        if (!onRoute.empty() && !offRoute.empty())
        {
            split += "\n  on the bindless route:";
            for (const std::string& s : onRoute)
            {
                split += "\n    " + s;
            }
            split += "\n  still on the SPIR-V route:";
            for (const std::string& s : offRoute)
            {
                split += "\n    " + s;
            }
        }
        EXPECT_TRUE(onRoute.empty() || offRoute.empty())
            << "The depth-invariance contract group is SPLIT across compile routes, so the two\n"
               "programs no longer agree bit-for-bit on gl_Position and the colour pass will fail\n"
               "LEQUAL against its own prepass in blotches — on curved geometry only.\n"
               "A shader with no samplers of its own opts in with OLO_BINDLESS_ROUTE_PARITY."
            << split;
    }
} // namespace OloEngine::Tests
