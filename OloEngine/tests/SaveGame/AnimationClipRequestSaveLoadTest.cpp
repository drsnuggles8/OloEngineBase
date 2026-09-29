// OLO_TEST_LAYER: unit
// =============================================================================
// AnimationClipRequestSaveLoadTest.cpp
//
// The save-game cell for issue #1533's clip playback state — the v40 band of
// AnimationStateComponent: the loop flags, the pending clip request and the
// playback speed.
//
// A v38 save must keep the defaults (every clip loops at the authored rate,
// which is what every clip did in the build that wrote it), and the band must
// be consumed exactly, or every component after this one reads garbage.
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

    TEST(AnimationClipRequestSaveLoad, TheV40BandSurvivesARoundTrip)
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

    TEST(AnimationClipRequestSaveLoad, AV38SaveKeepsEveryClipLoopingAtTheAuthoredRate)
    {
        // Hand-built in the exact pre-v40 layout: a v38 save can no longer be
        // produced, because HasFieldsSince always writes the current layout.
        // Mirror any change to the serializer's pre-v40 field ORDER here.
        AnimationStateComponent old;
        old.m_CurrentTime = 0.75f;
        old.m_IsPlaying = true;
        std::vector<u8> buffer;
        {
            FMemoryWriter ar(buffer);
            ar.ArIsSaveGame = true;
            ar.SetArchiveVersion(38);
            ar << old.m_State;
            ar << old.m_CurrentClipIndex;
            ar << old.m_CurrentTime << old.m_NextTime;
            ar << old.m_BlendFactor << old.m_Blending;
            ar << old.m_BlendDuration << old.m_BlendTime;
            ar << old.m_IsPlaying;
            ar << old.m_SourceFilePath;
            ar << old.m_BoneEntityIds;
            ar << old.m_RootBoneTransform;
        }

        // Restore default-constructs the component, so the v38 read, which
        // stops before the band, leaves the constructor defaults in place.
        AnimationStateComponent loaded;
        FMemoryReader reader(buffer);
        reader.ArIsSaveGame = true;
        reader.SetArchiveVersion(38);
        SaveGameComponentSerializer::Serialize(reader, loaded);
        EXPECT_FALSE(reader.IsError());
        EXPECT_TRUE(reader.AtEnd()) << "a v38 read must stop exactly where a v38 save ends";
        EXPECT_TRUE(loaded.m_Loop);
        EXPECT_FLOAT_EQ(loaded.m_PlaybackSpeed, 1.0f);
        EXPECT_TRUE(loaded.m_RequestedClip.empty());
        EXPECT_FLOAT_EQ(loaded.m_CurrentTime, 0.75f);
        EXPECT_TRUE(loaded.m_IsPlaying);
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
