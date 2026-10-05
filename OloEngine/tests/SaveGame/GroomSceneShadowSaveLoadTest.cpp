// OLO_TEST_LAYER: unit
// =============================================================================
// GroomSceneShadowSaveLoadTest.cpp
//
// The save-game cell for the groom scene-shadow component (#1323, re-landed by
// #1523). #1380 shipped the Serialize overload without a round-trip test; this
// is that test.
//
// Two things can go wrong here and both are silent.
//
//   * A FIELD IN THE WRONG ORDER desynchronises the fixed-order archive, which
//     corrupts every component written after this one rather than only this
//     one. The `AtEnd()` check is what turns that into a failure.
//
//   * A CORRUPT WIDTH FLOOR THAT IS NOT SANITISED reaches a DIVISOR in the
//     light-space widening (Groom/GroomShadowWidening.h), makes every strand's
//     half width a NaN and removes the whole coat from every shadow map with
//     nothing in any log. The OLO_SERIALIZE annotation guards scene YAML and the
//     live-write registries; it does NOT reach this archive.
//
// NO VERSION BAND IS TESTED, and there is none to test: saves are keyed by an
// FNV hash of the type name, so a save written before the component existed does
// not contain its key and the load never reaches this function.
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
        [[nodiscard]] GroomSceneShadowComponent RoundTrip(const GroomSceneShadowComponent& seed)
        {
            std::vector<u8> buffer;
            {
                FMemoryWriter writer(buffer);
                writer.ArIsSaveGame = true;
                writer.SetArchiveVersion(kSaveGameFormatVersion);
                GroomSceneShadowComponent copy = seed;
                SaveGameComponentSerializer::Serialize(writer, copy);
            }

            GroomSceneShadowComponent loaded{};
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

    TEST(GroomSceneShadowSaveLoad, EveryAuthoredFieldSurvivesTheRoundTrip)
    {
        // Every field away from its default, and the two directions opposite to
        // each other, so a swapped pair of bools cannot pass.
        GroomSceneShadowComponent seed;
        seed.m_ShadowWidthTexels = 2.5f;
        seed.m_CastShadows = false;
        seed.m_ReceiveShadows = true;

        const GroomSceneShadowComponent loaded = RoundTrip(seed);
        EXPECT_FLOAT_EQ(loaded.m_ShadowWidthTexels, seed.m_ShadowWidthTexels);
        EXPECT_EQ(loaded.m_CastShadows, seed.m_CastShadows);
        EXPECT_EQ(loaded.m_ReceiveShadows, seed.m_ReceiveShadows);
        EXPECT_TRUE(loaded == seed);

        seed.m_CastShadows = true;
        seed.m_ReceiveShadows = false;
        const GroomSceneShadowComponent swapped = RoundTrip(seed);
        EXPECT_TRUE(swapped.m_CastShadows);
        EXPECT_FALSE(swapped.m_ReceiveShadows);
    }

    TEST(GroomSceneShadowSaveLoad, ACorruptWidthFloorIsSanitisedRatherThanPassedToTheShadowPass)
    {
        const f32 defaultFloor = GroomSceneShadowComponent{}.m_ShadowWidthTexels;

        GroomSceneShadowComponent corrupt;
        corrupt.m_ShadowWidthTexels = std::numeric_limits<f32>::quiet_NaN();
        EXPECT_FLOAT_EQ(RoundTrip(corrupt).m_ShadowWidthTexels, defaultFloor) << "a NaN floor must be repaired";

        corrupt.m_ShadowWidthTexels = -3.0f;
        EXPECT_FLOAT_EQ(RoundTrip(corrupt).m_ShadowWidthTexels, defaultFloor) << "a negative floor must be repaired";

        corrupt.m_ShadowWidthTexels = std::numeric_limits<f32>::infinity();
        EXPECT_FLOAT_EQ(RoundTrip(corrupt).m_ShadowWidthTexels, defaultFloor) << "an infinite floor must be repaired";

        // Past the authoring bound is clamped, not repaired to the default: the
        // author asked for a thick shadow, and the bound is where that stops.
        corrupt.m_ShadowWidthTexels = 1000.0f;
        EXPECT_FLOAT_EQ(RoundTrip(corrupt).m_ShadowWidthTexels, 16.0f);

        // Zero is the legal A/B control (the floor off), and must survive.
        corrupt.m_ShadowWidthTexels = 0.0f;
        EXPECT_FLOAT_EQ(RoundTrip(corrupt).m_ShadowWidthTexels, 0.0f);
    }
} // namespace OloEngine::Tests
