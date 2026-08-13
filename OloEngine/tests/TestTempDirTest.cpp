// OLO_TEST_LAYER: unit
// =============================================================================
// TestTempDirTest -- contracts of the suite's own scratch-path helper.
//
// Everything that writes a file during a test now routes through TestTempDir.h,
// so a regression here is a regression in test isolation across the whole suite
// (issue #789). The properties worth pinning are the ones a plain
// `temp_directory_path() / "name"` silently lacks.
// =============================================================================

#include "OloEnginePCH.h"
#include <gtest/gtest.h>
#include "TestTempDir.h"

#include <filesystem>
#include <fstream>
#include <string>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace
{
    [[nodiscard]] std::string CurrentPidString()
    {
#if defined(_WIN32)
        return std::to_string(static_cast<unsigned long>(::_getpid()));
#else
        return std::to_string(static_cast<unsigned long>(::getpid()));
#endif
    }
} // namespace

TEST(TestTempDir, RootIsAnExistingProcessOwnedDirectory)
{
    const std::filesystem::path& root = OloEngine::Tests::TempRoot();

    ASSERT_TRUE(std::filesystem::exists(root)) << root.string();
    EXPECT_TRUE(std::filesystem::is_directory(root));

    const std::string leaf = root.filename().string();

    // A flat `OloEngineTests-` prefix, NOT a shared `OloEngineTests/` parent: a
    // fixed-name directory in a sticky, world-writable /tmp cannot be created by
    // any account except the one that made it, and this repo's CI runs as its own
    // user alongside others. The prefix keeps a manual sweep just as easy.
    EXPECT_TRUE(leaf.starts_with("OloEngineTests-")) << leaf;
    EXPECT_NE("OloEngineTests", root.parent_path().filename().string())
        << "a shared fixed-name parent reintroduces the cross-user /tmp hazard";

    // Named after THIS process so a post-mortem can attribute leftovers...
    EXPECT_NE(std::string::npos, leaf.find("-" + CurrentPidString() + "_")) << leaf;

    // ...but PID alone is not enough: a recycled PID would collide with a crashed
    // run's leftover directory. The random tag is what makes the claim exclusive.
    EXPECT_GT(leaf.size(), std::string("OloEngineTests-").size() + CurrentPidString().size() + 1)
        << "the root leaf carries only a pid; PID reuse would let it adopt a stale tree";
}

TEST(TestTempDir, CaseDirectoryIsUnderTheRootAndNamedAfterTheCase)
{
    const std::filesystem::path dir = OloEngine::Tests::TempDir();

    ASSERT_TRUE(std::filesystem::is_directory(dir)) << dir.string();
    EXPECT_EQ(OloEngine::Tests::TempRoot(), dir.parent_path());
    EXPECT_NE(std::string::npos, dir.filename().string().find("CaseDirectoryIsUnderTheRoot"));
}

TEST(TestTempDir, LabelsGiveTheSameCaseSeveralIndependentDirectories)
{
    const std::filesystem::path plain = OloEngine::Tests::TempDir();
    const std::filesystem::path a = OloEngine::Tests::TempDir("alpha");
    const std::filesystem::path b = OloEngine::Tests::TempDir("beta");

    EXPECT_NE(plain, a);
    EXPECT_NE(a, b);
    EXPECT_TRUE(std::filesystem::is_directory(a));
    EXPECT_TRUE(std::filesystem::is_directory(b));

    // Same case + same label is stable, so a helper can re-derive the path
    // instead of threading it through.
    EXPECT_EQ(a, OloEngine::Tests::TempDir("alpha"));
}

// The clean slate is per TEST, not per call. Helpers like
// MeshCookingFactoryCacheTest's MakeUniqueScratchDir() call TempDir() repeatedly
// and rely on earlier content surviving; a wipe-every-call implementation would
// destroy it, and would do so silently.
TEST(TestTempDir, RepeatedCallsWithinOneTestKeepEarlierContent)
{
    const std::filesystem::path dir = OloEngine::Tests::TempDir("scratch");
    {
        std::ofstream out(dir / "first.txt");
        ASSERT_TRUE(out.is_open());
        out << "kept";
    }

    ASSERT_EQ(dir, OloEngine::Tests::TempDir("scratch"));
    EXPECT_TRUE(std::filesystem::exists(dir / "first.txt"))
        << "a second TempDir() call for the same label wiped the first call's content";

    // ...and a DIFFERENT label in the same test must not disturb it either.
    (void)OloEngine::Tests::TempDir("other");
    EXPECT_TRUE(std::filesystem::exists(dir / "first.txt"));
}

// Under `--gtest_repeat` this case runs several times in ONE process and resolves
// to the same directory each time, so a leftover from the previous iteration
// would make the EXPECT_FALSE below fail. That is the regression this asserts:
// the fixtures migrated in #789 dropped their `SetUp` remove_all on the promise
// that TempDir() hands back an EMPTY directory at the start of every test.
TEST(TestTempDir, TempFileHasAnExistingParentButDoesNotExistItself)
{
    const std::filesystem::path file = OloEngine::Tests::TempFile("config.yaml");

    EXPECT_TRUE(std::filesystem::is_directory(file.parent_path()));
    // A test that wants a guaranteed-missing path relies on this.
    EXPECT_FALSE(std::filesystem::exists(file));

    {
        std::ofstream out(file);
        ASSERT_TRUE(out.is_open()) << file.string();
        out << "value: 1\n";
    }
    ASSERT_TRUE(std::filesystem::exists(file));

    std::ifstream in(file);
    std::string line;
    ASSERT_TRUE(std::getline(in, line));
    EXPECT_EQ("value: 1", line);
}

// -----------------------------------------------------------------------------
// A value-parameterised case's gtest name contains a '/' (here
// "SanitizesNames/TestTempDirNaming.LeafIsFlat/0"). Unsanitised, that would turn
// one directory name into a nested path — silently on POSIX, and as an outright
// failure on Windows. This is the case that would have caught it.
// -----------------------------------------------------------------------------
class TestTempDirNaming : public ::testing::TestWithParam<int>
{
};

TEST_P(TestTempDirNaming, LeafIsFlatAndFilesystemSafe)
{
    const std::filesystem::path dir = OloEngine::Tests::TempDir();

    ASSERT_TRUE(std::filesystem::is_directory(dir)) << dir.string();
    EXPECT_EQ(OloEngine::Tests::TempRoot(), dir.parent_path())
        << "a '/' in the gtest case name leaked into the path as a directory separator";

    const std::string leaf = dir.filename().string();
    for (const char c : leaf)
    {
        const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
        EXPECT_TRUE(safe) << "unsafe character '" << c << "' in leaf '" << leaf << "'";
    }
}

INSTANTIATE_TEST_SUITE_P(SanitizesNames, TestTempDirNaming, ::testing::Values(0, 1));
