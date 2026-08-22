// OLO_TEST_LAYER: unit
//
// #691: the runtime backend-selection chain (ADR 0011 §2) —
// `--rhi=<name>` flag → config-file fallback → OpenGL default. Pure parse
// logic, fully headless: SelectRendererBackend touches no GL/Vulkan state and
// never writes RendererAPI::s_API itself (Application applies the result), so
// these tests cannot perturb the process-wide backend other suites read.

#include <gtest/gtest.h>
#include "TestTempDir.h"

#include "OloEngine/Renderer/BackendSelection.h"

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace
{
    using namespace OloEngine;

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
        // The helper isolates by process and by gtest case; the counter only keeps
        // several config files WITHIN one case apart. It must not be keyed by
        // gtest's random_seed — gtest randomises that only under --gtest_shuffle, so
        // it is 0 in every process here, and a seed-keyed name collided across the
        // four config-file cases the moment ctest ran them in parallel.
        explicit ScopedConfigFile(const std::string& contents)
            : m_Path(OloEngine::Tests::TempFile("config-" + std::to_string(s_Counter++) + ".yaml"))
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

    // #691: the writer and the parser share one schema owner. This is
    // the drift gate — if either side changes shape, the round-trip breaks
    // here rather than in a shipped game's config directory.
    TEST(BackendSelection, WriteRendererConfigRoundTripsThroughTheParser)
    {
        const auto path = OloEngine::Tests::TempFile("write-roundtrip.yaml");
        ASSERT_TRUE(WriteRendererConfig(path, RendererAPI::API::Vulkan));
        const BackendSelection vulkan = Select<1>({ "app.exe" }, path);
#if OLO_WITH_VULKAN
        EXPECT_EQ(vulkan.Api, RendererAPI::API::Vulkan);
        EXPECT_TRUE(vulkan.Diagnostic.empty());
#else
        EXPECT_EQ(vulkan.Api, RendererAPI::API::OpenGL);
        EXPECT_FALSE(vulkan.Diagnostic.empty());
#endif
        EXPECT_EQ(vulkan.Source, "config file");

        ASSERT_TRUE(WriteRendererConfig(path, RendererAPI::API::OpenGL));
        const BackendSelection opengl = Select<1>({ "app.exe" }, path);
        EXPECT_EQ(opengl.Api, RendererAPI::API::OpenGL);
        EXPECT_EQ(opengl.Source, "config file");
        EXPECT_TRUE(opengl.Diagnostic.empty());

        std::error_code ec;
        std::filesystem::remove(path, ec);
    }

    // #691: config-path resolution — cwd wins when its file exists,
    // the exe-dir file rescues a wrong working directory, and the creation
    // default stays cwd-anchored so a fresh editor write cannot land in the
    // build output tree.
    TEST(BackendSelection, ConfigPathResolutionPrefersBaseThenExeDirThenBaseDefault)
    {
        const auto root = OloEngine::Tests::TempFile("cfg-resolve");
        const auto base = root / "cwd";
        const auto exeDir = root / "exe";
        const auto relative = std::filesystem::path("config") / "renderer.yaml";
        std::filesystem::create_directories(base / "config");
        std::filesystem::create_directories(exeDir / "config");

        // Neither file exists: creation default is base-anchored.
        EXPECT_EQ(ResolveRendererConfigPath(base, exeDir), base / relative);

        // Only the exe-dir file exists (packaged game, wrong "Start in"): rescued.
        {
            std::ofstream out(exeDir / relative);
            out << "Renderer:\n  RHI: opengl\n";
        }
        EXPECT_EQ(ResolveRendererConfigPath(base, exeDir), exeDir / relative);

        // An empty exe dir (platform could not answer) falls back to base.
        EXPECT_EQ(ResolveRendererConfigPath(base, {}), base / relative);

        // Both exist: base (cwd) wins — the editor's own config stays authoritative.
        {
            std::ofstream out(base / relative);
            out << "Renderer:\n  RHI: opengl\n";
        }
        EXPECT_EQ(ResolveRendererConfigPath(base, exeDir), base / relative);

        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
} // namespace
