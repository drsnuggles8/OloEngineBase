#include "OloEnginePCH.h"

// OLO_TEST_LAYER: unit
// =============================================================================
// TextureSourcePathResolutionTest — issue #1067.
//
// Texture2D::ResolveStoredSourcePath is the whole of that bug's fix: every
// backend's Reload() re-reads the path this returns. The defect was that
// Reload() used the stored source path verbatim, and the asset system stores
// the PROJECT-RELATIVE spelling ("Assets/Textures/Foo.png") on purpose — so the
// re-read resolved against the process working directory (OloEditor/, one level
// above the project) and could never find the file. Hot-reload was dead for
// every texture that came through TextureSerializer.
//
// Why this file is CPU-only, and why that matters: the test that exercises the
// real thing (TextureInPlaceReloadTest) needs a GL 4.6 context, so CI SKIPs it
// and the regression could come back green. The resolution rule is pure path
// logic with no GPU in it, so it can be pinned here and actually run on every
// push. See docs/agent-rules/substituted-seams-compound.md — the point is to
// cover the seam that broke, not a friendlier neighbour of it.
//
// No GPU, no textures: the helper is a static that takes a string.
// =============================================================================

#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/Texture.h"

#include <gtest/gtest.h>

#include "TestTempDir.h"

#include <filesystem>
#include <fstream>
#include <system_error>

namespace OloEngine::Tests
{
    namespace
    {
        void WriteFile(const std::filesystem::path& path)
        {
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            ASSERT_TRUE(out.is_open()) << "could not create " << path.string();
            out << "not really a png";
        }

        // A project rooted in this case's temp directory, which is never the
        // process working directory — that difference is the bug.
        std::filesystem::path MakeProject(const char* label)
        {
            const std::filesystem::path dir = TempDir(label);
            std::error_code ec;
            std::filesystem::create_directories(dir / "Assets", ec);

            {
                std::ofstream proj(dir / "PathResolution.oloproj");
                EXPECT_TRUE(proj.is_open()) << "could not write the temp .oloproj";
                proj << "Project:\n"
                        "  Name: PathResolution\n"
                        "  StartScene: \"\"\n"
                        "  AssetDirectory: \"Assets\"\n"
                        "  ScriptModulePath: \"\"\n";
            }

            EXPECT_TRUE(Project::Load(dir / "PathResolution.oloproj"))
                << "Project::Load failed for the temp project at " << dir.string();
            return dir;
        }

        struct ProjectScope
        {
            ~ProjectScope()
            {
                // Leave the no-project state behind: these cases install a project
                // into a process-wide static, and the next test must not inherit a
                // project rooted in a temp directory that is about to disappear.
                Project::Unload();
            }
        };

        // Runs a case with the working directory moved somewhere this test owns,
        // restoring it on every exit path. Needed because one case has to exercise
        // "relative to the CWD" — and writing a probe file into the real working
        // directory would drop a fixed-name directory in the repo root, which is the
        // shared-mutable-path pattern TestTempDir.h (and its pre-commit hook) forbid.
        struct ScopedWorkingDirectory
        {
            explicit ScopedWorkingDirectory(const std::filesystem::path& to)
                : Previous(std::filesystem::current_path())
            {
                std::filesystem::current_path(to);
            }

            ~ScopedWorkingDirectory()
            {
                std::error_code ec;
                std::filesystem::current_path(Previous, ec);
            }

            std::filesystem::path Previous;
        };
    } // namespace

    // The #1067 case itself: the asset system's project-relative spelling resolves
    // against the project directory, not the CWD.
    TEST(TextureSourcePathResolution, ResolvesAProjectRelativePathAgainstTheProjectDirectory)
    {
        ProjectScope scope;
        const std::filesystem::path projectDir = MakeProject("relative_resolves");
        ASSERT_FALSE(::testing::Test::HasFatalFailure());

        const std::filesystem::path relative = std::filesystem::path("Assets") / "Textures" / "Foo.png";
        WriteFile(projectDir / relative);
        ASSERT_FALSE(::testing::Test::HasFatalFailure());

        ASSERT_NE(projectDir, std::filesystem::current_path())
            << "the temp project is the working directory, so this case could pass "
               "with the bug present.";

        const std::filesystem::path resolved = Texture2D::ResolveStoredSourcePath(relative.string());
        EXPECT_EQ(resolved, projectDir / relative);
    }

    // An absolute path is what Texture2D::Create(path) stores; it must survive
    // untouched — this is the spelling that always worked, and the fix must not
    // start prefixing it.
    TEST(TextureSourcePathResolution, LeavesAnAbsolutePathAlone)
    {
        ProjectScope scope;
        const std::filesystem::path projectDir = MakeProject("absolute_untouched");
        ASSERT_FALSE(::testing::Test::HasFatalFailure());

        const std::filesystem::path absolute = projectDir / "Assets" / "Textures" / "Bar.png";
        WriteFile(absolute);
        ASSERT_FALSE(::testing::Test::HasFatalFailure());

        EXPECT_EQ(Texture2D::ResolveStoredSourcePath(absolute.string()), absolute);
    }

    // A path that exists relative to the working directory but not under the
    // project keeps working — engine-owned textures (editor icons) are loaded that
    // way, and the fix must not break their reload.
    TEST(TextureSourcePathResolution, FallsBackToAWorkingDirectoryRelativePathThatExists)
    {
        ProjectScope scope;
        const std::filesystem::path projectDir = MakeProject("cwd_relative");
        ASSERT_FALSE(::testing::Test::HasFatalFailure());

        // A working directory this case owns, so the probe below is not written into
        // the repo root. It is deliberately NOT the project directory.
        const std::filesystem::path workingDir = TempDir("cwd_relative_cwd");
        const ScopedWorkingDirectory movedCwd(workingDir);
        ASSERT_NE(std::filesystem::current_path(), projectDir);

        // Authored under the CWD, deliberately absent from the project tree.
        const std::filesystem::path relative = std::filesystem::path("Icons") / "Icon.png";
        WriteFile(std::filesystem::current_path() / relative);
        ASSERT_FALSE(::testing::Test::HasFatalFailure());
        ASSERT_FALSE(std::filesystem::exists(projectDir / relative))
            << "the probe leaked into the project tree, so this case proves nothing.";

        EXPECT_EQ(Texture2D::ResolveStoredSourcePath(relative.string()), relative);
    }

    // Unresolvable input refuses loudly rather than handing the loader a path that
    // silently reads against the CWD — the failure mode #1067 was.
    TEST(TextureSourcePathResolution, RefusesARelativePathThatExistsNowhere)
    {
        ProjectScope scope;
        MakeProject("missing_refuses");
        ASSERT_FALSE(::testing::Test::HasFatalFailure());

        const std::filesystem::path missing =
            std::filesystem::path("Assets") / "Textures" / "NotHere_1067.png";
        EXPECT_TRUE(Texture2D::ResolveStoredSourcePath(missing.string()).empty());
    }

    // With no project open there is no base to resolve against; a relative path
    // must not fall through to the CWD.
    TEST(TextureSourcePathResolution, RefusesARelativePathWhenNoProjectIsOpen)
    {
        ProjectScope scope;
        Project::Unload();

        const std::filesystem::path relative = std::filesystem::path("Assets") / "Textures" / "Foo.png";
        EXPECT_TRUE(Texture2D::ResolveStoredSourcePath(relative.string()).empty());
    }

    // An empty source path is the "spec-created texture" case: nothing to re-read,
    // and no error worth logging.
    TEST(TextureSourcePathResolution, ReturnsEmptyForAnEmptySourcePath)
    {
        EXPECT_TRUE(Texture2D::ResolveStoredSourcePath("").empty());
    }
} // namespace OloEngine::Tests
