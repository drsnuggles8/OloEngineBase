// OLO_TEST_LAYER: unit
// =============================================================================
// AnimationClipRequestSaveLoadTest.cpp
//
// The save-game cell for issue #1533's clip playback state in
// AnimationStateComponent: the loop flags, the pending clip request and the
// playback speed. They must round-trip, a hostile speed must load as the
// authored rate, and the fields must be consumed exactly, or every component
// after this one reads garbage. There is no older layout to read: a save of any
// other format version is rejected at the header (binary-format-versioning.md).
// =============================================================================

#include "OloEnginePCH.h"

#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/SaveGame/SaveGameComponentSerializer.h"
#include "OloEngine/SaveGame/SaveGameTypes.h"
#include "OloEngine/Serialization/Archive.h"
#include "OloEngine/Serialization/ArchiveExtensions.h"

#include <gtest/gtest.h>

#include <limits>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        [[nodiscard]] AnimationStateComponent RoundTrip(const AnimationStateComponent& seed)
        {
            std::vector<u8> buffer;
            {
                FMemoryWriter writer(buffer);
                writer.ArIsSaveGame = true;
                writer.SetArchiveVersion(kSaveGameFormatVersion);
                AnimationStateComponent copy = seed;
                SaveGameComponentSerializer::Serialize(writer, copy);
            }

            AnimationStateComponent loaded{};
            FMemoryReader reader(buffer);
            reader.ArIsSaveGame = true;
            reader.SetArchiveVersion(kSaveGameFormatVersion);
            SaveGameComponentSerializer::Serialize(reader, loaded);
            EXPECT_FALSE(reader.IsError());
            EXPECT_TRUE(reader.AtEnd()) << "the reader did not consume exactly the payload — a field-order desync";
            return loaded;
        }
    } // namespace

    TEST(AnimationClipRequestSaveLoad, TheLoopFlagsRequestAndSpeedSurviveARoundTrip)
    {
        AnimationStateComponent seed;
        seed.m_Loop = false;
        seed.m_NextLoop = false;
        seed.m_RequestedClip = "Sit";
        seed.m_RequestedLoop = false;
        seed.m_PlaybackSpeed = 0.625f;
        seed.m_CurrentTime = 1.25f;

        const AnimationStateComponent loaded = RoundTrip(seed);
        EXPECT_FALSE(loaded.m_Loop);
        EXPECT_FALSE(loaded.m_NextLoop);
        EXPECT_EQ(loaded.m_RequestedClip, "Sit") << "a request filed just before the save must not be lost";
        EXPECT_FALSE(loaded.m_RequestedLoop);
        EXPECT_FLOAT_EQ(loaded.m_PlaybackSpeed, 0.625f);
        EXPECT_FLOAT_EQ(loaded.m_CurrentTime, 1.25f);
    }

    TEST(AnimationClipRequestSaveLoad, AHostilePlaybackSpeedLoadsAsTheAuthoredRate)
    {
        AnimationStateComponent seed;
        seed.m_PlaybackSpeed = std::numeric_limits<f32>::quiet_NaN();
        EXPECT_FLOAT_EQ(RoundTrip(seed).m_PlaybackSpeed, 1.0f);

        seed.m_PlaybackSpeed = 1.0e6f; // would lap a clip thousands of times per tick
        EXPECT_FLOAT_EQ(RoundTrip(seed).m_PlaybackSpeed, 1.0f);
    }
} // namespace OloEngine::Tests
