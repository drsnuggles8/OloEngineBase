// OLO_TEST_LAYER: unit
// =============================================================================
// MaterialOverridesSaveLoadTest.cpp
//
// The save-game cell for issue #1533's MaterialOverridesComponent.
//
// Three silent failures are guarded:
//   * A FIELD IN THE WRONG ORDER desynchronises the fixed-order archive and
//     corrupts every component written after this one. The `AtEnd()` check
//     turns it into a failure here.
//   * A CORRUPT VALUE THAT IS NOT SANITISED reaches the material UBO: the
//     OLO_SERIALIZE bounds on MaterialOverride guard scene YAML and the binary
//     sidecar, not this archive, so the loader restates them.
//   * A CORRUPT COUNT sizes an allocation from whatever the bytes claim.
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

#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        // Exactly representable values throughout: a float that survives the
        // round trip did so because the bytes were right.
        [[nodiscard]] MaterialOverridesComponent MakeAuthoredOverrides()
        {
            MaterialOverridesComponent component;

            MaterialOverride nose;
            nose.MaterialName = "DogNose";
            nose.Kind = MaterialKind::Skin;
            nose.SkinProfile = 0xABCD'0001ull;
            nose.ThicknessFactor = 0.0078125f;
            nose.OverrideBaseColor = true;
            nose.BaseColor = glm::vec4(0.125f, 0.0625f, 0.03125f, 1.0f);
            nose.OverrideRoughness = true;
            nose.Roughness = 0.375f;
            component.m_Overrides.Add(nose);

            MaterialOverride gums;
            gums.MaterialName = "DogGums";
            gums.Kind = MaterialKind::Skin;
            gums.SkinProfile = 0xABCD'0002ull;
            gums.ThicknessFactor = 0.25f;
            gums.OverrideMetallic = true;
            gums.Metallic = 0.5f;
            component.m_Overrides.Add(gums);
            return component;
        }

        [[nodiscard]] std::vector<u8> Write(const MaterialOverridesComponent& seed)
        {
            std::vector<u8> buffer;
            FMemoryWriter writer(buffer);
            writer.ArIsSaveGame = true;
            writer.SetArchiveVersion(kSaveGameFormatVersion);
            MaterialOverridesComponent copy = seed;
            SaveGameComponentSerializer::Serialize(writer, copy);
            return buffer;
        }

        [[nodiscard]] MaterialOverridesComponent Read(const std::vector<u8>& buffer, bool expectError = false)
        {
            MaterialOverridesComponent loaded;
            FMemoryReader reader(buffer);
            reader.ArIsSaveGame = true;
            reader.SetArchiveVersion(kSaveGameFormatVersion);
            SaveGameComponentSerializer::Serialize(reader, loaded);
            EXPECT_EQ(reader.IsError(), expectError);
            if (!expectError)
            {
                EXPECT_TRUE(reader.AtEnd())
                    << "the reader did not consume exactly the payload — a field-order desync, which in a "
                       "fixed-order archive corrupts every component after this one, not just this one";
            }
            return loaded;
        }
    } // namespace

    TEST(MaterialOverridesSaveLoad, EveryAuthoredFieldOfEveryOverrideSurvivesTheRoundTrip)
    {
        const MaterialOverridesComponent seed = MakeAuthoredOverrides();
        const MaterialOverridesComponent loaded = Read(Write(seed));

        ASSERT_EQ(loaded.m_Overrides.Num(), 2);
        EXPECT_EQ(loaded.m_Overrides[0].MaterialName, "DogNose");
        EXPECT_EQ(loaded.m_Overrides[0].Kind, MaterialKind::Skin);
        EXPECT_EQ(static_cast<u64>(loaded.m_Overrides[0].SkinProfile), 0xABCD'0001ull);
        EXPECT_FLOAT_EQ(loaded.m_Overrides[0].ThicknessFactor, 0.0078125f);
        EXPECT_FLOAT_EQ(loaded.m_Overrides[0].BaseColor.g, 0.0625f);
        EXPECT_FLOAT_EQ(loaded.m_Overrides[0].Roughness, 0.375f);
        EXPECT_EQ(loaded.m_Overrides[1].MaterialName, "DogGums");
        EXPECT_TRUE(loaded.m_Overrides[1].OverrideMetallic);
        EXPECT_FLOAT_EQ(loaded.m_Overrides[1].Metallic, 0.5f);

        // The whole-object equality is the statement that nothing was missed.
        EXPECT_TRUE(loaded == seed);
    }

    TEST(MaterialOverridesSaveLoad, CorruptValuesAreSanitisedOnLoad)
    {
        MaterialOverridesComponent seed;
        MaterialOverride patch;
        patch.MaterialName = "DogTongue";
        patch.Kind = static_cast<MaterialKind>(7); // a kind from a future build
        patch.ThicknessFactor = -2.0f;
        patch.OverrideBaseColor = true;
        patch.BaseColor = glm::vec4(std::numeric_limits<f32>::quiet_NaN(), 0.5f, 0.5f, 1.0f);
        patch.OverrideRoughness = true;
        patch.Roughness = std::numeric_limits<f32>::infinity();
        patch.OverrideMetallic = true;
        patch.Metallic = 4.0f;
        seed.m_Overrides.Add(patch);

        const MaterialOverridesComponent loaded = Read(Write(seed));
        ASSERT_EQ(loaded.m_Overrides.Num(), 1);
        const MaterialOverride& clean = loaded.m_Overrides[0];
        EXPECT_EQ(clean.Kind, MaterialKind::Generic) << "a corrupt kind must reject to Generic, not saturate";
        EXPECT_FLOAT_EQ(clean.ThicknessFactor, 0.0f);
        EXPECT_TRUE(std::isfinite(clean.BaseColor.r) && std::isfinite(clean.BaseColor.g) &&
                    std::isfinite(clean.BaseColor.b) && std::isfinite(clean.BaseColor.a));
        EXPECT_TRUE(std::isfinite(clean.Roughness));
        EXPECT_GE(clean.Roughness, 0.0f);
        EXPECT_LE(clean.Roughness, 1.0f);
        EXPECT_FLOAT_EQ(clean.Metallic, 1.0f);
    }

    TEST(MaterialOverridesSaveLoad, AnImplausibleCountIsRefusedRatherThanAllocated)
    {
        // A count past any authored entity, and nothing after it.
        std::vector<u8> buffer;
        {
            FMemoryWriter writer(buffer);
            writer.ArIsSaveGame = true;
            writer.SetArchiveVersion(kSaveGameFormatVersion);
            u32 count = 50'000'000u;
            writer << count;
        }
        const MaterialOverridesComponent loaded = Read(buffer, /*expectError=*/true);
        EXPECT_TRUE(loaded.m_Overrides.IsEmpty());
    }
} // namespace OloEngine::Tests
