#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/ReflectionProbeBaker.h"

#include <glm/glm.hpp>

#include <array>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file, brevity preferred

// =============================================================================
// SelectDominantReflectionProbe — pure geometry, no renderer required.
// Matches the math used by Scene::ApplyReflectionProbeOverride.
// =============================================================================

TEST(ReflectionProbeSelection, EmptyListReturnsNoMatch)
{
    EXPECT_EQ(SelectDominantReflectionProbe(glm::vec3(0.0f), {}), -1);
}

TEST(ReflectionProbeSelection, CameraOutsideAllProbesReturnsNoMatch)
{
    std::array<ReflectionProbeRef, 2> probes{ {
        { glm::vec3(100.0f, 0.0f, 0.0f), 1.0f },
        { glm::vec3(0.0f, 100.0f, 0.0f), 1.0f },
    } };
    EXPECT_EQ(SelectDominantReflectionProbe(glm::vec3(0.0f), probes), -1);
}

TEST(ReflectionProbeSelection, CameraExactlyOnBoundaryCounts)
{
    // Camera sits at distance == InfluenceRadius. Math uses distSq <= radiusSq
    // (the early-out is distSq > radiusSq), so this should be a hit.
    std::array<ReflectionProbeRef, 1> probes{ {
        { glm::vec3(0.0f), 5.0f },
    } };
    EXPECT_EQ(SelectDominantReflectionProbe(glm::vec3(5.0f, 0.0f, 0.0f), probes), 0);
}

TEST(ReflectionProbeSelection, SingleContainingProbeWins)
{
    std::array<ReflectionProbeRef, 3> probes{ {
        { glm::vec3(100.0f, 0.0f, 0.0f), 1.0f },  // out of range
        { glm::vec3(2.0f, 0.0f, 0.0f), 5.0f },    // contains camera
        { glm::vec3(-100.0f, 0.0f, 0.0f), 1.0f }, // out of range
    } };
    EXPECT_EQ(SelectDominantReflectionProbe(glm::vec3(0.0f), probes), 1);
}

TEST(ReflectionProbeSelection, PicksClosestWhenMultipleContain)
{
    // Both probes contain the origin (camera). Probe at distance 1 should win
    // over probe at distance 4 even though the farther probe has a larger radius.
    std::array<ReflectionProbeRef, 2> probes{ {
        { glm::vec3(4.0f, 0.0f, 0.0f), 10.0f },
        { glm::vec3(1.0f, 0.0f, 0.0f), 2.0f },
    } };
    EXPECT_EQ(SelectDominantReflectionProbe(glm::vec3(0.0f), probes), 1);
}

TEST(ReflectionProbeSelection, DegenerateZeroRadiusProbeIgnored)
{
    // A probe with radius 0 only matches a camera exactly at its center —
    // a degenerate case. Verify it doesn't accidentally claim a nearby camera.
    std::array<ReflectionProbeRef, 1> probes{ {
        { glm::vec3(0.0f), 0.0f },
    } };
    EXPECT_EQ(SelectDominantReflectionProbe(glm::vec3(0.001f, 0.0f, 0.0f), probes), -1);
}
