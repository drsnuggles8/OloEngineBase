#pragma once

// =============================================================================
// ShaderToolchainFloor.h — the minimum shader toolchain the Vulkan tier needs,
// and the one place that names it (issue #1139, ADR 0011 amendment (97)).
//
// THE RULE. Amendments (95) and (96) put `#extension GL_EXT_descriptor_heap :
// require` in production shaders. That is a HARD requirement on the compiler,
// exactly as `VK_EXT_descriptor_heap` is a hard requirement on the device
// (ADR 0010, VulkanCapabilities.h): a toolchain that cannot compile it is
// REFUSED, naming the missing capability. There is no gated fallback and no
// non-heap arm to fall back to — a shader that quietly dropped the five
// material-local maps would render wrong, not slow.
//
// WHY A PROBE AND NOT A VERSION NUMBER. shaderc's C/C++ API exposes no version
// query, and the version that matters is glslang's, which is linked INSIDE
// libshaderc. The only skew-free question is the behavioural one: hand the
// linked compiler the qualifiers and see what it says. Asking a *different*
// artefact — `glslc` on PATH, or a header's version macro — is how #1139
// happened in the first place: hosted CI carried an SDK whose `glslc` accepted
// the extension while the engine linked Ubuntu's libshaderc 2023.8, which did
// not.
//
// THIS HEADER IS DELIBERATELY DEPENDENCY-FREE — no `Core/Base.h`, no shaderc.
// `cmake/ShaderToolchainProbe.cpp` compiles against it at CONFIGURE time, before
// any engine target exists, so that the configure-time guard and the runtime
// refusal test the same source text rather than two copies that drift.
// =============================================================================

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace OloEngine
{
    // The verdict for one shader toolchain, shaped like VulkanCapabilityReport
    // for the same reason it is: an all-or-nothing contract reads better as a
    // verdict plus the list of what was not met than as a bag of booleans.
    struct ShaderToolchainReport
    {
        // True iff every requirement below was met. All-or-nothing: a toolchain
        // that supports some of the contract is refused, never partially used.
        bool Satisfied = false;

        // One human-readable entry per unmet requirement, for the refusal
        // message ("refuse, naming the missing capability" — ADR 0010).
        std::vector<std::string> Missing;

        // The toolchain's own diagnostic for the first unmet requirement,
        // verbatim. Printed with the refusal because glslang's message names the
        // line, and a reader who has never seen this contract needs it.
        std::string Diagnostic;
    };

    class ShaderToolchainFloor
    {
      public:
        // The lowest LunarG SDK this engine is TESTED against. It is not a
        // bisected boundary and does not pretend to be: SDK 1.4.321.0 is known
        // to reject the extension, 1.4.357.0 is what the dev box, Windows CI,
        // the self-hosted Linux box and `.github/actions/setup-vulkan` all
        // carry, and nothing in between has been tried. The PROBE below is the
        // authority; this string exists so the refusal can tell a human what to
        // install.
        static constexpr std::string_view kMinimumVulkanSdk = "1.4.357.0";

        // Equivalently, for anyone building the toolchain rather than installing
        // it: SDK 1.4.357.0's shader toolchain is shaderc v2026.3, whose DEPS
        // pins glslang 168d452a — byte-identical to glslang's own
        // `vulkan-sdk-1.4.357.0` tag. `GL_EXT_descriptor_heap` landed in glslang
        // on 2026-01-22 (c8d3e0661).
        static constexpr std::string_view kMinimumShadercTag = "v2026.3";

        // The extensions a production Vulkan-tier shader declares `: require`.
        // Probed one at a time so the refusal can name WHICH one is missing.
        static constexpr std::string_view kRequiredExtensions[] = {
            "GL_EXT_descriptor_heap",
            "GL_EXT_nonuniform_qualifier",
        };

        // A minimal replica of include/DescriptorHeapTextures.glsl's declarations
        // — the qualifiers, not just the `#extension` line. Both halves are
        // needed: an old glslang rejects the directive, and a glslang that knows
        // the NAME but not the layout identifiers would pass an
        // extension-only probe and still fail on the real shader.
        static constexpr std::string_view kLayoutProbeSource = R"GLSL(#version 460 core
#extension GL_EXT_descriptor_heap : require
#extension GL_EXT_nonuniform_qualifier : require
layout(descriptor_heap, descriptor_stride = 1) uniform texture2D g_OloFloorProbeTexture2D[];
layout(descriptor_heap, descriptor_stride = 1) uniform sampler g_OloFloorProbeSampler[];
layout(location = 0) out vec4 o_OloFloorProbeColor;
void main()
{
    o_OloFloorProbeColor = textureLod(sampler2D(g_OloFloorProbeTexture2D[nonuniformEXT(0)],
                                                g_OloFloorProbeSampler[nonuniformEXT(0)]),
                                      vec2(0.0), 0.0);
}
)GLSL";

        // What kLayoutProbeSource is called in the Missing list, so the
        // configure-time probe and the runtime one print the same words.
        static constexpr std::string_view kLayoutProbeName =
            "the descriptor_heap / descriptor_stride layout qualifiers";

        // The Vulkan tier's target env and SPIR-V dialect, as the fixed words
        // VulkanShader.cpp hand-encodes them. Named here because BOTH probes
        // must use them and a below-floor shaderc is exactly the build that
        // lacks the `shaderc_env_version_vulkan_1_4` enumerator, so neither
        // probe may name it. env versions are VK_MAKE_API_VERSION(0, M, m, 0);
        // SPIR-V version words are (major << 16) | (minor << 8).
        static constexpr std::uint32_t kProbeTargetEnvVulkan14 = (1u << 22) | (4u << 12);
        static constexpr std::uint32_t kProbeTargetSpirv16 = (1u << 16) | (6u << 8);

        // Build the `#extension <name> : require` probe for one extension.
        // INLINE, and that is what makes this header worth its dependency-free
        // discipline: `cmake/ShaderToolchainProbe.cpp` needs the definition at
        // configure time, when no engine object file exists to link against.
        //
        // `: require` and not `: enable`, because `: require` is what the
        // production shaders declare and it is the spelling that makes an
        // unsupported extension an ERROR rather than a warning glslang drops.
        [[nodiscard]] static std::string ExtensionProbeSource(std::string_view extension)
        {
            std::string source = "#version 460 core\n#extension ";
            source.append(extension);
            source.append(" : require\nvoid main() {}\n");
            return source;
        }

        // Evaluate the contract against the LINKED shaderc, once per process.
        // Never throws; the report carries the verdict. Compiling three tiny
        // shaders costs single-digit milliseconds and happens at most once.
        [[nodiscard]] static const ShaderToolchainReport& Report();

        // True when `source` reaches for something the floor covers, i.e. when a
        // below-floor toolchain would fail on THIS shader rather than on some
        // other one. Scanned as a whole identifier outside comments, so the
        // prose in DescriptorHeapTextures.glsl's header does not opt a shader in.
        [[nodiscard]] static bool SourceNeedsFloor(std::string_view source);

        // The refusal, in one place because both Vulkan shader tiers issue it.
        // Logs a named error, counts it, and returns true when the caller must
        // NOT attempt the compile. Returns false — silently — whenever the floor
        // is met or the source does not need it.
        [[nodiscard]] static bool RefuseIfBelowFloor(std::string_view source, std::string_view shaderName);

        // How many compiles this process refused. Countable, not merely loud:
        // a log line scrolls, a counter can be asserted on and reported.
        [[nodiscard]] static std::uint64_t RefusalCount();

        // Test seam. Resets the refusal counter only — never the cached report,
        // which describes the linked library and cannot change within a process.
        static void ResetRefusalCountForTesting();
    };
} // namespace OloEngine
