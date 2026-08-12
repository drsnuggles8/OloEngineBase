// OLO_TEST_LAYER: unit
//
// #691 Phase 4: the runtime backend-selection chain (ADR 0011 §2) —
// `--rhi=<name>` flag → config-file fallback → OpenGL default. Pure parse
// logic, fully headless: SelectRendererBackend touches no GL/Vulkan state and
// never writes RendererAPI::s_API itself (Application applies the result), so
// these tests cannot perturb the process-wide backend other suites read.

#include <gtest/gtest.h>

#include "OloEngine/Renderer/BackendSelection.h"

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>

#ifdef _WIN32
#include <process.h> // _getpid()
#else
#include <unistd.h>
#endif

namespace
{
    using namespace OloEngine;

    [[nodiscard]] long long CurrentPid()
    {
#ifdef _WIN32
        return static_cast<long long>(::_getpid());
#else
        return static_cast<long long>(::getpid());
#endif
    }

    // argv helper: string literals are never written through, and GLFW-style
    // main() argv is char**, so the const_cast mirrors what EntryPoint passes.
    template<sizet N>
    BackendSelection Select(std::array<const char*, N> args, const std::filesystem::path& configFile = {})
    {
        std::array<char*, N> argv{};
        for (sizet i = 0; i < N; ++i)
        {
            argv[i] = const_cast<char*>(args[i]);
        }
        return SelectRendererBackend(static_cast<int>(N), argv.data(), configFile);
    }

    class ScopedConfigFile
    {
      public:
        // Keyed by PID, not by gtest's random_seed: gtest only randomises that seed
        // when --gtest_shuffle is set, so it is 0 in every process here. Since
        // gtest_discover_tests gives each case its own ctest entry (its own process,
        // each starting the counter at 0), a seed-keyed name collided across the four
        // config-file cases the moment ctest ran them in parallel — one case then read
        // (or raced the deletion of) another's file. PID + counter is the repo-wide
        // convention for exactly this reason.
        explicit ScopedConfigFile(const std::string& contents)
            : m_Path(std::filesystem::temp_directory_path() /
                     ("olo-backend-selection-test-" + std::to_string(CurrentPid()) + "-" +
                      std::to_string(s_Counter++) + ".yaml"))
        {
            std::ofstream out(m_Path);
            out << contents;
        }
        ~ScopedConfigFile()
        {
            std::error_code ec;
            std::filesystem::remove(m_Path, ec);
        }
        [[nodiscard]] const std::filesystem::path& Path() const
        {
            return m_Path;
        }

      private:
        static inline int s_Counter = 0;
        std::filesystem::path m_Path;
    };

    TEST(BackendSelection, DefaultsToOpenGLWithNoFlagAndNoConfig)
    {
        const BackendSelection selection = Select<1>({ "app.exe" });
        EXPECT_EQ(selection.Api, RendererAPI::API::OpenGL);
        EXPECT_EQ(selection.Source, "default");
        EXPECT_TRUE(selection.Diagnostic.empty());
    }

    TEST(BackendSelection, FlagSelectsOpenGLExplicitly)
    {
        const BackendSelection selection = Select<2>({ "app.exe", "--rhi=opengl" });
        EXPECT_EQ(selection.Api, RendererAPI::API::OpenGL);
        EXPECT_EQ(selection.Source, "--rhi flag");
        EXPECT_TRUE(selection.Diagnostic.empty());
    }

    TEST(BackendSelection, FlagIsCaseInsensitive)
    {
        const BackendSelection selection = Select<2>({ "app.exe", "--rhi=OpenGL" });
        EXPECT_EQ(selection.Api, RendererAPI::API::OpenGL);
        EXPECT_TRUE(selection.Diagnostic.empty());
    }

    TEST(BackendSelection, VulkanRequestHonouredOrLoudlyDegraded)
    {
        const BackendSelection selection = Select<2>({ "app.exe", "--rhi=vulkan" });
#if OLO_WITH_VULKAN
        EXPECT_EQ(selection.Api, RendererAPI::API::Vulkan);
        EXPECT_TRUE(selection.Diagnostic.empty());
#else
        // Not compiled in: degrade to GL, but NEVER silently — the diagnostic is
        // what Application logs at error level.
        EXPECT_EQ(selection.Api, RendererAPI::API::OpenGL);
        EXPECT_FALSE(selection.Diagnostic.empty());
#endif
        EXPECT_EQ(selection.Source, "--rhi flag");
    }

    TEST(BackendSelection, UnknownFlagValueDegradesToOpenGLWithDiagnostic)
    {
        const BackendSelection selection = Select<2>({ "app.exe", "--rhi=metal" });
        EXPECT_EQ(selection.Api, RendererAPI::API::OpenGL);
        EXPECT_FALSE(selection.Diagnostic.empty());
    }

    TEST(BackendSelection, ConfigFileSuppliesTheFallback)
    {
        const ScopedConfigFile config("Renderer:\n  RHI: vulkan\n");
        const BackendSelection selection = Select<1>({ "app.exe" }, config.Path());
#if OLO_WITH_VULKAN
        EXPECT_EQ(selection.Api, RendererAPI::API::Vulkan);
        EXPECT_EQ(selection.Source, "config file");
#else
        EXPECT_EQ(selection.Api, RendererAPI::API::OpenGL);
        EXPECT_FALSE(selection.Diagnostic.empty());
#endif
    }

    TEST(BackendSelection, WellFormedConfigWithUnknownBackendDegradesLoudly)
    {
        // Distinct from the malformed-config case below: the YAML parses fine and
        // the Renderer.RHI key exists, but names no known backend. That is a USER
        // mistake worth a diagnostic (Application logs it at error level), with
        // Source naming where the rejected request came from — unlike malformed
        // YAML, which falls through silently to the default.
        const ScopedConfigFile config("Renderer:\n  RHI: directx12\n");
        const BackendSelection selection = Select<1>({ "app.exe" }, config.Path());
        EXPECT_EQ(selection.Api, RendererAPI::API::OpenGL);
        EXPECT_EQ(selection.Source, "config file");
        EXPECT_FALSE(selection.Diagnostic.empty());
    }

    TEST(BackendSelection, FlagWinsOverConfigFile)
    {
        const ScopedConfigFile config("Renderer:\n  RHI: vulkan\n");
        const BackendSelection selection = Select<2>({ "app.exe", "--rhi=opengl" }, config.Path());
        EXPECT_EQ(selection.Api, RendererAPI::API::OpenGL);
        EXPECT_EQ(selection.Source, "--rhi flag");
        EXPECT_TRUE(selection.Diagnostic.empty());
    }

    TEST(BackendSelection, MalformedConfigFallsBackToDefaultSilently)
    {
        const ScopedConfigFile config("Renderer: [unclosed\n  nonsense: {{{\n");
        const BackendSelection selection = Select<1>({ "app.exe" }, config.Path());
        EXPECT_EQ(selection.Api, RendererAPI::API::OpenGL);
        EXPECT_EQ(selection.Source, "default");
        EXPECT_TRUE(selection.Diagnostic.empty());
    }

    TEST(BackendSelection, AbsentConfigFileIsSilent)
    {
        const BackendSelection selection =
            Select<1>({ "app.exe" }, std::filesystem::path("definitely") / "not" / "here.yaml");
        EXPECT_EQ(selection.Api, RendererAPI::API::OpenGL);
        EXPECT_EQ(selection.Source, "default");
        EXPECT_TRUE(selection.Diagnostic.empty());
    }
} // namespace
