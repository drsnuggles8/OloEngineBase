// =============================================================================
// ShaderToolchainProbe.cpp — the configure-time half of the shader-toolchain
// floor (issue #1139, ADR 0011 amendment (97)).
//
// `cmake/ShaderToolchainFloor.cmake` builds and runs this against the SAME
// shaderc the engine is about to link, so a build that cannot compile
// `GL_EXT_descriptor_heap` says so in the configure output — not 4,000 tests
// later, in a glslang diagnostic that names a line in an include file.
//
// IT ALWAYS EXITS 0 AND REPORTS THROUGH STDOUT. try_run's exit code is not a
// usable channel here: this program is compiled with the project's flags, so
// under ASan/LSan a leak inside shaderc, and under TSan any race it reports,
// would set a non-zero status and be read as "toolchain below the floor" — a
// FATAL_ERROR for a perfectly good toolchain. A verdict line cannot be produced
// by accident.
// =============================================================================

#include "OloEngine/Renderer/ShaderToolchainFloor.h"

#include <shaderc/shaderc.hpp>

#include <cstdio>
#include <string>
#include <string_view>

namespace
{
    constexpr const char* kVerdictPrefix = "OLO_SHADER_TOOLCHAIN_FLOOR:";

    shaderc::CompileOptions ProbeOptions()
    {
        // Hand-encoded, from the header's shared constants: a toolchain below
        // the floor is precisely the one whose shaderc lacks the
        // `shaderc_env_version_vulkan_1_4` enumerator, so naming it would make
        // this probe fail to COMPILE on the build it exists to diagnose.
        shaderc::CompileOptions options;
        options.SetTargetEnvironment(
            shaderc_target_env_vulkan,
            static_cast<shaderc_env_version>(OloEngine::ShaderToolchainFloor::kProbeTargetEnvVulkan14));
        options.SetTargetSpirv(
            static_cast<shaderc_spirv_version>(OloEngine::ShaderToolchainFloor::kProbeTargetSpirv16));
        options.SetSuppressWarnings();
        return options;
    }

    void ReportMissing(std::string_view what, const std::string& diagnostic)
    {
        std::printf("%s missing %.*s\n%s\n", kVerdictPrefix, static_cast<int>(what.size()), what.data(),
                    diagnostic.c_str());
    }
} // namespace

int main()
{
    using OloEngine::ShaderToolchainFloor;

    shaderc::Compiler compiler;
    if (!compiler.IsValid())
    {
        ReportMissing("a usable shaderc compiler instance", "shaderc::Compiler failed to initialise");
        return 0;
    }

    for (const std::string_view extension : ShaderToolchainFloor::kRequiredExtensions)
    {
        const std::string source = ShaderToolchainFloor::ExtensionProbeSource(extension);
        const std::string name(extension);
        const auto result =
            compiler.CompileGlslToSpv(source, shaderc_glsl_fragment_shader, name.c_str(), ProbeOptions());
        if (result.GetCompilationStatus() != shaderc_compilation_status_success)
        {
            ReportMissing(extension, result.GetErrorMessage());
            return 0;
        }
    }

    {
        const std::string source(ShaderToolchainFloor::kLayoutProbeSource);
        const auto result = compiler.CompileGlslToSpv(source, shaderc_glsl_fragment_shader,
                                                      "olo-shader-toolchain-floor-probe", ProbeOptions());
        if (result.GetCompilationStatus() != shaderc_compilation_status_success)
        {
            ReportMissing(ShaderToolchainFloor::kLayoutProbeName, result.GetErrorMessage());
            return 0;
        }
    }

    std::printf("%s ok\n", kVerdictPrefix);
    return 0;
}
