// OLO_TEST_LAYER: unit
// =============================================================================
// FoliageLeafMaterialSaveLoadTest.cpp
//
// The BINARY save-game half of issue #1234's persistence criterion. Its sibling
// on the scene-YAML side is FoliageLeafMaterialTest.cpp; both exist because the
// two paths are written independently and inherit nothing from each other —
// SceneSerializer.cpp reads YAML keys, SaveGameComponentSerializer.cpp reads a
// FIXED-ORDER byte stream — so they drift silently, and a save file that loads
// values the scene loader would have rejected feeds them straight into a pow()
// exponent in the shared shader evaluation.
//
// Two claims:
//
//   THE FIELDS SURVIVE A ROUND TRIP at the current format version. A field
//   added to FoliageLayer and to the scene serializer but forgotten here
//   round-trips fine through a .olo and silently vanishes through a save —
//   the exact bug class SaveGameComponentSerializerCoverageTest exists for at
//   the component level, asserted here at the field level.
//
//   HOSTILE VALUES ARE SANITIZED ON LOAD. A save file is no more trusted than
//   a .olo, and the bounds are declared in both places by hand.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Math/Math.h"
#include "OloEngine/SaveGame/SaveGameComponentSerializer.h"
#include "OloEngine/SaveGame/SaveGameTypes.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Serialization/Archive.h"
#include "OloEngine/Serialization/ArchiveExtensions.h"
#include "OloEngine/Terrain/Foliage/FoliageLayer.h"

#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        FoliageLayer MakeAuthoredLeafLayer()
        {
            FoliageLayer layer;
            layer.Name = "BacklitClump";
            layer.AlbedoPath = "assets/textures/grass.png";
            layer.NormalMapPath = "assets/textures/leaf_normal.png";
            layer.RoughnessMapPath = "assets/textures/leaf_roughness.png";
            layer.ThicknessMapPath = "assets/textures/leaf_thickness.png";
            layer.NormalStrength = 0.6875f;
            layer.TransmissionStrength = 1.3125f;
            layer.TransmissionColor = glm::vec3(0.3125f, 0.5625f, 0.1875f);
            layer.Thickness = 0.8125f;
            layer.TransmissionDistortion = 0.4375f;
            layer.TransmissionPower = 6.5f;
            layer.TransmissionWrap = 0.28125f;
            layer.TransmissionAmbient = 0.90625f;
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
                << "the reader did not consume exactly the payload — a field-order desync, which in a fixed-order "
                   "archive corrupts every component after this one, not just this one";
            return loaded;
        }
    } // namespace

    TEST(FoliageLeafMaterialSaveLoad, EveryLeafFieldSurvivesTheCurrentFormat)
    {
        FoliageComponent seed;
        seed.m_Enabled = true;
        seed.m_Layers.Add(MakeAuthoredLeafLayer());

        const FoliageComponent loaded = RoundTrip(seed);
        ASSERT_EQ(loaded.m_Layers.Num(), 1u);
        const FoliageLayer& got = loaded.m_Layers[0];
        const FoliageLayer& want = seed.m_Layers[0];

        EXPECT_EQ(got.NormalMapPath, want.NormalMapPath);
        EXPECT_EQ(got.RoughnessMapPath, want.RoughnessMapPath);
        EXPECT_EQ(got.ThicknessMapPath, want.ThicknessMapPath);
        EXPECT_TRUE(Math::BitwiseEqual(got.NormalStrength, want.NormalStrength));
        EXPECT_TRUE(Math::BitwiseEqual(got.TransmissionStrength, want.TransmissionStrength));
        EXPECT_TRUE(Math::BitwiseEqual(got.TransmissionColor, want.TransmissionColor));
        EXPECT_TRUE(Math::BitwiseEqual(got.Thickness, want.Thickness));
        EXPECT_TRUE(Math::BitwiseEqual(got.TransmissionDistortion, want.TransmissionDistortion));
        EXPECT_TRUE(Math::BitwiseEqual(got.TransmissionPower, want.TransmissionPower));
        EXPECT_TRUE(Math::BitwiseEqual(got.TransmissionWrap, want.TransmissionWrap));
        EXPECT_TRUE(Math::BitwiseEqual(got.TransmissionAmbient, want.TransmissionAmbient));
    }

    TEST(FoliageLeafMaterialSaveLoad, HostileValuesAreSanitizedOnLoad)
    {
        FoliageComponent seed;
        seed.m_Enabled = true;
        FoliageLayer hostile;
        hostile.Name = "Hostile";
        hostile.TransmissionStrength = std::numeric_limits<f32>::quiet_NaN();
        hostile.TransmissionColor = glm::vec3(std::numeric_limits<f32>::infinity(), -5.0f, 0.5f);
        hostile.Thickness = 42.0f;
        hostile.TransmissionDistortion = -3.0f;
        // Finite and still wrong: below 1 the pow() lobe turns inside out and is
        // brightest AWAY from the light.
        hostile.TransmissionPower = 0.0f;
        hostile.TransmissionWrap = std::numeric_limits<f32>::quiet_NaN();
        hostile.TransmissionAmbient = std::numeric_limits<f32>::infinity();
        hostile.NormalStrength = -std::numeric_limits<f32>::infinity();
        seed.m_Layers.Add(hostile);

        // A SECOND LAYER, FINITE BUT OUT OF RANGE. The layer above proves the
        // REJECT path: a non-finite component sends the whole vector back to
        // its default before any clamping, so it can never show that the
        // components clamp independently. This one has every component finite,
        // so the clamp actually runs.
        FoliageLayer outOfRange;
        outOfRange.Name = "FiniteOutOfRange";
        outOfRange.TransmissionStrength = 2.0f;
        outOfRange.TransmissionColor = glm::vec3(-0.5f, 4.0f, 0.25f);
        outOfRange.Thickness = 0.5f;
        seed.m_Layers.Add(outOfRange);

        const FoliageComponent loaded = RoundTrip(seed);
        ASSERT_EQ(loaded.m_Layers.Num(), 2u);
        const FoliageLayer& l = loaded.m_Layers[0];

        EXPECT_TRUE(std::isfinite(l.TransmissionStrength));
        EXPECT_TRUE(std::isfinite(l.NormalStrength));
        EXPECT_TRUE(std::isfinite(l.Thickness));
        EXPECT_TRUE(std::isfinite(l.TransmissionDistortion));
        EXPECT_TRUE(std::isfinite(l.TransmissionPower));
        EXPECT_TRUE(std::isfinite(l.TransmissionWrap));
        EXPECT_TRUE(std::isfinite(l.TransmissionAmbient));
        EXPECT_TRUE(std::isfinite(l.TransmissionColor.x) && std::isfinite(l.TransmissionColor.y) &&
                    std::isfinite(l.TransmissionColor.z));

        EXPECT_GE(l.TransmissionPower, 1.0f);
        EXPECT_LE(l.TransmissionPower, 64.0f);
        EXPECT_GE(l.Thickness, 0.0f);
        EXPECT_LE(l.Thickness, 1.0f);
        EXPECT_GE(l.TransmissionDistortion, 0.0f);
        EXPECT_GE(l.NormalStrength, 0.0f);
        EXPECT_TRUE(Math::BitwiseEqual(l.TransmissionColor, glm::vec3(0.42f, 0.62f, 0.18f)))
            << "a colour with a non-finite component must keep the default outright, not a "
               "partially-clamped mixture of authored and default components";

        const FoliageLayer& oor = loaded.m_Layers[1];
        EXPECT_FLOAT_EQ(oor.TransmissionColor.x, 0.0f) << "x below 0 must clamp UP to 0";
        EXPECT_FLOAT_EQ(oor.TransmissionColor.y, 1.0f) << "y above 1 must clamp DOWN to 1";
        EXPECT_FLOAT_EQ(oor.TransmissionColor.z, 0.25f) << "z is already in range and must be untouched";
        EXPECT_FLOAT_EQ(oor.TransmissionStrength, 2.0f) << "a strength inside [0, 8] is not clamped";
    }

} // namespace OloEngine::Tests
