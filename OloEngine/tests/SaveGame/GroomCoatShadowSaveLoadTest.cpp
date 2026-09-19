// OLO_TEST_LAYER: unit
// =============================================================================
// GroomCoatShadowSaveLoadTest.cpp
//
// The save-game cell for issue #1248's coat-shadow component.
//
// Three things can go wrong here and all three are silent.
//
//   * A FIELD IN THE WRONG ORDER desynchronises the fixed-order archive, which
//     corrupts every component written after this one rather than only this
//     one. The `AtEnd()` check below is what turns that into a failure.
//
//   * A CORRUPT VALUE THAT IS NOT SANITISED reaches an exp() and a divide
//     inside a fragment shader's march. The OLO_SERIALIZE annotations on the
//     component's fields guard scene YAML and the live-write registries; they
//     do NOT reach this archive, so a save game is the one route into the
//     renderer that bypasses every one of them. A zero resolution divides by
//     zero deriving the voxel size; a corrupt one sizes an allocation.
//
//   * A MODE INDEX SATURATED INSTEAD OF REJECTED turns a corrupt value into a
//     different VALID representation, and the coat is then shadowed by
//     something nobody authored — which looks entirely plausible.
//
// NO VERSION BAND IS TESTED, and there is none to test: the component is new,
// so a save written before it existed does not contain its key and the load
// never reaches this function. That is stated here rather than left as an
// absence, because "where is the old-version test" is the first question this
// file should answer.
// =============================================================================

#include "OloEnginePCH.h"

#include "OloEngine/Groom/GroomCoatShadow.h"
#include "OloEngine/SaveGame/SaveGameComponentSerializer.h"
#include "OloEngine/SaveGame/SaveGameTypes.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Serialization/Archive.h"
#include "OloEngine/Serialization/ArchiveExtensions.h"

#include <gtest/gtest.h>

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
        [[nodiscard]] GroomCoatShadowComponent MakeAuthoredCoat()
        {
            GroomCoatShadowComponent component;
            component.m_Kappa = 2.25f;
            component.m_Resolution = 128u;
            component.m_StepVoxels = 1.5f;
            component.m_MaxLodSteps = 5u;
            component.m_PixelSizeForLod0 = 1024.0f;
            component.m_MinResolution = 16u;
            component.m_Mode = static_cast<u8>(GroomCoatShadow::CoatShadowMode::IsotropicDensityVolume);
            component.m_Enabled = true;
            return component;
        }

        [[nodiscard]] GroomCoatShadowComponent RoundTrip(const GroomCoatShadowComponent& seed)
        {
            std::vector<u8> buffer;
            {
                FMemoryWriter writer(buffer);
                writer.ArIsSaveGame = true;
                writer.SetArchiveVersion(kSaveGameFormatVersion);
                GroomCoatShadowComponent copy = seed;
                SaveGameComponentSerializer::Serialize(writer, copy);
            }

            GroomCoatShadowComponent loaded{};
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

    TEST(GroomCoatShadowSaveLoad, EveryAuthoredFieldSurvivesTheRoundTrip)
    {
        const GroomCoatShadowComponent seed = MakeAuthoredCoat();
        const GroomCoatShadowComponent loaded = RoundTrip(seed);

        EXPECT_FLOAT_EQ(loaded.m_Kappa, seed.m_Kappa);
        EXPECT_EQ(loaded.m_Resolution, seed.m_Resolution);
        EXPECT_FLOAT_EQ(loaded.m_StepVoxels, seed.m_StepVoxels);
        EXPECT_EQ(loaded.m_MaxLodSteps, seed.m_MaxLodSteps);
        EXPECT_FLOAT_EQ(loaded.m_PixelSizeForLod0, seed.m_PixelSizeForLod0);
        EXPECT_EQ(loaded.m_MinResolution, seed.m_MinResolution);
        EXPECT_EQ(loaded.m_Mode, seed.m_Mode);
        EXPECT_EQ(loaded.m_Enabled, seed.m_Enabled);

        // The whole-object equality, which is the statement that nothing was
        // missed — the per-field checks above exist to say WHICH field moved
        // when this one fails.
        EXPECT_TRUE(loaded == seed);
    }

    TEST(GroomCoatShadowSaveLoad, TheComponentRoundTripsIntoTheSameResolvedPolicy)
    {
        // The round trip that actually matters. Two components can compare
        // equal field by field and still resolve a different LOD policy if the
        // load path mis-handles one — so the check that closes the loop is on
        // the DERIVED policy, which is what the renderer consumes.
        const GroomCoatShadowComponent seed = MakeAuthoredCoat();
        const GroomCoatShadowComponent loaded = RoundTrip(seed);

        const GroomCoatShadow::CoatLodPolicy before = MakeGroomCoatLodPolicy(seed);
        const GroomCoatShadow::CoatLodPolicy after = MakeGroomCoatLodPolicy(loaded);
        EXPECT_EQ(before.BaseResolution, after.BaseResolution);
        EXPECT_EQ(before.MaxLodSteps, after.MaxLodSteps);
        EXPECT_FLOAT_EQ(before.PixelSizeForLod0, after.PixelSizeForLod0);
        EXPECT_EQ(before.MinResolution, after.MinResolution);
        EXPECT_EQ(MakeGroomCoatShadowMode(seed), MakeGroomCoatShadowMode(loaded));
    }

    TEST(GroomCoatShadowSaveLoad, ACorruptSaveIsSanitisedRatherThanPassedToTheShader)
    {
        // A save file is untrusted input. These values reach an exp() and a
        // divide in the fragment shader's march, where a NaN is not a wrong
        // shadow — it is a NaN written into scene colour and then spread across
        // the frame by the post chain.
        GroomCoatShadowComponent corrupt{};
        corrupt.m_Kappa = std::numeric_limits<f32>::quiet_NaN();
        corrupt.m_StepVoxels = -1.0f;
        corrupt.m_PixelSizeForLod0 = std::numeric_limits<f32>::infinity();
        // A zero resolution is the one that divides by zero deriving the voxel
        // size; the huge one is what would size an allocation from a corrupt
        // byte.
        corrupt.m_Resolution = 0u;
        corrupt.m_MaxLodSteps = 4000u;
        corrupt.m_MinResolution = 9999u;
        corrupt.m_Mode = 200u;

        const GroomCoatShadowComponent loaded = RoundTrip(corrupt);

        EXPECT_TRUE(std::isfinite(loaded.m_Kappa));
        EXPECT_GE(loaded.m_Kappa, 0.0f);
        EXPECT_LE(loaded.m_Kappa, 16.0f);

        EXPECT_TRUE(std::isfinite(loaded.m_StepVoxels));
        EXPECT_GT(loaded.m_StepVoxels, 0.0f);

        EXPECT_TRUE(std::isfinite(loaded.m_PixelSizeForLod0));
        EXPECT_GT(loaded.m_PixelSizeForLod0, 0.0f);

        EXPECT_GE(loaded.m_Resolution, 8u);
        EXPECT_LE(loaded.m_Resolution, 256u);
        EXPECT_LE(loaded.m_MaxLodSteps, 6u);
        EXPECT_LE(loaded.m_MinResolution, 64u);

        // REJECTED, not clamped onto a neighbouring valid mode.
        EXPECT_TRUE(GroomCoatShadow::IsValidCoatShadowMode(static_cast<i32>(loaded.m_Mode)));
        EXPECT_EQ(loaded.m_Mode, static_cast<u8>(GroomCoatShadow::CoatShadowMode::AnisotropicDensityVolume));

        // And the derived policy is usable: a floor above the base resolution
        // would make every coat fall back for a reason that is true but that
        // nobody authored.
        const GroomCoatShadow::CoatLodPolicy policy = MakeGroomCoatLodPolicy(loaded);
        EXPECT_LE(policy.MinResolution, policy.BaseResolution);
        EXPECT_GT(policy.BaseResolution, 0u);
    }

    TEST(GroomCoatShadowSaveLoad, ADisabledComponentResolvesToNoCoatShadowing)
    {
        // Disabled must mean UNSHADOWED, not "shadowed with the default mode".
        // The unshadowed coat is #1247's picture and the A/B control every
        // capture in this feature's evidence is measured against.
        GroomCoatShadowComponent seed = MakeAuthoredCoat();
        seed.m_Enabled = false;
        const GroomCoatShadowComponent loaded = RoundTrip(seed);

        EXPECT_FALSE(loaded.m_Enabled);
        EXPECT_EQ(MakeGroomCoatShadowMode(loaded), GroomCoatShadow::CoatShadowMode::None);
        // The MODE itself is preserved, so turning the component back on
        // restores what was authored rather than a default.
        EXPECT_EQ(loaded.m_Mode, seed.m_Mode);
    }
} // namespace OloEngine::Tests
