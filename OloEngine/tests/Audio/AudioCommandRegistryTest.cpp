#include <gtest/gtest.h>
#include "OloEngine/Audio/AudioEvents/AudioCommandRegistry.h"

#include <filesystem>
#include <fstream>

using namespace OloEngine::Audio;

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

    auto tempPath = std::filesystem::temp_directory_path() / "test_audio_events.yaml";
    m_Registry.Serialize(tempPath);

    AudioCommandRegistry loaded;
    ASSERT_TRUE(loaded.Deserialize(tempPath));

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

    std::filesystem::remove(tempPath);
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

    auto tempPath = std::filesystem::temp_directory_path() / "test_multi_triggers.yaml";
    m_Registry.Serialize(tempPath);

    AudioCommandRegistry loaded;
    ASSERT_TRUE(loaded.Deserialize(tempPath));
    EXPECT_EQ(loaded.GetTriggerCount(), 3u);

    std::filesystem::remove(tempPath);
}
