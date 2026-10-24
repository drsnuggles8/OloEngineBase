// OLO_TEST_LAYER: unit
// =============================================================================
// PlayComponentsSaveLoadTest.cpp
//
// The save-game cell for issue #1559's components: GroomColliderComponent, the
// hand or toy that pushes a coat; PickVolumesComponent, the bone capsules a
// cursor hits; and NavAgentComponent's root-motion steering fields. Each field
// is set away from its default, so one that is not written cannot come back
// equal by being default-initialised, and hostile bytes load sanitized the way
// the scene YAML clamps them. The `AtEnd()` check turns a field-order desync
// into a failure here rather than into every component after this one.
//
// NO VERSION BAND IS TESTED for the two components: they are new, so a save
// written before them does not contain their keys. NavAgentComponent's fields
// grew, and the format version rose with them (an older save is refused,
// SaveGameHeaderTest.OlderFormatVersionIsInvalid).
// =============================================================================

#include "OloEnginePCH.h"

#include "OloEngine/SaveGame/SaveGameComponentSerializer.h"
#include "OloEngine/SaveGame/SaveGameTypes.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Serialization/Archive.h"
#include "OloEngine/Serialization/ArchiveExtensions.h"

#include <gtest/gtest.h>

#include <limits>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        template<typename T>
        [[nodiscard]] T RoundTrip(const T& seed)
        {
            std::vector<u8> buffer;
            {
                FMemoryWriter writer(buffer);
                writer.ArIsSaveGame = true;
                writer.SetArchiveVersion(kSaveGameFormatVersion);
                T copy = seed;
                SaveGameComponentSerializer::Serialize(writer, copy);
            }

            T loaded{};
            FMemoryReader reader(buffer);
            reader.ArIsSaveGame = true;
            reader.SetArchiveVersion(kSaveGameFormatVersion);
            SaveGameComponentSerializer::Serialize(reader, loaded);
            EXPECT_FALSE(reader.IsError());
            EXPECT_TRUE(reader.AtEnd()) << "the reader did not consume exactly the payload: a field-order desync";
            return loaded;
        }

        constexpr f32 kNaN = std::numeric_limits<f32>::quiet_NaN();
    } // namespace

    TEST(PlayComponentsSaveLoad, AGroomColliderSurvivesTheRoundTrip)
    {
        GroomColliderComponent seed;
        seed.m_Radius = 0.045f;
        seed.m_HalfHeight = 0.2f;
        seed.m_Enabled = false;

        const GroomColliderComponent loaded = RoundTrip(seed);
        EXPECT_FLOAT_EQ(loaded.m_Radius, 0.045f);
        EXPECT_FLOAT_EQ(loaded.m_HalfHeight, 0.2f);
        EXPECT_FALSE(loaded.m_Enabled);
    }

    TEST(PlayComponentsSaveLoad, AHostileGroomColliderLoadsSanitized)
    {
        GroomColliderComponent seed;
        seed.m_Radius = kNaN;
        seed.m_HalfHeight = 500.0f;

        const GroomColliderComponent loaded = RoundTrip(seed);
        EXPECT_FLOAT_EQ(loaded.m_Radius, 0.05f) << "a non-finite radius loads as the default";
        EXPECT_FLOAT_EQ(loaded.m_HalfHeight, 10.0f) << "the half height is clamped as the scene YAML clamps it";
    }

    TEST(PlayComponentsSaveLoad, PickVolumesSurviveTheRoundTrip)
    {
        PickVolumesComponent seed;
        seed.m_Regions = "Head: head, jaw; Ears: ear_L_01, ear_R_01";
        seed.m_Padding = 0.035f;
        seed.m_RadiusScale = 1.25f;
        seed.m_Enabled = false;

        const PickVolumesComponent loaded = RoundTrip(seed);
        EXPECT_EQ(loaded.m_Regions, seed.m_Regions);
        EXPECT_FLOAT_EQ(loaded.m_Padding, 0.035f);
        EXPECT_FLOAT_EQ(loaded.m_RadiusScale, 1.25f);
        EXPECT_FALSE(loaded.m_Enabled);
    }

    TEST(PlayComponentsSaveLoad, HostilePickVolumesLoadSanitized)
    {
        PickVolumesComponent seed;
        seed.m_Padding = kNaN;
        seed.m_RadiusScale = 1000.0f;

        const PickVolumesComponent loaded = RoundTrip(seed);
        EXPECT_FLOAT_EQ(loaded.m_Padding, 0.0f);
        EXPECT_FLOAT_EQ(loaded.m_RadiusScale, 100.0f);
    }

    TEST(PlayComponentsSaveLoad, ANavAgentKeepsItsRootMotionSteering)
    {
        NavAgentComponent seed;
        seed.m_UseRootMotion = true;
        seed.m_TurnSpeed = 2.5f;
        seed.m_MaxSpeed = 4.0f; // a field from before, after which the new ones are written

        const NavAgentComponent loaded = RoundTrip(seed);
        EXPECT_TRUE(loaded.m_UseRootMotion);
        EXPECT_FLOAT_EQ(loaded.m_TurnSpeed, 2.5f);
        EXPECT_FLOAT_EQ(loaded.m_MaxSpeed, 4.0f);

        NavAgentComponent hostile;
        hostile.m_TurnSpeed = kNaN;
        EXPECT_FLOAT_EQ(RoundTrip(hostile).m_TurnSpeed, 6.0f) << "a non-finite turn speed loads as the default";
    }
} // namespace OloEngine::Tests
