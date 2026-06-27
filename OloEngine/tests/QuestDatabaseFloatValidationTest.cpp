// OLO_TEST_LAYER: unit
//
// Guards QuestDatabase::LoadFromDirectory against non-finite floats in hand-edited or
// corrupt .oloquest files. A NaN/inf TimeLimit or RepeatCooldownSeconds would silently
// corrupt quest timing (QuestJournal compares TimeLimit/cooldown against 0), so the
// loader must substitute the documented sentinels. See cpp-coding-quality.md §2b.
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Gameplay/Quest/QuestDatabase.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

using namespace OloEngine;

namespace
{
    class QuestDatabaseFloatValidationTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            QuestDatabase::Clear();
            // Per-test subdir so parallel ctest runs (each case is its own process)
            // don't fight over a shared path — see testing-architecture.md §6.1.
            const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
            const std::string testSuite = info ? info->test_suite_name() : "x";
            const std::string testName = info ? info->name() : "y";
            const std::filesystem::path baseDir = std::filesystem::temp_directory_path() / "olo_quest_floatval_test";
            m_Dir = baseDir / (testSuite + "_" + testName);
            std::error_code ec;
            std::filesystem::remove_all(m_Dir, ec);
            std::filesystem::create_directories(m_Dir, ec);
        }

        void TearDown() override
        {
            QuestDatabase::Clear();
            std::error_code ec;
            std::filesystem::remove_all(m_Dir, ec);
        }

        // Writes a single .oloquest file with the given body and loads the directory.
        void WriteQuest(const std::string& fileName, const std::string& body)
        {
            std::ofstream out(m_Dir / fileName);
            out << body;
            out.close();
            QuestDatabase::LoadFromDirectory(m_Dir.string());
        }

        std::filesystem::path m_Dir;
    };

    TEST_F(QuestDatabaseFloatValidationTest, NaNTimeLimitFallsBackToNoLimitSentinel)
    {
        WriteQuest("nan_timelimit.oloquest",
                   "QuestID: q_nan_time\n"
                   "TimeLimit: .nan\n");

        const auto* def = QuestDatabase::Get("q_nan_time");
        ASSERT_NE(def, nullptr);
        EXPECT_TRUE(std::isfinite(def->TimeLimit));
        EXPECT_FLOAT_EQ(def->TimeLimit, -1.0f); // -1 == "no time limit"
    }

    TEST_F(QuestDatabaseFloatValidationTest, InfCooldownFallsBackToZero)
    {
        WriteQuest("inf_cooldown.oloquest",
                   "QuestID: q_inf_cd\n"
                   "IsRepeatable: true\n"
                   "RepeatCooldownSeconds: .inf\n");

        const auto* def = QuestDatabase::Get("q_inf_cd");
        ASSERT_NE(def, nullptr);
        EXPECT_TRUE(std::isfinite(def->RepeatCooldownSeconds));
        EXPECT_FLOAT_EQ(def->RepeatCooldownSeconds, 0.0f);
    }

    TEST_F(QuestDatabaseFloatValidationTest, NegativeCooldownIsClampedToZero)
    {
        WriteQuest("neg_cooldown.oloquest",
                   "QuestID: q_neg_cd\n"
                   "IsRepeatable: true\n"
                   "RepeatCooldownSeconds: -5.0\n");

        const auto* def = QuestDatabase::Get("q_neg_cd");
        ASSERT_NE(def, nullptr);
        EXPECT_FLOAT_EQ(def->RepeatCooldownSeconds, 0.0f);
    }

    TEST_F(QuestDatabaseFloatValidationTest, ValidFloatsAreLeftUntouched)
    {
        WriteQuest("valid.oloquest",
                   "QuestID: q_valid\n"
                   "TimeLimit: 120.0\n"
                   "IsRepeatable: true\n"
                   "RepeatCooldownSeconds: 30.0\n");

        const auto* def = QuestDatabase::Get("q_valid");
        ASSERT_NE(def, nullptr);
        EXPECT_FLOAT_EQ(def->TimeLimit, 120.0f);
        EXPECT_FLOAT_EQ(def->RepeatCooldownSeconds, 30.0f);
    }
} // namespace
