// OLO_TEST_LAYER: unit
// =============================================================================
// ScopedFileRestoreTest -- the three states the registry guard distinguishes.
//
// ScopedFileRestore puts a tracked file back after a test whose code under test
// rewrites it. An existing file restores to its bytes, an absent one is removed
// again, and an existing file it could not read whole is left alone: treating
// that case as "absent" would delete it.
// =============================================================================

#include "OloEnginePCH.h"
#include <gtest/gtest.h>
#include "ScopedFileRestore.h"
#include "TestTempDir.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace
{
    namespace fs = std::filesystem;
    using OloEngine::Tests::ScopedFileRestore;

    void WriteBytes(const fs::path& path, const std::string& bytes)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << bytes;
    }

    [[nodiscard]] std::string ReadBytes(const fs::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
    }
} // namespace

TEST(ScopedFileRestore, AnExistingFileGetsItsOriginalBytesBack)
{
    const fs::path file = OloEngine::Tests::TempFile("registry.oar");
    WriteBytes(file, std::string("original\0bytes", 14));
    {
        ScopedFileRestore restore(file);
        ASSERT_TRUE(restore.IsCaptured());
        WriteBytes(file, "rewritten by the code under test");
    }
    EXPECT_EQ(ReadBytes(file), std::string("original\0bytes", 14));
}

TEST(ScopedFileRestore, AFileThatDidNotExistIsRemovedAgain)
{
    const fs::path file = OloEngine::Tests::TempFile("absent.oar");
    ASSERT_FALSE(fs::exists(file));
    ScopedFileRestore restore(file);
    ASSERT_TRUE(restore.IsCaptured());
    WriteBytes(file, "created by the code under test");
    EXPECT_TRUE(restore.Restore());
    EXPECT_FALSE(fs::exists(file));
}

// A directory exists and cannot be opened as a file: the stand-in for a file
// that is there but unreadable. Before, the failed read left the snapshot
// empty, which Restore took for "there was no file" and removed the path.
TEST(ScopedFileRestore, AnExistingPathItCannotReadIsNeverRemoved)
{
    const fs::path dir = OloEngine::Tests::TempDir("unreadable");
    WriteBytes(dir / "inside.txt", "kept");
    ScopedFileRestore restore(dir);
    EXPECT_FALSE(restore.IsCaptured());
    EXPECT_FALSE(restore.Restore()) << "nothing was captured, so there is nothing to put back";
    EXPECT_TRUE(fs::is_directory(dir));
    EXPECT_EQ(ReadBytes(dir / "inside.txt"), "kept");
}
