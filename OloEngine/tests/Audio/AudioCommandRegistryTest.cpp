#include <gtest/gtest.h>
#include "OloEngine/Audio/AudioEvents/AudioCommandRegistry.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

using namespace OloEngine::Audio;

namespace
{
    /// RAII helper: creates a unique temp file path and removes it on destruction.
    struct ScopedTempFile
    {
        std::filesystem::path Path;

        explicit ScopedTempFile(std::string_view testName)
        {
            auto pid = static_cast<u32>(
#ifdef _WIN32
                _getpid()
#else
                getpid()
#endif
            );
            auto name = std::string("olo_test_") + std::string(testName) + "_" + std::to_string(pid) + ".yaml";
            Path = std::filesystem::temp_directory_path() / name;
        }

        ~ScopedTempFile()
        {
            std::filesystem::remove(Path);
        }

        ScopedTempFile(const ScopedTempFile&) = delete;
        auto operator=(const ScopedTempFile&) -> ScopedTempFile& = delete;
    };
} // namespace

class AudioCommandRegistryTest : public ::testing::Test
{
  protected:
    AudioCommandRegistry m_Registry;
};

TEST_F(AudioCommandRegistryTest, AddTrigger)
{
    auto id = m_Registry.AddTrigger("Play_Gunshot");
    EXPECT_TRUE(id.IsValid());
    EXPECT_TRUE(m_Registry.Contains(id));
    EXPECT_EQ(m_Registry.GetTriggerCount(), 1u);
}

TEST_F(AudioCommandRegistryTest, RemoveTrigger)
{
    auto id = m_Registry.AddTrigger("Play_Gunshot");
    EXPECT_TRUE(m_Registry.RemoveTrigger(id));
    EXPECT_FALSE(m_Registry.Contains(id));
    EXPECT_EQ(m_Registry.GetTriggerCount(), 0u);
}

TEST_F(AudioCommandRegistryTest, RemoveNonExistentTrigger)
{
    CommandID fake(999u);
    EXPECT_FALSE(m_Registry.RemoveTrigger(fake));
}

TEST_F(AudioCommandRegistryTest, DuplicateNameReturnsSameID)
{
    auto id1 = m_Registry.AddTrigger("Play_Gunshot");
    auto id2 = m_Registry.AddTrigger("Play_Gunshot");
    EXPECT_EQ(id1.ID, id2.ID);
    EXPECT_EQ(m_Registry.GetTriggerCount(), 1u);
}

TEST_F(AudioCommandRegistryTest, AddAction)
{
    auto id = m_Registry.AddTrigger("Play_Music");
    TriggerAction action;
    action.Type = ActionType::Play;
    action.AudioFilepath = "audio/music.wav";
    action.VolumeMultiplier = 0.8f;
    EXPECT_TRUE(m_Registry.AddAction(id, action));

    const auto* cmd = m_Registry.GetTrigger(id);
    ASSERT_NE(cmd, nullptr);
    ASSERT_EQ(cmd->Actions.size(), 1u);
    EXPECT_EQ(cmd->Actions[0].Type, ActionType::Play);
    EXPECT_EQ(cmd->Actions[0].AudioFilepath, "audio/music.wav");
    EXPECT_FLOAT_EQ(cmd->Actions[0].VolumeMultiplier, 0.8f);
}

TEST_F(AudioCommandRegistryTest, RemoveAction)
{
    auto id = m_Registry.AddTrigger("SFX");
    TriggerAction a1;
    a1.Type = ActionType::Play;
    TriggerAction a2;
    a2.Type = ActionType::Stop;
    m_Registry.AddAction(id, a1);
    m_Registry.AddAction(id, a2);

    EXPECT_TRUE(m_Registry.RemoveAction(id, 0));
    const auto* cmd = m_Registry.GetTrigger(id);
    ASSERT_NE(cmd, nullptr);
    ASSERT_EQ(cmd->Actions.size(), 1u);
    EXPECT_EQ(cmd->Actions[0].Type, ActionType::Stop);
}

TEST_F(AudioCommandRegistryTest, RemoveActionOutOfRange)
{
    auto id = m_Registry.AddTrigger("SFX");
    EXPECT_FALSE(m_Registry.RemoveAction(id, 0));
}

TEST_F(AudioCommandRegistryTest, Clear)
{
    m_Registry.AddTrigger("A");
    m_Registry.AddTrigger("B");
    m_Registry.Clear();
    EXPECT_EQ(m_Registry.GetTriggerCount(), 0u);
}

TEST_F(AudioCommandRegistryTest, SerializeDeserializeRoundTrip)
{
    auto id = m_Registry.AddTrigger("Play_Gunshot");
    TriggerAction action;
    action.Type = ActionType::Play;
    action.Context = ActionContext::GameObject;
    action.AudioFilepath = "audio/gunshot.wav";
    action.VolumeMultiplier = 0.9f;
    action.PitchMultiplier = 1.1f;
    action.Looping = true;
    m_Registry.AddAction(id, action);

    ScopedTempFile tmp("RoundTrip");
    ASSERT_TRUE(m_Registry.Serialize(tmp.Path));

    AudioCommandRegistry loaded;
    ASSERT_TRUE(loaded.Deserialize(tmp.Path));

    EXPECT_EQ(loaded.GetTriggerCount(), 1u);
    EXPECT_TRUE(loaded.Contains(id));

    const auto* cmd = loaded.GetTrigger(id);
    ASSERT_NE(cmd, nullptr);
    EXPECT_EQ(cmd->DebugName, "Play_Gunshot");
    ASSERT_EQ(cmd->Actions.size(), 1u);
    EXPECT_EQ(cmd->Actions[0].Type, ActionType::Play);
    EXPECT_EQ(cmd->Actions[0].Context, ActionContext::GameObject);
    EXPECT_EQ(cmd->Actions[0].AudioFilepath, "audio/gunshot.wav");
    EXPECT_FLOAT_EQ(cmd->Actions[0].VolumeMultiplier, 0.9f);
    EXPECT_FLOAT_EQ(cmd->Actions[0].PitchMultiplier, 1.1f);
    EXPECT_TRUE(cmd->Actions[0].Looping);
}

TEST_F(AudioCommandRegistryTest, DeserializeNonExistentFile)
{
    EXPECT_FALSE(m_Registry.Deserialize("nonexistent_file_12345.yaml"));
}

TEST_F(AudioCommandRegistryTest, MultipleTriggersRoundTrip)
{
    m_Registry.AddTrigger("Play_Gunshot");
    m_Registry.AddTrigger("Stop_Music");
    m_Registry.AddTrigger("Pause_Ambience");

    ScopedTempFile tmp("MultiTriggers");
    ASSERT_TRUE(m_Registry.Serialize(tmp.Path));

    AudioCommandRegistry loaded;
    ASSERT_TRUE(loaded.Deserialize(tmp.Path));
    EXPECT_EQ(loaded.GetTriggerCount(), 3u);
}

TEST_F(AudioCommandRegistryTest, DeserializeSanitizesInvalidMultipliers)
{
    // Write a YAML fixture with NaN/inf/extreme values for multipliers
    ScopedTempFile tmp("SanitizeMultipliers");
    {
        std::ofstream fout(tmp.Path);
        fout << "AudioEvents:\n"
             << "  Triggers:\n"
             << "    - Name: BadValues\n"
             << "      Actions:\n"
             << "        - Type: Play\n"
             << "          AudioFilepath: audio/test.wav\n"
             << "          Context: GameObject\n"
             << "          VolumeMultiplier: .nan\n"
             << "          PitchMultiplier: .inf\n"
             << "          Looping: false\n"
             << "        - Type: Play\n"
             << "          AudioFilepath: audio/test2.wav\n"
             << "          Context: GameObject\n"
             << "          VolumeMultiplier: 999.0\n"
             << "          PitchMultiplier: -5.0\n"
             << "          Looping: false\n";
    }

    AudioCommandRegistry loaded;
    ASSERT_TRUE(loaded.Deserialize(tmp.Path));
    ASSERT_EQ(loaded.GetTriggerCount(), 1u);

    auto id = CommandID::FromString("BadValues");
    const auto* cmd = loaded.GetTrigger(id);
    ASSERT_NE(cmd, nullptr);
    ASSERT_EQ(cmd->Actions.size(), 2u);

    // NaN/inf should be sanitized to defaults (1.0)
    EXPECT_TRUE(std::isfinite(cmd->Actions[0].VolumeMultiplier));
    EXPECT_FLOAT_EQ(cmd->Actions[0].VolumeMultiplier, 1.0f);
    EXPECT_TRUE(std::isfinite(cmd->Actions[0].PitchMultiplier));
    EXPECT_FLOAT_EQ(cmd->Actions[0].PitchMultiplier, 1.0f);

    // Out-of-range should also be sanitized to defaults (1.0)
    EXPECT_TRUE(std::isfinite(cmd->Actions[1].VolumeMultiplier));
    EXPECT_FLOAT_EQ(cmd->Actions[1].VolumeMultiplier, 1.0f);
    EXPECT_TRUE(std::isfinite(cmd->Actions[1].PitchMultiplier));
    EXPECT_FLOAT_EQ(cmd->Actions[1].PitchMultiplier, 1.0f);
}

TEST_F(AudioCommandRegistryTest, SerializeReturnsFalseOnBadPath)
{
    m_Registry.AddTrigger("Test");
    // Try to write to an invalid path
    EXPECT_FALSE(m_Registry.Serialize("Z:/nonexistent_drive_12345/impossible.yaml"));
}
