// OLO_TEST_LAYER: unit
// =============================================================================
// BoneAttachmentSaveLoadTest.cpp
//
// The save-game cell for issue #1533's BoneAttachmentComponent.
//
// The failure this guards is silent: without a Serialize() overload and a
// RegisterAll() entry, the component round-trips through scene YAML and simply
// vanishes from every save-game, so a quick-load drops the eye off the head bone
// and it snaps to the parent's origin. The `AtEnd()` check turns a field-order
// desync — which in the fixed-order archive corrupts every component written
// after this one — into a failure here.
//
// NO VERSION BAND IS TESTED, and there is none: the component is new, so a save
// written before it existed does not contain its key and the load never reaches
// its Serialize().
// =============================================================================

#include "OloEnginePCH.h"

#include "OloEngine/SaveGame/SaveGameComponentSerializer.h"
#include "OloEngine/SaveGame/SaveGameTypes.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Serialization/Archive.h"
#include "OloEngine/Serialization/ArchiveExtensions.h"

#include <gtest/gtest.h>

#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        [[nodiscard]] BoneAttachmentComponent RoundTrip(const BoneAttachmentComponent& seed)
        {
            std::vector<u8> buffer;
            {
                FMemoryWriter writer(buffer);
                writer.ArIsSaveGame = true;
                writer.SetArchiveVersion(kSaveGameFormatVersion);
                BoneAttachmentComponent copy = seed;
                SaveGameComponentSerializer::Serialize(writer, copy);
            }

            BoneAttachmentComponent loaded{};
            FMemoryReader reader(buffer);
            reader.ArIsSaveGame = true;
            reader.SetArchiveVersion(kSaveGameFormatVersion);
            SaveGameComponentSerializer::Serialize(reader, loaded);
            EXPECT_FALSE(reader.IsError());
            EXPECT_TRUE(reader.AtEnd())
                << "the reader did not consume exactly the payload — a field-order desync, which in a "
                   "fixed-order archive corrupts every component after this one, not just this one";
            return loaded;
        }
    } // namespace

    TEST(BoneAttachmentSaveLoad, EveryAuthoredFieldSurvivesTheRoundTrip)
    {
        // Both fields away from their defaults, so a field that is not written
        // cannot come back equal by being default-initialised.
        BoneAttachmentComponent seed;
        seed.m_BoneName = "b_Eye_L_017";
        seed.m_Enabled = false;

        const BoneAttachmentComponent loaded = RoundTrip(seed);
        EXPECT_EQ(loaded.m_BoneName, seed.m_BoneName);
        EXPECT_EQ(loaded.m_Enabled, seed.m_Enabled);
        EXPECT_TRUE(loaded == seed);
    }

    TEST(BoneAttachmentSaveLoad, AnEmptyBoneNameRoundTripsAsEmpty)
    {
        // An empty name is a real authored state (it is reported as NoBoneName at
        // runtime), and the string encoding must not turn it into garbage.
        BoneAttachmentComponent seed;
        seed.m_BoneName.clear();
        seed.m_Enabled = true;

        const BoneAttachmentComponent loaded = RoundTrip(seed);
        EXPECT_TRUE(loaded.m_BoneName.empty());
        EXPECT_TRUE(loaded.m_Enabled);
    }
} // namespace OloEngine::Tests
