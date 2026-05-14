#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Core/FileSystem.h"

#include <chrono>
#include <filesystem>
#include <fstream>

// =============================================================================
// AutoSaveTest -- contract: FileSystem::IsNewer compares mtimes correctly
// across missing-file edge cases plus a real "B is newer than A" case.
//
// Prior version slept 50ms between writes to force a filesystem-clock
// difference. That's filesystem-resolution dependent (FAT32 has 2-second
// mtime granularity; some networked filesystems coalesce timestamps) and
// adds 50ms to every CI run. Per docs/testing.md section 3
// ("Don't busy-wait or sleep") we inject mtimes via
// std::filesystem::last_write_time instead.
//
// The "ProjectConfig defaults" / "fields are writable" runtime tests
// were retired as design-choice pinning per docs/testing.md
// section 4.1.
// =============================================================================

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file

namespace
{
    class FileSystemIsNewerTest : public ::testing::Test
    {
      protected:
        std::filesystem::path m_TempDir;

        void SetUp() override
        {
            const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
            // Per-test subdir so parallel ctest runs don't fight.
            m_TempDir = std::filesystem::temp_directory_path()
                        / "olo_autosave_test"
                        / (std::string(info ? info->test_suite_name() : "x")
                            + "_" + std::string(info ? info->name() : "y"));
            std::error_code ec;
            std::filesystem::remove_all(m_TempDir, ec);
            std::filesystem::create_directories(m_TempDir, ec);
        }

        void TearDown() override
        {
            std::error_code ec;
            std::filesystem::remove_all(m_TempDir, ec);
        }

        void WriteFile(const std::filesystem::path& path, const std::string& content) const
        {
            std::ofstream ofs(path);
            ofs << content;
        }

        // Set the file's mtime to `now + offsetSeconds`. Lets us force a
        // deterministic ordering without sleeping. If the platform refuses
        // the operation (some networked filesystems do), the test errors
        // out clearly rather than going flaky.
        void SetMTime(const std::filesystem::path& path, int offsetSeconds) const
        {
            std::error_code ec;
            const auto t = std::filesystem::file_time_type::clock::now()
                         + std::chrono::seconds(offsetSeconds);
            std::filesystem::last_write_time(path, t, ec);
            ASSERT_FALSE(ec)
                << "last_write_time injection failed for " << path
                << ": " << ec.message()
                << " (the test cannot proceed deterministically on this filesystem).";
        }
    };
}

TEST_F(FileSystemIsNewerTest, ReturnsFalseWhenFileADoesNotExist)
{
    auto fileB = m_TempDir / "b.txt";
    WriteFile(fileB, "B");
    EXPECT_FALSE(FileSystem::IsNewer(m_TempDir / "nonexistent.txt", fileB));
}

TEST_F(FileSystemIsNewerTest, ReturnsFalseWhenFileBDoesNotExist)
{
    auto fileA = m_TempDir / "a.txt";
    WriteFile(fileA, "A");
    EXPECT_FALSE(FileSystem::IsNewer(fileA, m_TempDir / "nonexistent.txt"));
}

TEST_F(FileSystemIsNewerTest, ReturnsFalseWhenBothDoNotExist)
{
    EXPECT_FALSE(FileSystem::IsNewer(m_TempDir / "no1.txt", m_TempDir / "no2.txt"));
}

TEST_F(FileSystemIsNewerTest, DetectsNewerFileByInjectedMTime)
{
    auto fileOld = m_TempDir / "old.txt";
    auto fileNew = m_TempDir / "new.txt";

    WriteFile(fileOld, "old");
    WriteFile(fileNew, "new");

    // Inject mtimes 10 seconds apart -- coarse enough that any reasonable
    // filesystem resolution sees a difference, deterministic regardless
    // of clock granularity.
    SetMTime(fileOld, -10);
    SetMTime(fileNew,  10);

    EXPECT_TRUE(FileSystem::IsNewer(fileNew, fileOld));
    EXPECT_FALSE(FileSystem::IsNewer(fileOld, fileNew));
}
