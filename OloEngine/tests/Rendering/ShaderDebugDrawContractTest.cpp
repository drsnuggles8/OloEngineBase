// OLO_TEST_LAYER: shaderpipe
// =============================================================================
// ShaderDebugDrawContractTest.cpp — issue #725
//
// The GPU-pushable debug-draw feature has a wire format shared by three parties
// that cannot see each other's declarations: the GLSL push helpers
// (`assets/shaders/include/DebugDrawCommon.glsl`), the GLSL draw-side expansion
// (`assets/shaders/DebugDrawPrimitives.glsl`), and the C++ mirror
// (`Renderer/Debug/ShaderDebugDrawTypes.h`). Nothing makes them agree at compile
// time.
//
// Every failure mode of a disagreement is quiet:
//   * a struct-size drift reinterprets every subsequent entry, so primitives
//     appear at plausible-but-wrong positions;
//   * a binding drift makes a channel write land in someone else's buffer;
//   * a segment-count drift draws a fraction of each primitive, which reads as
//     "the sphere looks a bit chunky" rather than as a bug;
//   * an enumerator reorder swaps two channels wholesale.
//
// So this file pins the layout in C++ AND cross-checks the two GLSL files as
// text. Text comparison is crude, but it is the only mechanism available: the
// shaders cannot be executed headlessly, and a wrong number here produces a
// picture, not a crash.
//
// Classification: L1 / shaderpipe (pure CPU, no GL context).
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Renderer/Debug/ShaderDebugDrawTypes.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        // Same resolution ladder as ShaderHarness::ResolveShaderRoot — the suite
        // runs from the repo root under ctest and from OloEditor/ when launched
        // by the editor.
        fs::path ResolveShaderRoot()
        {
            const fs::path candidates[] = {
                fs::path("OloEditor/assets/shaders"),
                fs::current_path() / "OloEditor/assets/shaders",
                fs::current_path().parent_path() / "OloEditor/assets/shaders",
                fs::current_path() / "assets" / "shaders",
            };
            for (const auto& candidate : candidates)
            {
                std::error_code ec;
                if (fs::exists(candidate, ec) && fs::is_directory(candidate, ec))
                    return fs::canonical(candidate, ec);
            }
            return {};
        }

        std::string ReadShader(const std::string& relativePath)
        {
            const fs::path root = ResolveShaderRoot();
            if (root.empty())
                return {};
            std::ifstream file(root / relativePath, std::ios::binary);
            std::ostringstream contents;
            contents << file.rdbuf();
            return contents.str();
        }

        constexpr auto kAllPrimitives = std::array{
            ShaderDebugDrawPrimitive::Line,
            ShaderDebugDrawPrimitive::Circle,
            ShaderDebugDrawPrimitive::Rectangle,
            ShaderDebugDrawPrimitive::AABB,
            ShaderDebugDrawPrimitive::Box,
            ShaderDebugDrawPrimitive::Cone,
            ShaderDebugDrawPrimitive::Sphere,
        };
    } // namespace

    // -------------------------------------------------------------------------
    // Layout
    // -------------------------------------------------------------------------

    TEST(ShaderDebugDrawContract, ChannelHeaderIsTheGLDrawArraysIndirectCommandFollowedByTheCounters)
    {
        // The first 16 bytes ARE the command glDrawArraysIndirect reads at offset
        // 0 of the channel buffer, in the GL-specified field order. Reordering
        // them draws garbage counts; the fact that it happens to still draw
        // *something* is why this is pinned rather than left to review.
        EXPECT_EQ(offsetof(ShaderDebugDrawChannelHeader, VertexCount), 0u);
        EXPECT_EQ(offsetof(ShaderDebugDrawChannelHeader, InstanceCount), 4u);
        EXPECT_EQ(offsetof(ShaderDebugDrawChannelHeader, First), 8u);
        EXPECT_EQ(offsetof(ShaderDebugDrawChannelHeader, BaseInstance), 12u);
        EXPECT_EQ(offsetof(ShaderDebugDrawChannelHeader, Capacity), 16u);
        EXPECT_EQ(offsetof(ShaderDebugDrawChannelHeader, RequestCount), 20u);
        EXPECT_EQ(sizeof(ShaderDebugDrawChannelHeader), 32u);
        EXPECT_EQ(ShaderDebugDrawContract::kEntryArrayOffset, 32u);
    }

    TEST(ShaderDebugDrawContract, EntryStructsMatchTheirStd430Layout)
    {
        // std430: a vec3 has 16-byte alignment and 12-byte size, so the trailing
        // scalar of each pair lands at +12 and the pair occupies exactly 16.
        EXPECT_EQ(sizeof(ShaderDebugDrawLine), 48u);
        EXPECT_EQ(offsetof(ShaderDebugDrawLine, Space), 12u);
        EXPECT_EQ(offsetof(ShaderDebugDrawLine, End), 16u);
        EXPECT_EQ(offsetof(ShaderDebugDrawLine, Color), 32u);

        EXPECT_EQ(sizeof(ShaderDebugDrawCircle), 48u);
        EXPECT_EQ(offsetof(ShaderDebugDrawCircle, Normal), 16u);
        EXPECT_EQ(offsetof(ShaderDebugDrawCircle, Radius), 28u);
        EXPECT_EQ(offsetof(ShaderDebugDrawCircle, Color), 32u);

        EXPECT_EQ(sizeof(ShaderDebugDrawRectangle), 64u);
        EXPECT_EQ(offsetof(ShaderDebugDrawRectangle, AxisU), 16u);
        EXPECT_EQ(offsetof(ShaderDebugDrawRectangle, AxisV), 32u);
        EXPECT_EQ(offsetof(ShaderDebugDrawRectangle, Color), 48u);

        EXPECT_EQ(sizeof(ShaderDebugDrawAABB), 48u);
        EXPECT_EQ(offsetof(ShaderDebugDrawAABB, Max), 16u);
        EXPECT_EQ(offsetof(ShaderDebugDrawAABB, Color), 32u);

        EXPECT_EQ(sizeof(ShaderDebugDrawBox), 144u);
        EXPECT_EQ(offsetof(ShaderDebugDrawBox, Color), 128u);
        EXPECT_EQ(offsetof(ShaderDebugDrawBox, Space), 140u);

        EXPECT_EQ(sizeof(ShaderDebugDrawCone), 48u);
        EXPECT_EQ(offsetof(ShaderDebugDrawCone, Axis), 16u);
        EXPECT_EQ(offsetof(ShaderDebugDrawCone, Radius), 28u);

        EXPECT_EQ(sizeof(ShaderDebugDrawSphere), 32u);
        EXPECT_EQ(offsetof(ShaderDebugDrawSphere, Radius), 12u);
        EXPECT_EQ(offsetof(ShaderDebugDrawSphere, Color), 16u);
        EXPECT_EQ(offsetof(ShaderDebugDrawSphere, Space), 28u);

        // Every entry must be a whole number of 16-byte units, or the entry array
        // stride the shader computes diverges from sizeof() on the C++ side and
        // the CPU-uploaded prefix stops aligning with the GPU-appended tail.
        for (const auto primitive : kAllPrimitives)
        {
            EXPECT_EQ(ShaderDebugDrawContract::EntryStride(primitive) % 16u, 0u)
                << ShaderDebugDrawContract::Name(primitive);
        }
    }

    TEST(ShaderDebugDrawContract, ParamsUBOIsStd140Clean)
    {
        EXPECT_EQ(offsetof(ShaderDebugDrawParamsUBO, ViewProjection), 0u);
        EXPECT_EQ(offsetof(ShaderDebugDrawParamsUBO, ObserverInvViewProjection), 64u);
        EXPECT_EQ(offsetof(ShaderDebugDrawParamsUBO, ViewportSize), 128u);
        EXPECT_EQ(offsetof(ShaderDebugDrawParamsUBO, LineWidth), 136u);
        EXPECT_EQ(offsetof(ShaderDebugDrawParamsUBO, PrimitiveType), 140u);
        EXPECT_EQ(sizeof(ShaderDebugDrawParamsUBO), 144u);
    }

    // -------------------------------------------------------------------------
    // Bindings
    // -------------------------------------------------------------------------

    TEST(ShaderDebugDrawContract, ChannelBindingsAreContiguousAndMatchTheEnumeratorOrder)
    {
        // ShaderDebugDraw derives a channel's binding as FIRST + enumerator, so a
        // reordered enum silently swaps two channels' buffers. Spell both sides
        // out here rather than deriving one from the other.
        EXPECT_EQ(ShaderBindingLayout::SSBO_DEBUG_DRAW_FIRST, ShaderBindingLayout::SSBO_DEBUG_DRAW_LINE);
        EXPECT_EQ(ShaderBindingLayout::SSBO_DEBUG_DRAW_COUNT, kShaderDebugDrawPrimitiveCount);

        const std::array<u32, kShaderDebugDrawPrimitiveCount> expected{
            ShaderBindingLayout::SSBO_DEBUG_DRAW_LINE,
            ShaderBindingLayout::SSBO_DEBUG_DRAW_CIRCLE,
            ShaderBindingLayout::SSBO_DEBUG_DRAW_RECTANGLE,
            ShaderBindingLayout::SSBO_DEBUG_DRAW_AABB,
            ShaderBindingLayout::SSBO_DEBUG_DRAW_BOX,
            ShaderBindingLayout::SSBO_DEBUG_DRAW_CONE,
            ShaderBindingLayout::SSBO_DEBUG_DRAW_SPHERE,
        };
        for (u32 i = 0; i < kShaderDebugDrawPrimitiveCount; ++i)
        {
            EXPECT_EQ(expected[i], ShaderBindingLayout::SSBO_DEBUG_DRAW_FIRST + i)
                << "Channel " << ShaderDebugDrawContract::Name(kAllPrimitives[i])
                << " is not at its enumerator's derived binding";
            EXPECT_EQ(static_cast<u32>(std::to_underlying(kAllPrimitives[i])), i);
        }
    }

    TEST(ShaderDebugDrawContract, PushHeaderDeclaresEachChannelAtItsCppBinding)
    {
        const std::string source = ReadShader("include/DebugDrawCommon.glsl");
        ASSERT_FALSE(source.empty()) << "Could not read include/DebugDrawCommon.glsl";

        const std::array<std::pair<u32, const char*>, kShaderDebugDrawPrimitiveCount> expected{ {
            { ShaderBindingLayout::SSBO_DEBUG_DRAW_LINE, "OloDebugDrawLineChannel" },
            { ShaderBindingLayout::SSBO_DEBUG_DRAW_CIRCLE, "OloDebugDrawCircleChannel" },
            { ShaderBindingLayout::SSBO_DEBUG_DRAW_RECTANGLE, "OloDebugDrawRectangleChannel" },
            { ShaderBindingLayout::SSBO_DEBUG_DRAW_AABB, "OloDebugDrawAABBChannel" },
            { ShaderBindingLayout::SSBO_DEBUG_DRAW_BOX, "OloDebugDrawBoxChannel" },
            { ShaderBindingLayout::SSBO_DEBUG_DRAW_CONE, "OloDebugDrawConeChannel" },
            { ShaderBindingLayout::SSBO_DEBUG_DRAW_SPHERE, "OloDebugDrawSphereChannel" },
        } };

        for (const auto& [binding, blockName] : expected)
        {
            const std::string declaration =
                "layout(std430, binding = " + std::to_string(binding) + ") buffer " + blockName;
            EXPECT_NE(source.find(declaration), std::string::npos)
                << "DebugDrawCommon.glsl does not declare " << blockName << " at binding " << binding
                << ". A push would then land in whatever buffer IS bound there.";
        }
    }

    TEST(ShaderDebugDrawContract, DrawShaderDeclaresItsParamsUBOAtTheCppBinding)
    {
        const std::string source = ReadShader("DebugDrawPrimitives.glsl");
        ASSERT_FALSE(source.empty()) << "Could not read DebugDrawPrimitives.glsl";

        const std::string declaration = "layout(std140, binding = " +
                                        std::to_string(ShaderBindingLayout::UBO_DEBUG_DRAW) +
                                        ") uniform DebugDrawParams";
        EXPECT_NE(source.find(declaration), std::string::npos);

        // The block name has to satisfy the engine's own binding-name validator,
        // which ShaderReflectionBindingTest runs over every discovered UBO.
        EXPECT_TRUE(ShaderBindingLayout::IsKnownUBOBinding(ShaderBindingLayout::UBO_DEBUG_DRAW, "DebugDrawParams"));
    }

    // -------------------------------------------------------------------------
    // Segment counts — the constants that live in BOTH the GLSL and the C++.
    // -------------------------------------------------------------------------

    TEST(ShaderDebugDrawContract, SegmentCountsMatchTheDrawShaderLiterals)
    {
        const std::string source = ReadShader("DebugDrawPrimitives.glsl");
        ASSERT_FALSE(source.empty()) << "Could not read DebugDrawPrimitives.glsl";

        const std::array<std::pair<const char*, u32>, 5> defines{ {
            { "OLO_DBG_CIRCLE_SEGMENTS", ShaderDebugDrawContract::kCircleSegments },
            { "OLO_DBG_SPHERE_RING_SEGMENTS", ShaderDebugDrawContract::kSphereRingSegments },
            { "OLO_DBG_CONE_RING_SEGMENTS", ShaderDebugDrawContract::kConeRingSegments },
            { "OLO_DBG_CONE_SIDE_LINES", ShaderDebugDrawContract::kConeSideLines },
            { "OLO_DBG_VERTS_PER_SEGMENT", ShaderDebugDrawContract::kVerticesPerSegment },
        } };

        for (const auto& [name, value] : defines)
        {
            const std::string expected = std::string("#define ") + name + " " + std::to_string(value) + "u";
            EXPECT_NE(source.find(expected), std::string::npos)
                << "DebugDrawPrimitives.glsl is missing `" << expected
                << "`. The C++ side writes the indirect draw's vertex count from its own copy of this "
                   "constant, so a mismatch draws a fraction of each primitive (or reads past its "
                   "segment table) rather than failing.";
        }
    }

    TEST(ShaderDebugDrawContract, VertexCountPerInstanceIsSixTimesTheSegmentCount)
    {
        // Each segment becomes one screen-space quad = 2 triangles = 6 vertices.
        // This is the number the CPU writes into DrawArraysIndirectCommand.count.
        EXPECT_EQ(ShaderDebugDrawContract::SegmentCount(ShaderDebugDrawPrimitive::Line), 1u);
        EXPECT_EQ(ShaderDebugDrawContract::SegmentCount(ShaderDebugDrawPrimitive::Rectangle), 4u);
        EXPECT_EQ(ShaderDebugDrawContract::SegmentCount(ShaderDebugDrawPrimitive::AABB), 12u);
        EXPECT_EQ(ShaderDebugDrawContract::SegmentCount(ShaderDebugDrawPrimitive::Box), 12u);
        EXPECT_EQ(ShaderDebugDrawContract::SegmentCount(ShaderDebugDrawPrimitive::Circle), 32u);
        EXPECT_EQ(ShaderDebugDrawContract::SegmentCount(ShaderDebugDrawPrimitive::Cone), 28u);
        EXPECT_EQ(ShaderDebugDrawContract::SegmentCount(ShaderDebugDrawPrimitive::Sphere), 96u);

        for (const auto primitive : kAllPrimitives)
        {
            EXPECT_EQ(ShaderDebugDrawContract::VertexCountPerInstance(primitive),
                      ShaderDebugDrawContract::SegmentCount(primitive) * 6u)
                << ShaderDebugDrawContract::Name(primitive);
        }
    }

    // -------------------------------------------------------------------------
    // The overflow protocol.
    //
    // Simulated rather than run on the GPU: the property being checked is the
    // ORDERING of the two atomics, which is a pure statement about the algorithm
    // and would be untestable headlessly if expressed only in GLSL. The
    // simulation is a line-for-line transcription of the helper bodies in
    // DebugDrawCommon.glsl.
    // -------------------------------------------------------------------------

    namespace
    {
        struct SimulatedChannel
        {
            u32 Capacity = 0;
            u32 RequestCount = 0;
            u32 InstanceCount = 0;
            std::vector<i32> Entries; // -1 == never written

            explicit SimulatedChannel(u32 capacity) : Capacity(capacity), Entries(capacity, -1) {}

            // Transcription of every OloDebugDraw*() helper in
            // DebugDrawCommon.glsl. Returns true if the push was accepted.
            bool Push(i32 payload)
            {
                if (Capacity == 0u)
                    return false;
                const u32 slot = RequestCount++;
                if (slot >= Capacity)
                    return false;
                Entries[slot] = payload;
                ++InstanceCount;
                return true;
            }
        };
    } // namespace

    TEST(ShaderDebugDrawContract, OverflowIsCountedNotSilentlyDropped)
    {
        SimulatedChannel channel(4);
        for (i32 i = 0; i < 10; ++i)
            channel.Push(i);

        // The excess is visible as the gap between the two counters — this is the
        // whole overflow flag. "I drew nothing" and "I overflowed" are the two
        // failure modes the issue calls out as indistinguishable without it.
        EXPECT_EQ(channel.RequestCount, 10u);
        EXPECT_EQ(channel.InstanceCount, 4u);

        ShaderDebugDrawChannelStats stats;
        stats.Capacity = channel.Capacity;
        stats.Drawn = channel.InstanceCount;
        stats.Requested = channel.RequestCount;
        EXPECT_TRUE(stats.Overflowed());
        EXPECT_EQ(stats.Dropped(), 6u);
    }

    TEST(ShaderDebugDrawContract, InstanceCountNeverExceedsTheEntriesActuallyWritten)
    {
        // The load-bearing safety property: the indirect draw reads
        // InstanceCount entries, so if InstanceCount could outrun the written
        // slots the vertex stage would read uninitialised memory and draw
        // garbage geometry — indistinguishable, on screen, from a real bug in
        // whatever shader was doing the pushing.
        for (const u32 capacity : { 0u, 1u, 3u, 8u })
        {
            SimulatedChannel channel(capacity);
            for (i32 i = 0; i < 20; ++i)
                channel.Push(i);

            EXPECT_LE(channel.InstanceCount, channel.Capacity) << "capacity " << capacity;
            EXPECT_EQ(channel.InstanceCount, std::min(channel.RequestCount, channel.Capacity))
                << "capacity " << capacity;
            for (u32 slot = 0; slot < channel.InstanceCount; ++slot)
            {
                EXPECT_NE(channel.Entries[slot], -1)
                    << "slot " << slot << " is covered by the indirect draw but was never written";
            }
        }
    }

    TEST(ShaderDebugDrawContract, ADisabledChannelAcceptsNothingAndCountsNothing)
    {
        // Capacity 0 is how the engine expresses "feature off". A push must not
        // even reach the atomic, or the disabled path is not free and the stats
        // would report phantom overflow on every frame the feature is off.
        SimulatedChannel channel(0);
        for (i32 i = 0; i < 100; ++i)
            EXPECT_FALSE(channel.Push(i));

        EXPECT_EQ(channel.RequestCount, 0u);
        EXPECT_EQ(channel.InstanceCount, 0u);

        ShaderDebugDrawChannelStats stats;
        stats.Capacity = 0;
        stats.Drawn = 0;
        stats.Requested = 0;
        EXPECT_FALSE(stats.Overflowed());
        EXPECT_EQ(stats.Dropped(), 0u);
    }

    TEST(ShaderDebugDrawContract, ExactlyFillingAChannelIsNotAnOverflow)
    {
        // The off-by-one that would cry wolf on every full frame.
        SimulatedChannel channel(5);
        for (i32 i = 0; i < 5; ++i)
            EXPECT_TRUE(channel.Push(i));

        ShaderDebugDrawChannelStats stats;
        stats.Capacity = 5;
        stats.Drawn = channel.InstanceCount;
        stats.Requested = channel.RequestCount;
        EXPECT_FALSE(stats.Overflowed());
        EXPECT_EQ(stats.Dropped(), 0u);
    }

    TEST(ShaderDebugDrawContract, StatsAggregateReportsOverflowFromAnyChannel)
    {
        ShaderDebugDrawStats stats;
        EXPECT_FALSE(stats.AnyOverflow());

        auto& coneChannel = stats.Channels[static_cast<u32>(std::to_underlying(ShaderDebugDrawPrimitive::Cone))];
        coneChannel.Capacity = 2;
        coneChannel.Requested = 3;
        coneChannel.Drawn = 2;
        EXPECT_TRUE(stats.AnyOverflow());
    }
} // namespace OloEngine::Tests
