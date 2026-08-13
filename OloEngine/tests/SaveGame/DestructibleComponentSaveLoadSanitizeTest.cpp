// OLO_TEST_LAYER: unit
// =============================================================================
// DestructibleComponentSaveLoadSanitizeTest.cpp
//
// Pins the on-load sanitization of DestructibleComponent in the BINARY save-game
// path (issue #459). The scene-YAML path gets its bounds for free from the
// OLO_SERIALIZE(Clamp/Skip) annotations in Components.h; the save-game path is
// HAND-WRITTEN in SaveGameComponentSerializer.cpp and inherits nothing from them,
// so the two can drift silently. A corrupt/hand-edited save must not be able to
// feed NaN or absurd magnitudes into the debris physics.
//
// Also pins the two runtime-flag rules the save path must honour: m_Broken is
// PERSISTED (a saved-broken object must not re-shatter on load) and m_PendingBreak
// is transient (always reset).
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/SaveGame/SaveGameComponentSerializer.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Serialization/Archive.h"
#include "OloEngine/Serialization/ArchiveExtensions.h"

#include <cmath>
#include <limits>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        // Write `seed` out, read it back into a fresh component through the binary
        // save-game path, and hand back what the loader produced.
        template<typename T>
        [[nodiscard]] T RoundTrip(T seed)
        {
            std::vector<u8> buffer;
            {
                FMemoryWriter writer(buffer);
                writer.ArIsSaveGame = true;
                SaveGameComponentSerializer::Serialize(writer, seed);
            }

            T loaded{};
            FMemoryReader reader(buffer);
            reader.ArIsSaveGame = true;
            SaveGameComponentSerializer::Serialize(reader, loaded);
            EXPECT_FALSE(reader.IsError());
            EXPECT_TRUE(reader.AtEnd()) << "reader did not consume exactly the payload — field-order desync";
            return loaded;
        }
    } // namespace

    TEST(DestructibleComponentSaveLoadSanitize, CleanValuesRoundTripUnchanged)
    {
        // A clean save must be a no-op through the sanitizer.
        DestructibleComponent seed;
        seed.m_Health = 42.5f;
        seed.m_MaxHealth = 250.0f;
        seed.m_DamageThreshold = 5.5f;
        seed.m_ChunkMesh = AssetHandle(0x0123456789ABCDEFull);
        seed.m_ChunkCount = 12;
        seed.m_ChunkScale = 0.4f;
        seed.m_ChunkMass = 2.5f;
        seed.m_ExplosionImpulse = 9.0f;
        seed.m_DebrisLifetime = 3.5f;
        seed.m_BreakOnJointBreak = false;
        seed.m_DestroyOnBreak = false;
        seed.m_Broken = true;       // must PERSIST
        seed.m_PendingBreak = true; // must RESET

        const DestructibleComponent loaded = RoundTrip(seed);

        EXPECT_EQ(loaded, seed) << "clean authored fields must survive intact";
        EXPECT_EQ(static_cast<u64>(loaded.m_ChunkMesh), 0x0123456789ABCDEFull);
        EXPECT_TRUE(loaded.m_Broken) << "m_Broken must persist so a saved-broken object does not re-shatter";
        EXPECT_FALSE(loaded.m_PendingBreak) << "m_PendingBreak is transient runtime state and must reset on load";
    }

    TEST(DestructibleComponentSaveLoadSanitize, NonFiniteAndOutOfRangeValuesAreSanitized)
    {
        const f32 nan = std::numeric_limits<f32>::quiet_NaN();
        const f32 inf = std::numeric_limits<f32>::infinity();

        DestructibleComponent seed;
        seed.m_Health = nan;            // -> 0 (low clamp)
        seed.m_MaxHealth = -inf;        // -> 0
        seed.m_DamageThreshold = -5.0f; // -> 0
        seed.m_ChunkMass = nan;         // -> 0.001 (low clamp)
        seed.m_ExplosionImpulse = -inf; // -> 0
        seed.m_DebrisLifetime = nan;    // -> 0
        seed.m_ChunkScale = 999.0f;     // -> 10 (max)
        seed.m_ChunkCount = 5000;       // -> 64 (cap)

        const DestructibleComponent loaded = RoundTrip(seed);

        EXPECT_FLOAT_EQ(loaded.m_Health, 0.0f);
        EXPECT_FLOAT_EQ(loaded.m_MaxHealth, 0.0f);
        EXPECT_FLOAT_EQ(loaded.m_DamageThreshold, 0.0f);
        EXPECT_FLOAT_EQ(loaded.m_ChunkMass, 0.001f);
        EXPECT_FLOAT_EQ(loaded.m_ExplosionImpulse, 0.0f);
        EXPECT_FLOAT_EQ(loaded.m_DebrisLifetime, 0.0f);
        EXPECT_FLOAT_EQ(loaded.m_ChunkScale, 10.0f);
        EXPECT_EQ(loaded.m_ChunkCount, 64u);
    }

    TEST(DestructibleComponentSaveLoadSanitize, ChunkScaleBelowFloorClampsUp)
    {
        DestructibleComponent seed;
        seed.m_ChunkScale = 0.0f; // below the 0.01 floor
        const DestructibleComponent loaded = RoundTrip(seed);
        EXPECT_FLOAT_EQ(loaded.m_ChunkScale, 0.01f);
    }
} // namespace OloEngine::Tests
