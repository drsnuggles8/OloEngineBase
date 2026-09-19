// OLO_TEST_LAYER: unit
// =============================================================================
// GroomFibreMaterialSaveLoadTest.cpp
//
// The save-game cell for issue #1247's fibre material.
//
// Three things can go wrong here and all three are silent.
//
//   * A FIELD IN THE WRONG ORDER desynchronises the fixed-order archive, which
//     corrupts every component written after this one rather than only this
//     one. The `AtEnd()` check below is what turns that into a failure.
//
//   * A CORRUPT VALUE THAT IS NOT SANITISED reaches a pow(), a log() and a
//     division inside a fragment shader. The OLO_SERIALIZE annotations on the
//     component's fields guard scene YAML and the live-write registries; they
//     do NOT reach this archive, so a save game is the one route into the
//     renderer that bypasses every one of them.
//
//   * A MODE INDEX SATURATED INSTEAD OF REJECTED turns a corrupt value into a
//     different VALID material — the exact failure ComponentReflection.h names
//     and GroomComponent::m_CompositionMode was fixed for.
//
// NO VERSION BAND IS TESTED, and there is none to test: the component is new,
// so a save written before it existed does not contain its key and the load
// never reaches this function. That is stated here rather than left as an
// absence, because "where is the old-version test" is the first question this
// file should answer.
// =============================================================================

#include "OloEnginePCH.h"

#include "OloEngine/Groom/GroomFibreScattering.h"
#include "OloEngine/SaveGame/SaveGameComponentSerializer.h"
#include "OloEngine/SaveGame/SaveGameTypes.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Serialization/Archive.h"
#include "OloEngine/Serialization/ArchiveExtensions.h"

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <cmath>
#include <limits>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        /// Every field set to a distinctive, exactly-representable value, so a
        /// field read back from the wrong offset cannot coincidentally match
        /// the one it was supposed to be. Powers of two and their sums
        /// throughout: a float that survives the round trip did so because the
        /// bytes were right, not because the comparison was loose.
        [[nodiscard]] GroomFibreComponent MakeAuthoredMaterial()
        {
            GroomFibreComponent component;
            component.m_BaseColor = glm::vec3(0.5f, 0.25f, 0.125f);
            component.m_Absorption = glm::vec3(1.5f, 2.25f, 3.125f);
            component.m_Eumelanin = 2.75f;
            component.m_Pheomelanin = 1.375f;
            component.m_LongitudinalRoughness = 0.40625f;
            component.m_AzimuthalRoughness = 0.65625f;
            component.m_TiltDegrees = 3.25f;
            component.m_IndexOfRefraction = 1.625f;
            component.m_Intensity = 6.5f;
            component.m_HSamples = 12u;
            component.m_PigmentMode = static_cast<u8>(GroomFibrePigmentMode::BaseColor);
            component.m_DebugMode = static_cast<u8>(GroomFibreDebugMode::LobeTT);
            component.m_Enabled = false;
            return component;
        }

        [[nodiscard]] GroomFibreComponent RoundTrip(const GroomFibreComponent& seed)
        {
            std::vector<u8> buffer;
            {
                FMemoryWriter writer(buffer);
                writer.ArIsSaveGame = true;
                writer.SetArchiveVersion(kSaveGameFormatVersion);
                GroomFibreComponent copy = seed;
                SaveGameComponentSerializer::Serialize(writer, copy);
            }

            GroomFibreComponent loaded{};
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

    TEST(GroomFibreMaterialSaveLoad, EveryAuthoredFieldSurvivesTheRoundTrip)
    {
        const GroomFibreComponent seed = MakeAuthoredMaterial();
        const GroomFibreComponent loaded = RoundTrip(seed);

        EXPECT_FLOAT_EQ(loaded.m_BaseColor.r, seed.m_BaseColor.r);
        EXPECT_FLOAT_EQ(loaded.m_BaseColor.g, seed.m_BaseColor.g);
        EXPECT_FLOAT_EQ(loaded.m_BaseColor.b, seed.m_BaseColor.b);
        EXPECT_FLOAT_EQ(loaded.m_Absorption.r, seed.m_Absorption.r);
        EXPECT_FLOAT_EQ(loaded.m_Absorption.g, seed.m_Absorption.g);
        EXPECT_FLOAT_EQ(loaded.m_Absorption.b, seed.m_Absorption.b);
        EXPECT_FLOAT_EQ(loaded.m_Eumelanin, seed.m_Eumelanin);
        EXPECT_FLOAT_EQ(loaded.m_Pheomelanin, seed.m_Pheomelanin);
        EXPECT_FLOAT_EQ(loaded.m_LongitudinalRoughness, seed.m_LongitudinalRoughness);
        EXPECT_FLOAT_EQ(loaded.m_AzimuthalRoughness, seed.m_AzimuthalRoughness);
        EXPECT_FLOAT_EQ(loaded.m_TiltDegrees, seed.m_TiltDegrees);
        EXPECT_FLOAT_EQ(loaded.m_IndexOfRefraction, seed.m_IndexOfRefraction);
        EXPECT_FLOAT_EQ(loaded.m_Intensity, seed.m_Intensity);
        EXPECT_EQ(loaded.m_HSamples, seed.m_HSamples);
        EXPECT_EQ(loaded.m_PigmentMode, seed.m_PigmentMode);
        EXPECT_EQ(loaded.m_DebugMode, seed.m_DebugMode);
        EXPECT_EQ(loaded.m_Enabled, seed.m_Enabled);

        // The whole-object equality, which is the statement that nothing was
        // missed — the per-field checks above exist to say WHICH field moved
        // when this one fails.
        EXPECT_TRUE(loaded == seed);
    }

    TEST(GroomFibreMaterialSaveLoad, TheMaterialRoundTripsIntoTheSameRenderedParameters)
    {
        // The round trip that actually matters. Two components can compare
        // equal field by field and still derive different model parameters if
        // the load path mis-handles a mode — so the check that closes the loop
        // is on the DERIVED GroomFibreParams, which is what the renderer and
        // the shader consume.
        GroomFibreComponent seed = MakeAuthoredMaterial();
        seed.m_PigmentMode = static_cast<u8>(GroomFibrePigmentMode::Melanin);
        const GroomFibreComponent loaded = RoundTrip(seed);

        const GroomFibreParams before = MakeGroomFibreParams(MakeGroomFibreAuthoring(seed));
        const GroomFibreParams after = MakeGroomFibreParams(MakeGroomFibreAuthoring(loaded));
        EXPECT_TRUE(before == after);
    }

    TEST(GroomFibreMaterialSaveLoad, ACorruptSaveIsSanitisedRatherThanPassedToTheShader)
    {
        // A save file is untrusted input. These are the values that reach a
        // pow(), a log() and a division in the fragment shader, where a NaN is
        // not a wrong colour — it is a NaN written into scene colour and then
        // spread across the frame by the post chain.
        std::vector<u8> buffer;
        {
            FMemoryWriter writer(buffer);
            writer.ArIsSaveGame = true;
            writer.SetArchiveVersion(kSaveGameFormatVersion);
            GroomFibreComponent corrupt;
            corrupt.m_BaseColor = glm::vec3(std::numeric_limits<f32>::quiet_NaN(), -3.0f, 12.0f);
            corrupt.m_Absorption = glm::vec3(std::numeric_limits<f32>::infinity(), -1.0f, 1.0e30f);
            corrupt.m_Eumelanin = std::numeric_limits<f32>::quiet_NaN();
            corrupt.m_Pheomelanin = -12.0f;
            corrupt.m_LongitudinalRoughness = 0.0f;
            corrupt.m_AzimuthalRoughness = std::numeric_limits<f32>::infinity();
            corrupt.m_TiltDegrees = 9000.0f;
            corrupt.m_IndexOfRefraction = 0.5f;
            corrupt.m_Intensity = -std::numeric_limits<f32>::infinity();
            corrupt.m_HSamples = 999999u;
            corrupt.m_PigmentMode = 200u;
            corrupt.m_DebugMode = 200u;
            SaveGameComponentSerializer::Serialize(writer, corrupt);
        }

        GroomFibreComponent loaded{};
        FMemoryReader reader(buffer);
        reader.ArIsSaveGame = true;
        reader.SetArchiveVersion(kSaveGameFormatVersion);
        SaveGameComponentSerializer::Serialize(reader, loaded);
        ASSERT_FALSE(reader.IsError());

        for (int channel = 0; channel < 3; ++channel)
        {
            EXPECT_TRUE(std::isfinite(loaded.m_BaseColor[channel]));
            EXPECT_GE(loaded.m_BaseColor[channel], 0.0f);
            EXPECT_LE(loaded.m_BaseColor[channel], 1.0f);
            EXPECT_TRUE(std::isfinite(loaded.m_Absorption[channel]));
            EXPECT_GE(loaded.m_Absorption[channel], 0.0f);
            EXPECT_LE(loaded.m_Absorption[channel], GroomFibreLimits::MaxAbsorption);
        }
        EXPECT_TRUE(std::isfinite(loaded.m_Eumelanin));
        EXPECT_GE(loaded.m_Pheomelanin, 0.0f);
        EXPECT_GE(loaded.m_LongitudinalRoughness, GroomFibreLimits::MinRoughness);
        EXPECT_TRUE(std::isfinite(loaded.m_AzimuthalRoughness));
        EXPECT_LE(loaded.m_TiltDegrees, GroomFibreLimits::MaxTiltDegrees);
        EXPECT_GE(loaded.m_IndexOfRefraction, GroomFibreLimits::MinIOR);
        EXPECT_GE(loaded.m_Intensity, 0.0f);
        EXPECT_LE(loaded.m_HSamples, GroomFibreLimits::MaxHSamples);

        // REJECTED to the constructor default, not saturated onto the nearest
        // valid neighbour. Saturating a corrupt pigment mode to Absorption
        // would render whatever happened to be in the absorption fields — a
        // perfectly legal material that nobody authored.
        EXPECT_EQ(loaded.m_PigmentMode, static_cast<u8>(GroomFibrePigmentMode::Melanin));
        EXPECT_EQ(loaded.m_DebugMode, static_cast<u8>(GroomFibreDebugMode::Full));

        // And the whole thing evaluates finite, which is the property all of
        // the above exists for rather than the clamping itself.
        const GroomFibreLobeSet lobes =
            GroomFibreEvaluateFar(MakeGroomFibreParams(MakeGroomFibreAuthoring(loaded)), 0.3f, -0.2f, 1.0f);
        for (u32 lobe = 0; lobe < kGroomFibreLobeCount; ++lobe)
        {
            for (int channel = 0; channel < 3; ++channel)
            {
                EXPECT_TRUE(std::isfinite(lobes.Lobe[lobe][channel]));
            }
        }
    }
} // namespace OloEngine::Tests
