// =============================================================================
// VRCSContractTest.cpp
//
// OLO_TEST_LAYER: L1
//
// Variable Rate Compute Shading (issue #683), CPU-side contract.
//
// VRCS's whole safety argument rests on a partition property: the classifier
// hands each 8x8 tile a footprint f in {1, 2, 4}, every consuming invocation
// asks "am I the leader of my f x f block?", and the leaders' broadcasts must
// cover every pixel EXACTLY ONCE. Break it one way and pixels keep whatever the
// previous frame's transient held; break it the other and two invocations race
// for the same texel. Neither shows up as a crash, and on a smooth surface
// neither shows up in a screenshot either — which is precisely why it is pinned
// here rather than left to the visual evidence.
//
// The other half of the file is a MIRROR CHECK, not a second implementation.
// Four numbers exist twice — the tile size, the three footprint encodings — once
// in C++ (ShadingRateClassifier) and once in GLSL (include/VRCS.glsl), plus the
// classification shader's local_size, which the C++ dispatch assumes equals the
// tile size when it launches one workgroup per tile. Every one of those
// disagreements is silent: a wrong tile size samples the rate image at the wrong
// texel, a wrong encoding decodes a valid footprint as full rate (slow but
// correct) or a full-rate tile as coarse (an artefact). So the test reads the
// SHADER TEXT and compares, rather than restating the constants.
//
// Deliberately NOT a CPU re-implementation of the classification thresholds.
// Two mirrors of a heuristic drift, and the drift would be invisible: a CPU
// mirror that disagrees with the shader fails nothing on the GPU. The thresholds
// are exercised against the real shader in VRCSClassifierGpuTest, which SKIPs
// without a GL 4.6 context; what lives here is only what is true independently
// of any driver.
// =============================================================================

#include "OloEnginePCH.h"

#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/VRCS/ShadingRateClassifier.h"
#include "ShaderHarness.h"

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <regex>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace SH = ShaderHarness;
        namespace fs = std::filesystem;

        // Pull `#define <name> <integer>` out of a GLSL source, tolerating the
        // `u` suffix the shader uses on unsigned literals. Returns -1 when the
        // define is absent, which the caller reports as a failure rather than
        // silently passing — an absent mirror is the same defect as a wrong one.
        [[nodiscard]] i64 ParseIntDefine(const std::string& source, const std::string& name)
        {
            const std::regex re("#define\\s+" + name + "\\s+([0-9]+)u?\\b");
            std::smatch m;
            if (!std::regex_search(source, m, re))
                return -1;
            return std::stoll(m[1].str());
        }

        [[nodiscard]] i64 ParseLocalSize(const std::string& source, const std::string& axis)
        {
            const std::regex re("local_size_" + axis + "\\s*=\\s*([0-9]+)");
            std::smatch m;
            if (!std::regex_search(source, m, re))
                return -1;
            return std::stoll(m[1].str());
        }

        [[nodiscard]] std::string ReadShader(const char* relativePath)
        {
            const fs::path root = SH::ResolveShaderRoot();
            if (root.empty())
                return {};
            return SH::ReadWholeFile(root / relativePath);
        }

        // The consumer-side predicate, transcribed from OloVRCSIsLeader in
        // include/VRCS.glsl. Transcribing it is safe in a way transcribing the
        // thresholds is not: it is three lines with no tunable in it, and the
        // partition properties below would fail on any transcription error.
        [[nodiscard]] constexpr bool IsLeader(u32 x, u32 y, u32 footprint) noexcept
        {
            return (x % footprint) == 0u && (y % footprint) == 0u;
        }
    } // namespace

    // -------------------------------------------------------------------------
    // The partition property, per footprint.
    // -------------------------------------------------------------------------
    TEST(VRCSContractTest, LeaderBroadcastsCoverEveryTilePixelExactlyOnce)
    {
        constexpr u32 kTile = ShadingRateClassifier::kTileSize;
        const std::array<u32, 3> footprints = { ShadingRateClassifier::kRate1x1,
                                                ShadingRateClassifier::kRate2x2,
                                                ShadingRateClassifier::kRate4x4 };

        for (u32 footprint : footprints)
        {
            SCOPED_TRACE("footprint " + std::to_string(footprint));

            // Two tiles across and down, so a leader whose block ran past the
            // tile boundary would show up as a double-write in the neighbour
            // rather than being masked by the single-tile case.
            constexpr u32 kSpan = kTile * 2u;
            std::vector<u32> writes(static_cast<std::size_t>(kSpan) * kSpan, 0u);

            for (u32 y = 0; y < kSpan; ++y)
            {
                for (u32 x = 0; x < kSpan; ++x)
                {
                    if (!IsLeader(x, y, footprint))
                        continue;
                    for (u32 dy = 0; dy < footprint; ++dy)
                    {
                        for (u32 dx = 0; dx < footprint; ++dx)
                        {
                            const u32 tx = x + dx;
                            const u32 ty = y + dy;
                            ASSERT_LT(tx, kSpan) << "leader block ran off the right edge";
                            ASSERT_LT(ty, kSpan) << "leader block ran off the bottom edge";
                            ++writes[static_cast<std::size_t>(ty) * kSpan + tx];
                        }
                    }
                }
            }

            for (u32 y = 0; y < kSpan; ++y)
            {
                for (u32 x = 0; x < kSpan; ++x)
                {
                    EXPECT_EQ(writes[static_cast<std::size_t>(y) * kSpan + x], 1u)
                        << "pixel (" << x << "," << y << ") written "
                        << writes[static_cast<std::size_t>(y) * kSpan + x]
                        << " times at footprint " << footprint
                        << " — 0 means a stale texel survives the frame, >1 means two invocations "
                           "race for it";
                }
            }
        }
    }

    // A footprint that does not divide the tile would put a leader's block
    // astride two tiles with different rates — the one case the leader/follower
    // agreement argument does not cover, because the follower would be reading a
    // different tile's footprint than its leader wrote.
    TEST(VRCSContractTest, EveryFootprintDividesTheTile)
    {
        constexpr u32 kTile = ShadingRateClassifier::kTileSize;
        EXPECT_EQ(kTile % ShadingRateClassifier::kRate1x1, 0u);
        EXPECT_EQ(kTile % ShadingRateClassifier::kRate2x2, 0u);
        EXPECT_EQ(kTile % ShadingRateClassifier::kRate4x4, 0u);
    }

    // The leader is the block's MINIMUM corner. That is what makes "if any pixel
    // of a footprint is inside the viewport, its leader is too" true, which is
    // what lets consumers bounds-check only the broadcast and not the dispatch.
    TEST(VRCSContractTest, LeaderIsTheMinimumCornerOfItsBlock)
    {
        const std::array<u32, 3> footprints = { 1u, 2u, 4u };
        for (u32 footprint : footprints)
        {
            for (u32 y = 0; y < ShadingRateClassifier::kTileSize; ++y)
            {
                for (u32 x = 0; x < ShadingRateClassifier::kTileSize; ++x)
                {
                    const u32 leaderX = x - (x % footprint);
                    const u32 leaderY = y - (y % footprint);
                    EXPECT_TRUE(IsLeader(leaderX, leaderY, footprint));
                    EXPECT_LE(leaderX, x);
                    EXPECT_LE(leaderY, y);
                }
            }
        }
    }

    // -------------------------------------------------------------------------
    // Tile-grid sizing.
    // -------------------------------------------------------------------------
    TEST(VRCSContractTest, TileGridCoversEveryPixelOfARaggedViewport)
    {
        // 1920/8 is exact; the others are not, and a floor-divide there would
        // leave the right/bottom strip unclassified — decoding as full rate,
        // so correct but permanently un-coarsened, which is the kind of "works,
        // just never helps" defect a saving-shaped measurement hides.
        const std::array<u32, 6> widths = { 1u, 7u, 8u, 9u, 1023u, 1920u };
        for (u32 w : widths)
        {
            const u32 tiles = ShadingRateClassifier::TileCountFor(w);
            EXPECT_GE(tiles * ShadingRateClassifier::kTileSize, w) << "width " << w;
            EXPECT_LT((tiles - 1u) * ShadingRateClassifier::kTileSize, w)
                << "width " << w << " — one more tile than needed";
        }
        EXPECT_EQ(ShadingRateClassifier::TileCountFor(0u), 0u);
    }

    // -------------------------------------------------------------------------
    // The mirrors.
    // -------------------------------------------------------------------------
    TEST(VRCSContractTest, ShaderConstantsMatchTheCppSide)
    {
        const std::string vrcs = ReadShader("include/VRCS.glsl");
        ASSERT_FALSE(vrcs.empty()) << "could not read include/VRCS.glsl from "
                                   << SH::ResolveShaderRoot().generic_string();

        EXPECT_EQ(ParseIntDefine(vrcs, "OLO_VRCS_TILE_SIZE"),
                  static_cast<i64>(ShadingRateClassifier::kTileSize))
            << "tile size disagrees — consumers would sample the rate image at the wrong texel";
        EXPECT_EQ(ParseIntDefine(vrcs, "OLO_VRCS_RATE_1X1"), static_cast<i64>(ShadingRateClassifier::kRate1x1));
        EXPECT_EQ(ParseIntDefine(vrcs, "OLO_VRCS_RATE_2X2"), static_cast<i64>(ShadingRateClassifier::kRate2x2));
        EXPECT_EQ(ParseIntDefine(vrcs, "OLO_VRCS_RATE_4X4"), static_cast<i64>(ShadingRateClassifier::kRate4x4));
    }

    // ShadingRateClassifier::Classify dispatches (tilesX, tilesY, 1) workgroups
    // — the tile grid itself, NOT a ceil-divide of it — because one workgroup is
    // one tile and one invocation is one pixel of it. If the shader's local_size
    // ever stopped equalling the tile size, that dispatch would silently cover
    // the wrong region: too small a group leaves most of each tile unsampled
    // (classification off partial data, biased toward "flat"), too large a one
    // reads neighbouring tiles into the reduction.
    TEST(VRCSContractTest, ClassifierWorkgroupIsExactlyOneTile)
    {
        const std::string comp = ReadShader("compute/VRCSClassify.comp");
        ASSERT_FALSE(comp.empty()) << "could not read compute/VRCSClassify.comp";

        EXPECT_EQ(ParseLocalSize(comp, "x"), static_cast<i64>(ShadingRateClassifier::kTileSize));
        EXPECT_EQ(ParseLocalSize(comp, "y"), static_cast<i64>(ShadingRateClassifier::kTileSize));
        EXPECT_EQ(ParseLocalSize(comp, "z"), 1);
    }

    // The classification params ride UBO_USER_0, the shared pass-local slot,
    // rather than a new binding — the engine has one uniform-buffer slot left
    // under the GL 4.6 minimum and #707 / #715 both recorded the decision not to
    // spend it. Pinned so a later "let's give VRCS its own binding" tidy-up is a
    // deliberate act rather than an unnoticed one.
    TEST(VRCSContractTest, ClassifierParamsRideThePassLocalUboSlot)
    {
        const std::string comp = ReadShader("compute/VRCSClassify.comp");
        ASSERT_FALSE(comp.empty());

        const std::regex re("layout\\(std140,\\s*binding\\s*=\\s*([0-9]+)\\)\\s*uniform\\s+ShadingRateParams");
        std::smatch m;
        ASSERT_TRUE(std::regex_search(comp, m, re))
            << "ShadingRateParams block not found in VRCSClassify.comp";
        EXPECT_EQ(std::stoul(m[1].str()), ShaderBindingLayout::UBO_USER_0);
    }
} // namespace OloEngine::Tests
