// OLO_TEST_LAYER: unit
// =============================================================================
// VirtualLightmapUVPackingTest.cpp
//
// The virtual-geometry lightmap UV2 has no binding of its own — the SSBO
// namespace is full below Mesa's ceiling of 80, and SSBO_BONE_PULL (63) is
// resolved from the draw's VAO streams on Vulkan, which the mesh-shader route
// does not have. So it rides the cluster vertex arena as a PACKED TAIL, four
// uv2 pairs to a 32-byte element (issue #867, VirtualLightmapUVPacking.h).
//
// That makes the addressing a two-mirrors contract of exactly the kind
// LightmapPageEncoding already is: C++ packs, GLSL reads, and if the two ever
// disagree nothing errors — every virtual surface simply samples another mesh's
// charts, which renders as a plausible patch of light.
//
// So this file pins both halves: the C++ round-trip, and a TEXT SCAN proving
// the shader still carries a matching decode. The scan cannot prove the decode
// is correct (the GPU evidence test does that); it proves the shader has not
// quietly reverted to reading vertices directly, which is the one way this
// silently becomes wrong on a machine with no GPU.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Renderer/VirtualGeometry/VirtualLightmapUVPacking.h"

#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        [[nodiscard]] fs::path ShaderRoot()
        {
            // The test binary runs from the repo root; the visual suites run
            // from OloEditor/. Accept both so this file does not care.
            if (fs::exists("OloEditor/assets/shaders"))
            {
                return fs::path("OloEditor") / "assets" / "shaders";
            }
            return fs::path("assets") / "shaders";
        }

        [[nodiscard]] std::string ReadWholeFile(const fs::path& path)
        {
            std::ifstream file(path, std::ios::binary);
            if (!file)
            {
                return {};
            }
            std::ostringstream buffer;
            buffer << file.rdbuf();
            return buffer.str();
        }
    } // namespace

    TEST(VirtualLightmapUVPacking, EveryLaneRoundTrips)
    {
        // Four distinct uv2s in one element, so a lane that read its neighbour
        // (or always read lane 0) cannot pass.
        VirtualGpuVertex element{};
        const glm::vec2 uvs[4] = {
            { 0.125f, 0.25f },
            { 0.375f, 0.5f },
            { 0.625f, 0.75f },
            { 0.875f, 1.0f },
        };
        for (u32 lane = 0; lane < 4; ++lane)
        {
            PackVirtualLightmapUV(element, lane, uvs[lane]);
        }
        for (u32 lane = 0; lane < 4; ++lane)
        {
            const glm::vec2 read = UnpackVirtualLightmapUV(element, lane);
            // EXPECT_EQ on floats: this is a byte-shuffle, not arithmetic, so
            // bit-identity IS the contract — the same reason
            // LightmapPageEncodingTest compares exactly.
            EXPECT_EQ(read.x, uvs[lane].x) << "lane " << lane;
            EXPECT_EQ(read.y, uvs[lane].y) << "lane " << lane;
        }
    }

    TEST(VirtualLightmapUVPacking, LaneAndElementDeriveFromTheGlobalIndex)
    {
        // The whole scheme rests on lane == index & 3 and element == index >> 2,
        // with no per-page fixup — which is only sound because a page starts at
        // slot-local 0 and the slot capacity is 4-aligned. If either invariant
        // is ever broken, this is the arithmetic that stops matching.
        for (u32 index = 0; index < 64; ++index)
        {
            EXPECT_EQ(VirtualLightmapUVElementOffset(index), index / 4) << "index " << index;
        }
        EXPECT_EQ(VirtualLightmapUVElementCount(0), 0u);
        EXPECT_EQ(VirtualLightmapUVElementCount(1), 1u);
        EXPECT_EQ(VirtualLightmapUVElementCount(4), 1u);
        EXPECT_EQ(VirtualLightmapUVElementCount(5), 2u);
    }

    TEST(VirtualLightmapUVPacking, PackingAWholePageIsReadableByTheGlobalIndex)
    {
        // A page-shaped run: pack slot-locally from 0 (what LoadPage does), then
        // read back with the SAME index. This is the end-to-end shape of the
        // contract minus the GPU copy.
        constexpr u32 kVertexCount = 37; // deliberately not a multiple of 4
        std::vector<VirtualGpuVertex> arena(VirtualLightmapUVElementCount(kVertexCount));
        for (u32 v = 0; v < kVertexCount; ++v)
        {
            PackVirtualLightmapUV(arena[VirtualLightmapUVElementOffset(v)], v,
                                  glm::vec2(static_cast<f32>(v) * 0.03125f, static_cast<f32>(v) * 0.0625f));
        }
        for (u32 v = 0; v < kVertexCount; ++v)
        {
            const glm::vec2 read = UnpackVirtualLightmapUV(arena[VirtualLightmapUVElementOffset(v)], v);
            EXPECT_EQ(read.x, static_cast<f32>(v) * 0.03125f) << "vertex " << v;
            EXPECT_EQ(read.y, static_cast<f32>(v) * 0.0625f) << "vertex " << v;
        }
    }

    TEST(VirtualLightmapUVPacking, ShaderCarriesTheMatchingDecode)
    {
        const std::string src = ReadWholeFile(ShaderRoot() / "include" / "VirtualDrawInfo.glsl");
        ASSERT_FALSE(src.empty()) << "VirtualDrawInfo.glsl not found from " << fs::current_path().string();

        // The base word must still be a real field, not a pad again.
        EXPECT_NE(src.find("u_VirtualLightmapUVBase"), std::string::npos)
            << "the draw-info block no longer carries the uv2 tail's base element, so no reader can find it";

        // element = base + (index >> 2)
        EXPECT_TRUE(std::regex_search(src, std::regex(R"(u_VirtualLightmapUVBase\s*\+\s*\(\s*globalVertexIndex\s*>>\s*2u\s*\))")))
            << "the shader's element derivation drifted from VirtualLightmapUVElementOffset (index >> 2). "
               "Nothing errors when it does — every virtual surface reads a neighbouring mesh's charts.";

        // lane = index & 3, selecting (PositionU.xy, .zw, NormalV.xy, .zw)
        EXPECT_TRUE(std::regex_search(src, std::regex(R"(globalVertexIndex\s*&\s*3u)")))
            << "the shader's lane derivation drifted from `index & 3`";
        EXPECT_TRUE(std::regex_search(src, std::regex(R"(\(\s*lane\s*&\s*2u\s*\)\s*==\s*0u\s*\)\s*\?\s*elementLow\s*:\s*elementHigh)")))
            << "the shader no longer picks PositionU for lanes 0/1 and NormalV for lanes 2/3, which is the "
               "order PackVirtualLightmapUV writes";
        EXPECT_TRUE(std::regex_search(src, std::regex(R"(\(\s*lane\s*&\s*1u\s*\)\s*==\s*0u\s*\)\s*\?\s*pair\.xy\s*:\s*pair\.zw)")))
            << "the shader no longer picks .xy for even lanes and .zw for odd ones";
    }
} // namespace OloEngine::Tests
