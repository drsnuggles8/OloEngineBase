#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Core/FileSystem.h"
#include "OloEngine/Project/Project.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file

// =============================================================================
// FileSystem::IsNewer
// =============================================================================

class FileSystemIsNewerTest : public ::testing::Test
{
  protected:
    std::filesystem::path m_TempDir;

    void SetUp() override
    {
        m_TempDir = std::filesystem::temp_directory_path() / "olo_autosave_test";
        std::filesystem::create_directories(m_TempDir);
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
};

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

TEST_F(FileSystemIsNewerTest, DetectsNewerFile)
{
    auto fileOld = m_TempDir / "old.txt";
    auto fileNew = m_TempDir / "new.txt";

    WriteFile(fileOld, "old");
    // Ensure filesystem timestamp difference
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    WriteFile(fileNew, "new");

    EXPECT_TRUE(FileSystem::IsNewer(fileNew, fileOld));
    EXPECT_FALSE(FileSystem::IsNewer(fileOld, fileNew));
}

// =============================================================================
// ProjectConfig auto-save defaults
// =============================================================================

TEST(ProjectConfigAutoSaveTest, DefaultsAreCorrect)
{
    ProjectConfig config;
    EXPECT_TRUE(config.EnableAutoSave);
    EXPECT_EQ(config.AutoSaveIntervalSeconds, 300);
}

TEST(ProjectConfigAutoSaveTest, FieldsAreWritable)
{
    ProjectConfig config;
    config.EnableAutoSave = false;
    config.AutoSaveIntervalSeconds = 60;
    EXPECT_FALSE(config.EnableAutoSave);
    EXPECT_EQ(config.AutoSaveIntervalSeconds, 60);
}
