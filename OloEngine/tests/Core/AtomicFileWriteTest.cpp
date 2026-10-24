#include "OloEnginePCH.h"
#include <gtest/gtest.h>
#include "TestTempDir.h"

// OLO_TEST_LAYER: unit

// =============================================================================
// FileSystem::WriteFileAtomically (issue #1635) — the one temp-and-rename write
// every serializer now goes through. Pins the contract the callers rely on:
//   * a success writes exactly the bytes, creating or replacing the file;
//   * every failure (OS refusal, failed stream, writer abandon, caller veto)
//     returns false and leaves the previous file byte-for-byte intact;
//   * no "<name>.<id>.tmp" is left beside the target either way.
// =============================================================================

#include "OloEngine/Core/FileSystem.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace OloEngine; // NOLINT(google-build-using-namespace)

namespace
{
    namespace fs = std::filesystem;

    [[nodiscard]] std::string ReadBytes(const fs::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
    }

    // Every entry in `dir` other than `keep`, so a leaked temporary shows by name.
    [[nodiscard]] std::string StrayEntries(const fs::path& dir, const fs::path& keep)
    {
        std::string stray;
        for (const auto& entry : fs::directory_iterator(dir))
            if (entry.path().filename() != keep.filename())
                stray += entry.path().filename().string() + " ";
        return stray;
    }
} // namespace

TEST(AtomicFileWriteTest, CreatesAndReplacesWithExactBytes)
{
    const fs::path dir = Tests::TempDir();
    const fs::path target = dir / "file.bin";

    // Binary by default: the '\n' must not become "\r\n" on Windows.
    const std::string first = std::string("first\n\0embedded", 15);
    ASSERT_TRUE(FileSystem::WriteFileAtomically(target, first));
    EXPECT_EQ(ReadBytes(target), first);

    const std::string second = "second, longer than the first file was";
    ASSERT_TRUE(FileSystem::WriteFileAtomically(target, second));
    EXPECT_EQ(ReadBytes(target), second);
    EXPECT_EQ(StrayEntries(dir, target), "");
}

TEST(AtomicFileWriteTest, CreatesMissingParentsOnlyWhenAsked)
{
    const fs::path target = Tests::TempDir() / "a" / "b" / "file.txt";

    std::string error;
    EXPECT_FALSE(FileSystem::WriteFileAtomically(target, "x", {}, &error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(fs::exists(target.parent_path()));

    ASSERT_TRUE(FileSystem::WriteFileAtomically(target, "x", { .CreateParentDirectories = true }));
    EXPECT_EQ(ReadBytes(target), "x");
}

// A parent path that is a regular file fails on every platform and for every
// user, root included, unlike a read-only bit.
TEST(AtomicFileWriteTest, UnwritableLocationReportsTheReason)
{
    const fs::path dir = Tests::TempDir();
    {
        std::ofstream blocker(dir / "blocker");
    }
    const fs::path target = dir / "blocker" / "file.txt";

    std::string error;
    EXPECT_FALSE(FileSystem::WriteFileAtomically(target, "x", { .CreateParentDirectories = true }, &error));
    EXPECT_NE(error.find("blocker"), std::string::npos) << error;
    EXPECT_EQ(StrayEntries(dir, dir / "blocker"), "");
}

TEST(AtomicFileWriteTest, FailedStreamLeavesTheOriginalIntact)
{
    const fs::path dir = Tests::TempDir();
    const fs::path target = dir / "file.txt";
    ASSERT_TRUE(FileSystem::WriteFileAtomically(target, "original"));

    // A full disk, as the stream sees it: the bytes go nowhere and badbit is set.
    std::string error;
    const bool written = FileSystem::WriteFileAtomically(
        target,
        [](std::ostream& out)
        {
            out << "partial";
            out.setstate(std::ios::badbit);
            return true;
        },
        {}, &error);
    EXPECT_FALSE(written);
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(ReadBytes(target), "original");
    EXPECT_EQ(StrayEntries(dir, target), "");
}

TEST(AtomicFileWriteTest, WriterAbandonLeavesTheOriginalIntact)
{
    const fs::path dir = Tests::TempDir();
    const fs::path target = dir / "file.txt";
    ASSERT_TRUE(FileSystem::WriteFileAtomically(target, "original"));

    EXPECT_FALSE(FileSystem::WriteFileAtomically(target,
                                                 [](std::ostream& out)
                                                 {
                                                     out << "half a record";
                                                     return false;
                                                 }));
    EXPECT_EQ(ReadBytes(target), "original");
    EXPECT_EQ(StrayEntries(dir, target), "");
}

TEST(AtomicFileWriteTest, VetoSeesTheFinishedTemporaryAndKeepsTheOriginal)
{
    const fs::path dir = Tests::TempDir();
    const fs::path target = dir / "file.txt";
    ASSERT_TRUE(FileSystem::WriteFileAtomically(target, "original"));

    std::string seen;
    const auto veto = [&seen](const fs::path& temporary)
    {
        seen = ReadBytes(temporary);
        return false;
    };
    EXPECT_FALSE(FileSystem::WriteFileAtomically(target, "replacement", { .BeforeReplace = veto }));
    EXPECT_EQ(seen, "replacement") << "the veto must see the complete, closed temporary";
    EXPECT_EQ(ReadBytes(target), "original");
    EXPECT_EQ(StrayEntries(dir, target), "");
}

TEST(AtomicFileWriteTest, DurableWriteLandsTheSameBytes)
{
    const fs::path target = Tests::TempDir() / "file.bin";
    ASSERT_TRUE(FileSystem::WriteFileAtomically(target, "old", { .Durable = true }));
    ASSERT_TRUE(FileSystem::WriteFileAtomically(target, "durable", { .Durable = true }));
    EXPECT_EQ(ReadBytes(target), "durable");
}
