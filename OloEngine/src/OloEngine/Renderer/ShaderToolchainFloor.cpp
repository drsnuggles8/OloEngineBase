#include "OloEnginePCH.h"
#include "OloEngine/Renderer/ShaderToolchainFloor.h"

#include "OloEngine/Renderer/ShaderSourceScan.h"

#include <shaderc/shaderc.hpp>

#include <atomic>
#include <mutex>

namespace OloEngine
{
    namespace
    {
        std::atomic<std::uint64_t> s_RefusalCount{ 0 };

        // The Vulkan tier's compile options, reduced to what the probe needs.
        // The env and dialect are hand-encoded for the reason VulkanShader.cpp
        // spells out at length: `shaderc_env_version_vulkan_1_4` exists only in
        // an SDK-current shaderc, and a toolchain BELOW the floor is precisely
        // the one that lacks the enumerator — so a probe naming it would not
        // compile on the build it exists to diagnose.
        shaderc::CompileOptions ProbeOptions()
        {
            constexpr auto kShadercEnvVulkan14 =
                static_cast<shaderc_env_version>(ShaderToolchainFloor::kProbeTargetEnvVulkan14);
            static_assert(static_cast<u32>(kShadercEnvVulkan14) == 0x00404000u,
                          "VK_MAKE_API_VERSION(0, 1, 4, 0) is a fixed encoding");
            constexpr auto kShadercSpirv16 =
                static_cast<shaderc_spirv_version>(ShaderToolchainFloor::kProbeTargetSpirv16);
            static_assert(static_cast<u32>(kShadercSpirv16) == 0x00010600u,
                          "SPIR-V version words are (major << 16) | (minor << 8)");

            shaderc::CompileOptions options;
            options.SetTargetEnvironment(shaderc_target_env_vulkan, kShadercEnvVulkan14);
            options.SetTargetSpirv(kShadercSpirv16);
            // Same rule as both production tiers: shaderc's message parser
            // asserts on malformed glslang warning strings.
            options.SetSuppressWarnings();
            return options;
        }

        ShaderToolchainReport Probe()
        {
            ShaderToolchainReport report;
            shaderc::Compiler compiler;
            if (!compiler.IsValid())
            {
                report.Missing.emplace_back("a usable shaderc compiler instance");
                report.Diagnostic = "shaderc::Compiler failed to initialise";
                return report;
            }

            const auto attempt = [&compiler](const std::string& source, const char* name)
            {
                return compiler.CompileGlslToSpv(source, shaderc_glsl_fragment_shader, name, ProbeOptions());
            };

            // Per extension first, so the refusal can name the one that is
            // missing rather than "something in this shader".
            for (const std::string_view extension : ShaderToolchainFloor::kRequiredExtensions)
            {
                const std::string source = ShaderToolchainFloor::ExtensionProbeSource(extension);
                const std::string name(extension);
                const auto result = attempt(source, name.c_str());
                if (result.GetCompilationStatus() != shaderc_compilation_status_success)
                {
                    report.Missing.emplace_back(name);
                    if (report.Diagnostic.empty())
                    {
                        report.Diagnostic = result.GetErrorMessage();
                    }
                }
            }

            // ...then the qualifiers themselves. A glslang that knows the
            // extension NAME but not `descriptor_heap` / `descriptor_stride`
            // would pass every loop above and still fail on the real shader,
            // which is the half of #1139's diagnostic that named
            // DescriptorHeapTextures.glsl rather than the `#extension` line.
            if (report.Missing.empty())
            {
                const std::string source(ShaderToolchainFloor::kLayoutProbeSource);
                const auto result = attempt(source, "olo-shader-toolchain-floor-probe");
                if (result.GetCompilationStatus() != shaderc_compilation_status_success)
                {
                    report.Missing.emplace_back(std::string(ShaderToolchainFloor::kLayoutProbeName));
                    report.Diagnostic = result.GetErrorMessage();
                }
            }

            report.Satisfied = report.Missing.empty();
            return report;
        }
    } // namespace

    const ShaderToolchainReport& ShaderToolchainFloor::Report()
    {
        // Function-local static: the answer describes the linked library, so it
        // is fixed for the life of the process and worth computing once. Not a
        // namespace-scope global — Ref.h's allocator singleton note (ADR 0004)
        // and this file's own place in static-init order both argue against one.
        static const ShaderToolchainReport s_Report = Probe();
        return s_Report;
    }

    bool ShaderToolchainFloor::SourceNeedsFloor(std::string_view source)
    {
        // Whole-identifier, comment-skipping — the scanner the two backends
        // already share. A plain substring search would opt in every shader that
        // merely INCLUDES DescriptorHeapTextures.glsl, whose header comment names
        // the extension four times.
        for (const std::string_view extension : kRequiredExtensions)
        {
            if (ShaderSourceScan::MentionsOutsideComments(source, extension))
            {
                return true;
            }
        }
        return false;
    }

    bool ShaderToolchainFloor::RefuseIfBelowFloor(std::string_view source, std::string_view shaderName)
    {
        return RefuseIfBelowFloor(Report(), source, shaderName);
    }

    bool ShaderToolchainFloor::RefuseIfBelowFloor(const ShaderToolchainReport& report, std::string_view source,
                                                  std::string_view shaderName)
    {
        if (report.Satisfied || !SourceNeedsFloor(source))
        {
            return false;
        }

        s_RefusalCount.fetch_add(1, std::memory_order_relaxed);

        std::string missing;
        for (const std::string& entry : report.Missing)
        {
            if (!missing.empty())
            {
                missing.append(", ");
            }
            missing.append(entry);
        }

        // Names the MISSING set once, rather than pairing it with a "declares X"
        // clause. The earlier phrasing read "declares GL_EXT_descriptor_heap but
        // does not support GL_EXT_descriptor_heap" — repetitive, and a lie for a
        // shader whose only declaration is the other extension, because the
        // "declares" half was the required list's first entry rather than
        // anything this source says.
        OLO_CORE_ERROR("Shader toolchain below the floor: '{}' needs the descriptor-heap contract, but the linked "
                       "shaderc/glslang does not support {}. Install Vulkan SDK {} or newer (equivalently shaderc "
                       "{}); there is no non-heap fallback, by decision — see ADR 0011 amendment (97). "
                       "Toolchain diagnostic: {}",
                       shaderName, missing, kMinimumVulkanSdk, kMinimumShadercTag, report.Diagnostic);
        return true;
    }

    std::uint64_t ShaderToolchainFloor::RefusalCount()
    {
        return s_RefusalCount.load(std::memory_order_relaxed);
    }

    void ShaderToolchainFloor::ResetRefusalCountForTesting()
    {
        s_RefusalCount.store(0, std::memory_order_relaxed);
    }
} // namespace OloEngine
