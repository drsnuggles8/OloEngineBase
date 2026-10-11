// OLO_TEST_LAYER: L1
// =============================================================================
// FoliageCullStreamSizingContractTest — issue #1581.
//
// A foliage cull slot's compacted stream is sized from what its view can
// survive, with headroom, and grows when a view overflows it. These pin the
// CPU half of that rule; FoliageGPUCullEvidenceTest
// .ViewSizedSlotGrowsAfterOneCountedOverflow drives the overflow and the grow
// through the real kernels.
//
//   * the headroom rule and its two clamps (StreamCapacityFor);
//   * the demand pair sits past the part of the header the CPU seeds, so a
//     later cull into the same slot cannot erase an earlier overflow, and the
//     GLSL block declares the same members in the same order.
// =============================================================================

#include "OloEnginePCH.h"

// Complete types: FoliageGPUCuller.h holds Ref<StorageBuffer> / Ref<VertexBuffer>
// by value and only forward-declares them.
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/VertexBuffer.h"
#include "OloEngine/Terrain/Foliage/FoliageGPUCuller.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#ifndef OLO_TEST_EDITOR_ROOT
#error "OLO_TEST_EDITOR_ROOT must be defined by the test target's CMake — see OloEngine/tests/CMakeLists.txt"
#endif

namespace OloEngine::Tests
{
    TEST(FoliageCullStreamSizing, CapacityIsTheSurvivorsWithHalfAgainOfHeadroom)
    {
        // The rule: survivors + survivors / 2.
        EXPECT_EQ(FoliageGPUCuller::StreamCapacityFor(40'000u, 358'000u), 60'000u);
        EXPECT_EQ(FoliageGPUCuller::StreamCapacityFor(100'001u, 1'000'000u), 150'001u);
        // Monotonic: a view that survives more never gets a smaller stream.
        u32 previous = 0;
        for (u32 survivors = 0; survivors <= 200'000u; survivors += 997u)
        {
            const u32 capacity = FoliageGPUCuller::StreamCapacityFor(survivors, 150'000u);
            EXPECT_GE(capacity, previous) << survivors;
            previous = capacity;
        }
    }

    TEST(FoliageCullStreamSizing, CapacityNeverExceedsTheLayerNorDropsBelowTheFloor)
    {
        // Ceiling: no cull can append more rows than the layer has.
        EXPECT_EQ(FoliageGPUCuller::StreamCapacityFor(300'000u, 358'000u), 358'000u);
        EXPECT_EQ(FoliageGPUCuller::StreamCapacityFor(~0u, 358'000u), 358'000u);
        // Floor: a view of the sky still gets a usable stream...
        EXPECT_EQ(FoliageGPUCuller::StreamCapacityFor(0u, 358'000u), FoliageGPUCuller::kMinStreamCapacity);
        EXPECT_EQ(FoliageGPUCuller::StreamCapacityFor(10u, 358'000u), FoliageGPUCuller::kMinStreamCapacity);
        // ...unless the layer itself is smaller, and an empty layer gets none.
        EXPECT_EQ(FoliageGPUCuller::StreamCapacityFor(0u, 300u), 300u);
        EXPECT_EQ(FoliageGPUCuller::StreamCapacityFor(5u, 0u), 0u);
    }

    TEST(FoliageCullStreamSizing, DemandPairIsPastTheSeededHeaderAndMirroredInGlsl)
    {
        // The CPU writes [0, kFoliageCullSeededHeaderBytes) every cull; the
        // demand pair must lie wholly past it or a later cull into the same
        // slot zeroes an overflow before it is read back.
        EXPECT_GE(offsetof(FoliageCullStateHeader, ListDemand), static_cast<sizet>(kFoliageCullSeededHeaderBytes));
        EXPECT_EQ(offsetof(FoliageCullStateHeader, MeshDemand), offsetof(FoliageCullStateHeader, ListDemand) + 4u);
        EXPECT_LT(offsetof(FoliageCullStateHeader, MeshCapacity), static_cast<sizet>(kFoliageCullSeededHeaderBytes));
        EXPECT_EQ(sizeof(FoliageCullDemand), 8u);

        // The GLSL twin declares the same tail of the header, in the same order,
        // each member exactly once.
        const std::filesystem::path glsl =
            std::filesystem::path{ OLO_TEST_EDITOR_ROOT } / "assets" / "shaders" / "include" / "FoliageCullCommon.glsl";
        std::ifstream in(glsl);
        ASSERT_TRUE(in) << glsl.string();
        std::stringstream buffer;
        buffer << in.rdbuf();
        const std::string text = buffer.str();
        const auto countOf = [&text](const std::string& needle)
        {
            sizet count = 0;
            for (sizet at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1))
                ++count;
            return count;
        };
        const std::string order[] = { "vec4 s_LodTransition1;", "uint s_MeshCapacity;", "uint s_ListDemand;",
                                      "uint s_MeshDemand;", "uint s_Tail[];" };
        sizet last = 0;
        for (const std::string& member : order)
        {
            EXPECT_EQ(countOf(member), 1u) << member;
            const sizet at = text.find(member);
            ASSERT_NE(at, std::string::npos) << member;
            EXPECT_GT(at, last) << member << " is out of order in FoliageCullCommon.glsl";
            last = at;
        }
    }
} // namespace OloEngine::Tests
