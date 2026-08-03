// OLO_TEST_LAYER: unit
// =============================================================================
// BoidComponentSaveLoadSanitizeTest.cpp
//
// Pins the on-load sanitization of BoidComponent / BoidObstacleComponent in
// the BINARY save-game path (issue #731).
//
// Why this needs its own test rather than a comment: the same bounds are
// declared in two places by two different mechanisms.
//
//   * Scene YAML gets them for free — OLO_SERIALIZE(Clamp, Min = …, Max = …)
//     in AIComponents.h, applied by OloHeaderTool-generated deserialize code.
//   * The save-game path is HAND-WRITTEN in SaveGameComponentSerializer.cpp
//     and inherits nothing from the annotation.
//
// So the two can drift silently, and the failure mode is not a crash — it is a
// save file that loads values the scene loader would have rejected, feeding
// NaN or absurd magnitudes straight into the steering maths. That is exactly
// the divergence class that made VehicleComponent::m_DriveMode need Reject
// instead of Clamp: both paths "worked", they just disagreed.
//
// These tests are deliberately written against the DEFAULTS and the declared
// ranges rather than against the sanitize code's internals, so they fail if
// either side of the pair moves without the other.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/AI/AIComponents.h"
#include "OloEngine/SaveGame/SaveGameComponentSerializer.h"
#include "OloEngine/Serialization/Archive.h"
#include "OloEngine/Serialization/ArchiveExtensions.h"

#include <cmath>
#include <limits>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        // Write `seed` out, read it back into a fresh component, and hand back
        // what the loader produced.
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

    TEST(BoidComponentSaveLoadSanitize, CleanValuesRoundTripUnchanged)
    {
        // The sanitizer must be a no-op on legitimate data — a clamp that
        // quietly rewrites valid saves is worse than no clamp at all.
        BoidComponent seed;
        seed.m_MaxSpeed = 9.5f;
        seed.m_MaxForce = 22.0f;
        seed.m_NeighborRadius = 7.25f;
        seed.m_SeparationRadius = 2.5f;
        seed.m_MaxNeighbors = 48;
        seed.m_SeparationWeight = 2.25f;
        seed.m_AlignmentWeight = 0.75f;
        seed.m_CohesionWeight = 1.25f;
        seed.m_GoalWeight = 3.0f;
        seed.m_GoalPosition = { 12.0f, -3.0f, 40.0f };
        seed.m_ObstacleAvoidWeight = 4.0f;
        seed.m_ObstacleAvoidRadius = 8.0f;
        seed.m_LockYAxis = true;
        seed.m_FaceVelocity = false;
        seed.m_Velocity = { 1.5f, 0.0f, -2.5f };

        const BoidComponent loaded = RoundTrip(seed);

        EXPECT_EQ(loaded, seed) << "a clean BoidComponent did not survive the save/load round trip intact";
    }

    TEST(BoidComponentSaveLoadSanitize, NonFiniteScalarsFallBackToDefaults)
    {
        // A corrupt or hand-edited save must not be able to inject NaN/Inf into
        // the steering solve. Every scalar falls back to its constructor
        // default rather than to an arbitrary sentinel.
        const f32 nan = std::numeric_limits<f32>::quiet_NaN();
        const f32 inf = std::numeric_limits<f32>::infinity();

        BoidComponent seed;
        seed.m_MaxSpeed = nan;
        seed.m_MaxForce = inf;
        seed.m_NeighborRadius = -inf;
        seed.m_SeparationRadius = nan;
        seed.m_SeparationWeight = nan;
        seed.m_AlignmentWeight = inf;
        seed.m_CohesionWeight = nan;
        seed.m_GoalWeight = nan;
        seed.m_ObstacleAvoidWeight = nan;
        seed.m_ObstacleAvoidRadius = nan;

        const BoidComponent loaded = RoundTrip(seed);
        const BoidComponent defaults;

        EXPECT_FLOAT_EQ(loaded.m_MaxSpeed, defaults.m_MaxSpeed);
        EXPECT_FLOAT_EQ(loaded.m_MaxForce, defaults.m_MaxForce);
        EXPECT_FLOAT_EQ(loaded.m_NeighborRadius, defaults.m_NeighborRadius);
        EXPECT_FLOAT_EQ(loaded.m_SeparationRadius, defaults.m_SeparationRadius);
        EXPECT_FLOAT_EQ(loaded.m_SeparationWeight, defaults.m_SeparationWeight);
        EXPECT_FLOAT_EQ(loaded.m_AlignmentWeight, defaults.m_AlignmentWeight);
        EXPECT_FLOAT_EQ(loaded.m_CohesionWeight, defaults.m_CohesionWeight);
        EXPECT_FLOAT_EQ(loaded.m_GoalWeight, defaults.m_GoalWeight);
        EXPECT_FLOAT_EQ(loaded.m_ObstacleAvoidWeight, defaults.m_ObstacleAvoidWeight);
        EXPECT_FLOAT_EQ(loaded.m_ObstacleAvoidRadius, defaults.m_ObstacleAvoidRadius);
    }

    TEST(BoidComponentSaveLoadSanitize, NonFiniteVectorsFallBackWholeVector)
    {
        // Vec3s fall back as a WHOLE vector, matching the generated YAML decode
        // (DecodeVec3 rejects the vector, not one component) — a half-restored
        // position would be a new, plausible-looking wrong answer.
        BoidComponent seed;
        seed.m_GoalPosition = { 5.0f, std::numeric_limits<f32>::quiet_NaN(), 7.0f };
        seed.m_Velocity = { std::numeric_limits<f32>::infinity(), 1.0f, 2.0f };

        const BoidComponent loaded = RoundTrip(seed);

        EXPECT_FLOAT_EQ(loaded.m_GoalPosition.x, 0.0f);
        EXPECT_FLOAT_EQ(loaded.m_GoalPosition.y, 0.0f);
        EXPECT_FLOAT_EQ(loaded.m_GoalPosition.z, 0.0f);
        EXPECT_FLOAT_EQ(loaded.m_Velocity.x, 0.0f);
        EXPECT_FLOAT_EQ(loaded.m_Velocity.y, 0.0f);
        EXPECT_FLOAT_EQ(loaded.m_Velocity.z, 0.0f);
    }

    TEST(BoidComponentSaveLoadSanitize, OutOfRangeValuesClampToTheAnnotatedBounds)
    {
        // The bounds asserted here are the ones declared by
        // OLO_SERIALIZE(Clamp, ...) in AIComponents.h. If either side moves
        // without the other, this fails — which is the whole point: the
        // annotation and the hand-written save path must agree.
        BoidComponent seed;
        seed.m_MaxSpeed = 1.0e6f;         // Max 1000
        seed.m_MaxForce = -5.0f;          // Min 0
        seed.m_NeighborRadius = 1.0e6f;   // Max 1000
        seed.m_SeparationRadius = 0.0f;   // Min 0.01
        seed.m_MaxNeighbors = 100000u;    // Max 4096
        seed.m_SeparationWeight = 500.0f; // Max 100
        seed.m_GoalWeight = -1.0f;        // Min 0

        const BoidComponent loaded = RoundTrip(seed);

        EXPECT_FLOAT_EQ(loaded.m_MaxSpeed, 1000.0f);
        EXPECT_FLOAT_EQ(loaded.m_MaxForce, 0.0f);
        EXPECT_FLOAT_EQ(loaded.m_NeighborRadius, 1000.0f);
        EXPECT_FLOAT_EQ(loaded.m_SeparationRadius, 0.01f);
        EXPECT_EQ(loaded.m_MaxNeighbors, 4096u);
        EXPECT_FLOAT_EQ(loaded.m_SeparationWeight, 100.0f);
        EXPECT_FLOAT_EQ(loaded.m_GoalWeight, 0.0f);
    }

    TEST(BoidComponentSaveLoadSanitize, MaxNeighborsNeverLoadsAsZero)
    {
        // Zero would make the steering sweep terminate before its first
        // neighbour — a flock that silently stops flocking. The declared Min is
        // 1, so a zero on disk must come back as 1.
        BoidComponent seed;
        seed.m_MaxNeighbors = 0;

        EXPECT_EQ(RoundTrip(seed).m_MaxNeighbors, 1u);
    }

    TEST(BoidObstacleComponentSaveLoadSanitize, RadiusIsSanitizedOnLoad)
    {
        BoidObstacleComponent clean;
        clean.m_Radius = 12.5f;
        EXPECT_FLOAT_EQ(RoundTrip(clean).m_Radius, 12.5f);

        // A zero or negative radius would make the avoidance ramp divide
        // through a degenerate reach; a non-finite one poisons the grid.
        BoidObstacleComponent zero;
        zero.m_Radius = 0.0f;
        EXPECT_FLOAT_EQ(RoundTrip(zero).m_Radius, 0.01f);

        BoidObstacleComponent nan;
        nan.m_Radius = std::numeric_limits<f32>::quiet_NaN();
        EXPECT_FLOAT_EQ(RoundTrip(nan).m_Radius, BoidObstacleComponent{}.m_Radius);

        BoidObstacleComponent huge;
        huge.m_Radius = 1.0e9f;
        EXPECT_FLOAT_EQ(RoundTrip(huge).m_Radius, 10000.0f);
    }
} // namespace OloEngine::Tests
