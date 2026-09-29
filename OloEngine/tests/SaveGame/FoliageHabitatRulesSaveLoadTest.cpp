// OLO_TEST_LAYER: unit
// =============================================================================
// FoliageHabitatRulesSaveLoadTest.cpp
//
// The save-game cell for issue #1254's species habitat rules, clumping and
// ground contact in SerializeFoliageLayer.
//
// A field written in the wrong ORDER desynchronises the fixed-order archive,
// corrupting every component after this one rather than just this one.
// =============================================================================

#include "OloEnginePCH.h"

#include "OloEngine/Math/Math.h"
#include "OloEngine/SaveGame/SaveGameComponentSerializer.h"
#include "OloEngine/SaveGame/SaveGameTypes.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Serialization/Archive.h"
#include "OloEngine/Serialization/ArchiveExtensions.h"
#include "OloEngine/Terrain/Foliage/FoliageLayer.h"

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        /// Every #1254 field set to a distinctive, exactly-representable value,
        /// so a field read back from the wrong offset cannot coincidentally
        /// match the one it was supposed to be.
        [[nodiscard]] FoliageLayer MakeHabitatLayer()
        {
            FoliageLayer layer;
            layer.Name = "WoodlandFringe";
            layer.AlbedoPath = "assets/textures/grass.png";
            layer.SlopeFeather = 7.5f;
            layer.UseAltitudeBand = true;
            layer.MinAltitude = 12.25f;
            layer.MaxAltitude = 48.75f;
            layer.AltitudeFeather = 3.125f;
            layer.UseMoisture = true;
            layer.MinMoisture = 0.15625f;
            layer.MaxMoisture = 0.78125f;
            layer.MoistureFeather = 0.09375f;
            layer.ExclusionSplatmapChannel = 2;
            layer.ExclusionThreshold = 0.65625f;
            layer.ClumpStrength = 0.71875f;
            layer.ClumpScale = 23.5f;
            layer.ClumpFalloff = 1.375f;
            layer.ClumpScaleInfluence = 0.40625f;
            layer.ClumpGroup = 3;
            layer.GroundOffset = -0.09375f;
            layer.SlopeSinkFactor = 0.84375f;
            layer.DecorrelatedVariation = true;
            return layer;
        }

        [[nodiscard]] FoliageComponent RoundTrip(const FoliageComponent& seed)
        {
            std::vector<u8> buffer;
            {
                FMemoryWriter writer(buffer);
                writer.ArIsSaveGame = true;
                writer.SetArchiveVersion(kSaveGameFormatVersion);
                FoliageComponent copy = seed;
                SaveGameComponentSerializer::Serialize(writer, copy);
            }

            FoliageComponent loaded{};
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

    TEST(FoliageHabitatRulesSaveLoad, EveryHabitatFieldSurvivesTheCurrentFormat)
    {
        FoliageComponent seed;
        seed.m_Enabled = true;
        seed.m_Layers.Add(MakeHabitatLayer());

        const FoliageComponent loaded = RoundTrip(seed);
        ASSERT_EQ(loaded.m_Layers.Num(), 1u);
        const FoliageLayer& got = loaded.m_Layers[0];
        const FoliageLayer& want = seed.m_Layers[0];

        EXPECT_TRUE(Math::BitwiseEqual(got.SlopeFeather, want.SlopeFeather));
        EXPECT_EQ(got.UseAltitudeBand, want.UseAltitudeBand);
        EXPECT_TRUE(Math::BitwiseEqual(got.MinAltitude, want.MinAltitude));
        EXPECT_TRUE(Math::BitwiseEqual(got.MaxAltitude, want.MaxAltitude));
        EXPECT_TRUE(Math::BitwiseEqual(got.AltitudeFeather, want.AltitudeFeather));
        EXPECT_EQ(got.UseMoisture, want.UseMoisture);
        EXPECT_TRUE(Math::BitwiseEqual(got.MinMoisture, want.MinMoisture));
        EXPECT_TRUE(Math::BitwiseEqual(got.MaxMoisture, want.MaxMoisture));
        EXPECT_TRUE(Math::BitwiseEqual(got.MoistureFeather, want.MoistureFeather));
        EXPECT_EQ(got.ExclusionSplatmapChannel, want.ExclusionSplatmapChannel);
        EXPECT_TRUE(Math::BitwiseEqual(got.ExclusionThreshold, want.ExclusionThreshold));
        EXPECT_TRUE(Math::BitwiseEqual(got.ClumpStrength, want.ClumpStrength));
        EXPECT_TRUE(Math::BitwiseEqual(got.ClumpScale, want.ClumpScale));
        EXPECT_TRUE(Math::BitwiseEqual(got.ClumpFalloff, want.ClumpFalloff));
        EXPECT_TRUE(Math::BitwiseEqual(got.ClumpScaleInfluence, want.ClumpScaleInfluence));
        EXPECT_EQ(got.ClumpGroup, want.ClumpGroup);
        EXPECT_TRUE(Math::BitwiseEqual(got.GroundOffset, want.GroundOffset));
        EXPECT_TRUE(Math::BitwiseEqual(got.SlopeSinkFactor, want.SlopeSinkFactor));
        EXPECT_EQ(got.DecorrelatedVariation, want.DecorrelatedVariation);

        // operator== is the editor-undo path, and a field missing from it means
        // undo silently skips that value. A full round-trip must compare equal.
        EXPECT_TRUE(got == want) << "a #1254 field round-tripped but is missing from FoliageLayer::operator==, "
                                    "so editor undo will not see it change";
    }

    TEST(FoliageHabitatRulesSaveLoad, ACorruptSaveIsClampedRatherThanTrusted)
    {
        // A save file is no more trusted than a .olo: every float here reaches
        // the placement generator, where a NaN band silently empties the layer
        // and a zero clump scale is a division by zero.
        FoliageComponent seed;
        seed.m_Enabled = true;
        FoliageLayer hostile = MakeHabitatLayer();
        const f32 nan = std::numeric_limits<f32>::quiet_NaN();
        hostile.SlopeFeather = nan;
        hostile.MinMoisture = nan;
        hostile.ClumpScale = 0.0f;
        hostile.ClumpFalloff = nan;
        hostile.SlopeSinkFactor = 1.0e9f;
        hostile.AltitudeFeather = -5.0f;
        seed.m_Layers.Add(hostile);

        const FoliageComponent loaded = RoundTrip(seed);
        ASSERT_EQ(loaded.m_Layers.Num(), 1u);
        const FoliageLayer& got = loaded.m_Layers[0];

        EXPECT_TRUE(std::isfinite(got.SlopeFeather));
        EXPECT_TRUE(std::isfinite(got.MinMoisture));
        EXPECT_TRUE(std::isfinite(got.ClumpFalloff));
        EXPECT_GE(got.ClumpScale, 0.01f) << "a zero clump scale is a division by zero in ClumpField";
        EXPECT_GE(got.AltitudeFeather, 0.0f);
        EXPECT_LE(got.SlopeSinkFactor, 4.0f);
    }
} // namespace OloEngine::Tests
